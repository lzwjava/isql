/* db.c — the minimal database engine. Tables are JSON objects:
 *
 *   {"name": "...",
 *    "columns": [{"name": "...", "type": "int|real|text", "primary": true}],
 *    "rows": [ {"col": value, ...}, ... ]}
 *
 * Queries arrive as JSON plans produced by the LLM parser (db_exec below).
 */
#include "db.h"
#include "wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ */
/* small helpers                                                     */
/* ================================================================ */

static const char *cell_text(const JsonValue *v, char *buf, int buflen)
{
    if (!v || v->type == JSON_NULL) return "NULL";
    if (v->type == JSON_STR) return v->str;
    if (v->type == JSON_BOOL) return v->boolean ? "true" : "false";
    if (v->type == JSON_NUM) {
        if (v->num == (double)(long long)v->num && (v->num < 9.0e18 && v->num > -9.0e18))
            snprintf(buf, buflen, "%lld", (long long)v->num);
        else
            snprintf(buf, buflen, "%g", v->num);
        return buf;
    }
    return "?";
}

/* numeric comparison when both sides look numeric, otherwise text comparison */
static int cell_cmp(const JsonValue *a, const JsonValue *b)
{
    double x, y;
    int ax = json_is_num_or_numeric_string(a, &x);
    int by = json_is_num_or_numeric_string(b, &y);
    if (ax && by) {
        if (x < y) return -1;
        if (x > y) return 1;
        return 0;
    }
    char ba[64], bb[64];
    const char *ta = cell_text(a, ba, sizeof ba);
    const char *tb = cell_text(b, bb, sizeof bb);
    return strcmp(ta, tb);
}

static JsonValue *find_table(Database *db, const char *name)
{
    if (!db->tables) return NULL;
    for (int i = 0; i < db->tables->nelems; i++) {
        JsonValue *t = db->tables->elems[i];
        JsonValue *n = t ? json_obj_get(t, "name") : NULL;
        if (n && n->type == JSON_STR && strcmp(n->str, name) == 0)
            return t;
    }
    return NULL;
}

static const char *col_name(const JsonValue *col)
{
    JsonValue *n = json_obj_get(col, "name");
    return (n && n->type == JSON_STR) ? n->str : "";
}

static const char *col_type(const JsonValue *col)
{
    JsonValue *n = json_obj_get(col, "type");
    return (n && n->type == JSON_STR) ? n->str : "";
}

static int col_is_pk(const JsonValue *col)
{
    JsonValue *p = json_obj_get(col, "primary");
    return p && p->type == JSON_BOOL && p->boolean;
}

static const JsonValue *cols_of(const JsonValue *tbl)
{
    return json_obj_get(tbl, "columns");
}

static JsonValue *rows_of(JsonValue *tbl)
{
    return json_obj_get(tbl, "rows");
}

static const char *table_name(const JsonValue *tbl)
{
    JsonValue *n = tbl ? json_obj_get(tbl, "name") : NULL;
    return (n && n->type == JSON_STR) ? n->str : "?";
}

/* resolve a column reference. "name" resolves to the key literally; if that
 * fails and the reference is qualified as "table.col", try the bare name
 * (joined rows expose right-table columns with a "table.col" prefix only
 * when the bare name collides). */
static JsonValue *row_get_qualified(const JsonValue *row, const char *spec)
{
    JsonValue *v = row ? json_obj_get(row, spec) : NULL;
    if (v) return v;
    const char *dot = strchr(spec, '.');
    if (dot && row) v = json_obj_get(row, dot + 1);
    return v;
}

/* SQL-style LIKE: '%' matches any sequence, '_' matches one character */
static int like_match(const char *text, const char *pat)
{
    const char *t = text, *p = pat;
    const char *star = NULL, *tt = NULL;
    while (*t) {
        if (*p == '%') { star = p++; tt = t; continue; }
        if (*p && (*p == '_' || *p == *t)) { p++; t++; continue; }
        if (star) { p = star + 1; t = ++tt; continue; }
        return 0;
    }
    while (*p == '%') p++;
    return *p == 0;
}

static int result_is_ok(const JsonValue *res)
{
    if (!res) return 0;
    JsonValue *ok = json_obj_get(res, "ok");
    return ok && ok->type == JSON_BOOL && ok->boolean;
}

/* ================================================================ */
/* results                                                           */
/* ================================================================ */

static JsonValue *result_ok(const char *msg, int affected)
{
    JsonValue *o = json_obj();
    json_obj_set(o, "ok", json_bool(1));
    json_obj_set(o, "message", json_str(msg));
    json_obj_set(o, "affected", json_num(affected));
    return o;
}

static JsonValue *result_rows(JsonValue *rows, JsonValue *columns, const char *msg)
{
    JsonValue *o = json_obj();
    json_obj_set(o, "ok", json_bool(1));
    json_obj_set(o, "message", json_str(msg));
    json_obj_set(o, "affected", json_num(rows ? rows->nelems : 0));
    if (columns) json_obj_set(o, "columns", columns);
    if (rows)    json_obj_set(o, "rows", rows);
    return o;
}

static JsonValue *result_err(const char *msg)
{
    JsonValue *o = json_obj();
    json_obj_set(o, "ok", json_bool(0));
    json_obj_set(o, "message", json_str(msg));
    return o;
}

/* ================================================================ */
/* WHERE evaluation                                                  */
/* ================================================================ */

static int pair_matches(const char *col, const JsonValue *cond, const JsonValue *row)
{
    JsonValue *cell = row ? row_get_qualified(row, col) : NULL;
    if (cond && cond->type == JSON_OBJ) {
        const char *ops[] = { "gt", "gte", "lt", "lte", "ne", "in", "like", "between", NULL };
        for (int i = 0; ops[i]; i++) {
            JsonValue *cv = json_obj_get(cond, ops[i]);
            if (!cv) continue;
            if (i <= 4) {
                int c = cell_cmp(cell, cv);
                switch (i) {
                case 0: return c > 0;   /* gt  */
                case 1: return c >= 0;  /* gte */
                case 2: return c < 0;   /* lt  */
                case 3: return c <= 0;  /* lte */
                default: return c != 0; /* ne  */
                }
            }
            if (i == 5) { /* in: cv is an array of candidate values */
                if (cv->type != JSON_ARR) return 0;
                for (int k = 0; k < cv->nelems; k++)
                    if (cell_cmp(cell, cv->elems[k]) == 0) return 1;
                return 0;
            }
            if (i == 6) { /* like: cv is a pattern using % and _ */
                if (cv->type != JSON_STR) return 0;
                if (!cell || cell->type == JSON_NULL) return 0;
                char buf[256];
                if (strchr(cv->str, '%') || strchr(cv->str, '_'))
                    return like_match(cell_text(cell, buf, sizeof buf), cv->str);
                return cell_cmp(cell, cv) == 0; /* no wildcard -> equality */
            }
            if (i == 7) { /* between: cv == [lo, hi] */
                if (cv->type != JSON_ARR || cv->nelems < 2) return 0;
                return cell_cmp(cell, cv->elems[0]) >= 0 && cell_cmp(cell, cv->elems[1]) <= 0;
            }
        }
    }
    /* plain value -> equality; missing cell is NULL */
    return cell_cmp(cell, cond) == 0;
}

static int cond_matches(const JsonValue *cond, const JsonValue *row)
{
    if (!cond || cond->type == JSON_NULL) return 1;
    if (cond->type != JSON_OBJ) return 0;

    JsonValue *andv = json_obj_get(cond, "and");
    JsonValue *orv  = json_obj_get(cond, "or");
    if (andv || orv) {
        if (andv) {
            if (andv->type != JSON_ARR) return 0;
            for (int i = 0; i < andv->nelems; i++)
                if (!cond_matches(andv->elems[i], row)) return 0;
        }
        if (orv) {
            if (orv->type != JSON_ARR) return 0;
            int any = 0;
            for (int i = 0; i < orv->nelems; i++)
                if (cond_matches(orv->elems[i], row)) { any = 1; break; }
            if (!any) return 0;
        }
        return 1;
    }
    for (int i = 0; i < cond->npairs; i++)
        if (!pair_matches(cond->pairs[i].key, cond->pairs[i].val, row))
            return 0;
    return 1;
}

/* ================================================================ */
/* type coercion / helpers                                           */
/* ================================================================ */

static char coerce_err_buf[256];

static JsonValue *coerce(const JsonValue *v, const char *type, const char *col, const char **err)
{
    *err = NULL;
    if (!v || v->type == JSON_NULL) return json_null();
    if (strcmp(type, "int") == 0 || strcmp(type, "real") == 0) {
        double d;
        if (!json_is_num_or_numeric_string(v, &d)) {
            snprintf(coerce_err_buf, sizeof coerce_err_buf,
                     "value for column '%s' is not numeric", col);
            *err = coerce_err_buf;
            return NULL;
        }
        return json_num(d);
    }
    if (v->type == JSON_STR) return json_str(v->str);
    {
        char tmp[64];
        return json_str(cell_text(v, tmp, sizeof tmp));
    }
}

static const JsonValue *find_column(const JsonValue *tbl, const char *name)
{
    const JsonValue *cols = cols_of(tbl);
    if (!cols) return NULL;
    for (int i = 0; i < cols->nelems; i++)
        if (strcmp(col_name(cols->elems[i]), name) == 0)
            return cols->elems[i];
    return NULL;
}

/* next value for an auto-increment primary key */
static JsonValue *next_pk(const JsonValue *tbl, const JsonValue *col)
{
    JsonValue *rows = rows_of((JsonValue *)tbl);
    double max = 0;
    int have = 0;
    const char *cname = col_name(col);
    for (int i = 0; i < rows->nelems; i++) {
        JsonValue *cell = json_obj_get(rows->elems[i], cname);
        if (cell && cell->type == JSON_NUM && (!have || cell->num > max)) {
            max = cell->num;
            have = 1;
        }
    }
    return json_num(have ? max + 1 : 1);
}

/* does any row (other than `skip`) already have this pk value? */
static const JsonValue *pk_conflict(const JsonValue *tbl, const char *cname, JsonValue *val, JsonValue *skip_row)
{
    JsonValue *rows = rows_of((JsonValue *)tbl);
    for (int i = 0; i < rows->nelems; i++) {
        JsonValue *row = rows->elems[i];
        if (row == skip_row) continue;
        JsonValue *cell = json_obj_get(row, cname);
        if (cell && cell->type != JSON_NULL && cell_cmp(cell, val) == 0)
            return row;
    }
    return NULL;
}

/* ================================================================ */
/* operations                                                        */
/* ================================================================ */

/* forward declarations: WAL helpers are implemented with the persistence code */
static const char *db_log(Database *db, JsonValue *effect);
const char *db_checkpoint(Database *db);

static JsonValue *op_create_table(Database *db, JsonValue *op)
{
    JsonValue *nv = json_obj_get(op, "table");
    if (!nv || nv->type != JSON_STR || nv->str[0] == '\0')
        return result_err("create_table: missing or empty 'table'");
    if (strpbrk(nv->str, " \t\"{}[]"))
        return result_err("create_table: invalid table name");
    if (find_table(db, nv->str))
        return result_err("create_table: table already exists");

    JsonValue *colsv = json_obj_get(op, "columns");
    if (!colsv || colsv->type != JSON_ARR || colsv->nelems == 0)
        return result_err("create_table: 'columns' must be a non-empty array");

    JsonValue *cols = json_arr();
    for (int i = 0; i < colsv->nelems; i++) {
        JsonValue *c = colsv->elems[i];
        if (!c || c->type != JSON_OBJ)
            return result_err("create_table: each column must be an object");
        JsonValue *cn = json_obj_get(c, "name");
        if (!cn || cn->type != JSON_STR || cn->str[0] == '\0')
            return result_err("create_table: column missing 'name'");
        JsonValue *ct = json_obj_get(c, "type");
        const char *type = (ct && ct->type == JSON_STR) ? ct->str : "text";
        if (strcmp(type, "int") && strcmp(type, "real") && strcmp(type, "text"))
            return result_err("create_table: unknown column type (use int, real or text)");
        for (int j = 0; j < cols->nelems; j++)
            if (strcmp(col_name(cols->elems[j]), cn->str) == 0)
                return result_err("create_table: duplicate column name");
        JsonValue *cp = json_obj_get(c, "primary");
        JsonValue *col = json_obj();
        json_obj_set(col, "name", json_str(cn->str));
        json_obj_set(col, "type", json_str(type));
        json_obj_set(col, "primary", json_bool(cp && cp->type == JSON_BOOL && cp->boolean));
        json_arr_push(cols, col);
    }

    JsonValue *tbl = json_obj();
    json_obj_set(tbl, "name", json_str(nv->str));
    json_obj_set(tbl, "columns", cols);
    json_obj_set(tbl, "rows", json_arr());
    json_arr_push(db->tables, tbl);

    JsonValue *eff = json_obj();
    json_obj_set(eff, "k", json_str("create"));
    json_obj_set(eff, "table", json_clone(tbl));
    const char *logerr = db_log(db, eff);
    if (logerr) return result_err(logerr);

    char msg[128];
    snprintf(msg, sizeof msg, "Created table '%s' (%d columns)", nv->str, cols->nelems);
    return result_ok(msg, 0);
}

static JsonValue *op_drop_table(Database *db, JsonValue *op)
{
    JsonValue *nv = json_obj_get(op, "table");
    if (!nv || nv->type != JSON_STR) return result_err("drop_table: missing 'table'");
    JsonValue *t = find_table(db, nv->str);
    if (!t) return result_err("drop_table: no such table");

    JsonValue *src = db->tables;
    int idx = -1;
    for (int i = 0; i < src->nelems; i++)
        if (src->elems[i] == t) { idx = i; break; }
    for (int i = idx; i < src->nelems - 1; i++)
        src->elems[i] = src->elems[i + 1];
    src->nelems--;
    json_free(t);

    JsonValue *eff = json_obj();
    json_obj_set(eff, "k", json_str("drop"));
    json_obj_set(eff, "t", json_str(nv->str));
    const char *logerr = db_log(db, eff);
    if (logerr) return result_err(logerr);

    char msg[96];
    snprintf(msg, sizeof msg, "Dropped table '%s'", nv->str);
    return result_ok(msg, 0);
}

static JsonValue *op_insert(Database *db, JsonValue *op)
{
    JsonValue *nv = json_obj_get(op, "table");
    if (!nv || nv->type != JSON_STR) return result_err("insert: missing 'table'");
    JsonValue *t = find_table(db, nv->str);
    if (!t) return result_err("insert: no such table");

    JsonValue *values = json_obj_get(op, "values");
    if (!values || values->type != JSON_OBJ)
        return result_err("insert: 'values' must be an object");

    JsonValue *cols = (JsonValue *)cols_of(t);
    JsonValue *row = json_obj();
    for (int i = 0; i < cols->nelems; i++) {
        const JsonValue *col = cols->elems[i];
        const char *cname = col_name(col);
        JsonValue *v = json_obj_get(values, cname);
        if (col_is_pk(col) && (!v || v->type == JSON_NULL))
            v = next_pk(t, col);
        const char *err = NULL;
        JsonValue *cv = coerce(v, col_type(col), cname, &err);
        if (err) { json_free(row); return result_err(err); }
        if (col_is_pk(col) && cv->type != JSON_NULL && pk_conflict(t, cname, cv, NULL))
            return result_err("insert: primary key already exists");
        json_obj_set(row, cname, cv);
    }
    json_arr_push(rows_of(t), row);

    JsonValue *eff = json_obj();
    json_obj_set(eff, "k", json_str("insert"));
    json_obj_set(eff, "t", json_str(table_name(t)));
    json_obj_set(eff, "row", json_clone(row));
    const char *logerr = db_log(db, eff);
    if (logerr) {
        /* roll the row back out of memory so the log stays authoritative */
        JsonValue *rows = rows_of(t);
        rows->nelems--;
        json_free(rows->elems[rows->nelems]);
        return result_err(logerr);
    }

    char msg[96];
    snprintf(msg, sizeof msg, "Inserted 1 row into '%s'", table_name(t));
    return result_ok(msg, 1);
}

/* returns a new array of *clones* of the rows matching `where`. The caller owns
 * the array and everything in it. */
static JsonValue *all_rows_owned(JsonValue *t)
{
    JsonValue *rows = rows_of(t);
    JsonValue *out = json_arr();
    for (int i = 0; i < rows->nelems; i++)
        json_arr_push(out, json_clone(rows->elems[i]));
    return out;
}

static JsonValue *filter_rows_owned(JsonValue *src, JsonValue *where)
{
    if (!where || where->type == JSON_NULL) return src; /* caller keeps ownership */
    JsonValue *out = json_arr();
    for (int i = 0; i < src->nelems; i++)
        if (cond_matches(where, src->elems[i]))
            json_arr_push(out, src->elems[i]);
    src->nelems = 0;
    json_free(src);
    return out;
}

/* stable insertion sort by the order_by entries (columns may be qualified) */
static void sort_rows(JsonValue *matched, JsonValue *ord)
{
    for (int i = 1; i < matched->nelems; i++) {
        JsonValue *key = matched->elems[i];
        int j = i - 1;
        while (j >= 0) {
            int cmp = 0, desc = 0;
            for (int k = 0; k < ord->nelems; k++) {
                JsonValue *o = ord->elems[k];
                if (!o || o->type != JSON_OBJ) continue;
                JsonValue *c = json_obj_get(o, "column");
                if (!c || c->type != JSON_STR) continue;
                JsonValue *dv = json_obj_get(o, "desc");
                desc = dv && dv->type == JSON_BOOL && dv->boolean;
                JsonValue *a = row_get_qualified(matched->elems[j], c->str);
                JsonValue *b = row_get_qualified(key, c->str);
                int cj = cell_cmp(a, b);
                if (cj != 0) { cmp = cj; break; }
            }
            if (cmp == 0) break;
            int keep = desc ? cmp < 0 : cmp > 0;
            if (!keep) break;
            matched->elems[j + 1] = matched->elems[j];
            j--;
        }
        matched->elems[j + 1] = key;
    }
}

/* ---------------------------------------------------------------- */
/* INNER JOIN                                                        */
/* ---------------------------------------------------------------- */

/* merge right into a clone of left; right columns whose bare name already
 * exists on the left are exposed as "<rtable>.<col>". */
static JsonValue *merge_rows(const JsonValue *left, const JsonValue *right, const char *rtable)
{
    JsonValue *out = json_clone(left);
    for (int i = 0; i < right->npairs; i++) {
        const char *k = right->pairs[i].key;
        char prefixed[256];
        if (json_obj_get(out, k)) {
            snprintf(prefixed, sizeof prefixed, "%s.%s", rtable, k);
            k = prefixed;
        }
        json_obj_set(out, k, json_clone(right->pairs[i].val));
    }
    return out;
}

/* resolve a join condition column reference: if it is qualified with
 * "table.", use that table's row; otherwise try the left row first, then the
 * right. The "on" map is symmetric, so either orientation works. */
static JsonValue *resolve_in(JsonValue *l, JsonValue *r, const char *lname, const char *spec)
{
    const char *dot = strchr(spec, '.');
    if (dot) {
        const char *qual = spec;
        const char *bare = dot + 1;
        /* qualifier is a byte-exact prefix of the table name */
        JsonValue *v;
        if (strncmp(qual, lname, (size_t)(dot - spec)) == 0 &&
            (strlen(lname) == (size_t)(dot - spec))) {
            v = json_obj_get(l, bare);
            if (v) return v;
            v = json_obj_get(r, bare);
            return v;
        }
        v = json_obj_get(r, bare);
        if (v) return v;
        return json_obj_get(l, bare);
    }
    JsonValue *v = json_obj_get(l, spec);
    if (v) return v;
    return json_obj_get(r, spec);
}

/* INNER JOIN: every (left row, right row) pair where ALL "on" equalities hold. */
static JsonValue *inner_join_rows(JsonValue *lt, JsonValue *rt, JsonValue *on)
{
    JsonValue *out = json_arr();
    const char *ltname = table_name(lt), *rtname = table_name(rt);
    JsonValue *lrows = rows_of(lt), *rrows = rows_of(rt);
    for (int li = 0; li < lrows->nelems; li++) {
        JsonValue *l = lrows->elems[li];
        for (int ri = 0; ri < rrows->nelems; ri++) {
            JsonValue *r = rrows->elems[ri];
            int ok = 1;
            for (int p = 0; p < on->npairs; p++) {
                JsonValue *rv = on->pairs[p].val;
                if (!rv || rv->type != JSON_STR) { ok = 0; break; }
                JsonValue *a = resolve_in(l, r, ltname, on->pairs[p].key);
                JsonValue *b = resolve_in(l, r, ltname, rv->str);
                if (cell_cmp(a, b) != 0) { ok = 0; break; }
            }
            if (ok) json_arr_push(out, merge_rows(l, r, rtname));
        }
    }
    return out;
}

/* column names visible on the joined rows (for validation / "*"), in order */
static JsonValue *joined_column_names(JsonValue *lt, JsonValue *rt)
{
    JsonValue *names = json_arr();
    const char *rtname = table_name(rt);
    JsonValue *lcols = (JsonValue *)cols_of(lt);
    JsonValue *rcols = (JsonValue *)cols_of(rt);
    for (int i = 0; i < lcols->nelems; i++)
        json_arr_push(names, json_str(col_name(lcols->elems[i])));
    for (int i = 0; i < rcols->nelems; i++) {
        const char *n = col_name(rcols->elems[i]);
        int dup = 0;
        for (int j = 0; j < names->nelems; j++)
            if (strcmp(names->elems[j]->str, n) == 0) { dup = 1; break; }
        if (dup) {
            char p[256];
            snprintf(p, sizeof p, "%s.%s", rtname, n);
            json_arr_push(names, json_str(p));
        } else {
            json_arr_push(names, json_str(n));
        }
    }
    return names;
}

static int name_known(JsonValue *names, const char *spec)
{
    for (int i = 0; i < names->nelems; i++)
        if (strcmp(names->elems[i]->str, spec) == 0) return 1;
    const char *dot = strchr(spec, '.');
    if (dot) {
        for (int i = 0; i < names->nelems; i++)
            if (strcmp(names->elems[i]->str, dot + 1) == 0) return 1;
    }
    return 0;
}

/* drop duplicate output rows (used for SELECT DISTINCT) */
static void dedupe_rows(JsonValue *rows)
{
    JsonValue *seen = json_arr();
    int w = 0;
    for (int r = 0; r < rows->nelems; r++) {
        char *key = json_to_string(rows->elems[r]);
        int dup = 0;
        for (int s = 0; s < seen->nelems; s++)
            if (strcmp(seen->elems[s]->str, key) == 0) { dup = 1; break; }
        if (dup) {
            json_free(rows->elems[r]);
        } else {
            json_arr_push(seen, json_str(key));
            rows->elems[w++] = rows->elems[r];
        }
        free(key);
    }
    rows->nelems = w;
    json_free(seen);
}

static JsonValue *op_select(Database *db, JsonValue *op)
{
    JsonValue *nv = json_obj_get(op, "table");
    if (!nv || nv->type != JSON_STR) return result_err("select: missing 'table'");
    JsonValue *t = find_table(db, nv->str);
    if (!t) return result_err("select: no such table");

    JsonValue *join = json_obj_get(op, "join");
    JsonValue *src = NULL;
    JsonValue *allnames = NULL;

    if (join && join->type == JSON_OBJ) {
        JsonValue *jn = json_obj_get(join, "table");
        if (!jn || jn->type != JSON_STR) return result_err("select: join is missing 'table'");
        JsonValue *jt = find_table(db, jn->str);
        if (!jt) return result_err("select: join table not found");
        JsonValue *on = json_obj_get(join, "on");
        if (!on || on->type != JSON_OBJ) return result_err("select: join requires an 'on' object");
        src = inner_join_rows(t, jt, on);
        allnames = joined_column_names(t, jt);
    } else {
        JsonValue *cols = (JsonValue *)cols_of(t);
        allnames = json_arr();
        for (int i = 0; i < cols->nelems; i++)
            json_arr_push(allnames, json_str(col_name(cols->elems[i])));
        src = all_rows_owned(t);
    }
    /* WHERE applies after the join (SQL semantics) */
    src = filter_rows_owned(src, json_obj_get(op, "where"));

    JsonValue *ord = json_obj_get(op, "order_by");
    if (ord && ord->type == JSON_ARR && ord->nelems > 0)
        sort_rows(src, ord);

    JsonValue *lim = json_obj_get(op, "limit");
    int limit = (lim && lim->type == JSON_NUM && lim->num >= 0) ? (int)lim->num : src->nelems;
    if (limit > src->nelems) limit = src->nelems;

    /* which columns to project */
    JsonValue *spec = json_obj_get(op, "columns");
    int all_cols = (!spec || (spec->type == JSON_STR && spec->str[0] == '*'));
    JsonValue *colnames = NULL;
    if (all_cols) {
        colnames = allnames; /* ownership moves into the result */
        allnames = NULL;
    } else {
        if (spec->type != JSON_ARR) {
            json_free(allnames); json_free(src);
            return result_err("select: 'columns' must be '*' or an array of names");
        }
        colnames = json_arr();
        for (int i = 0; i < spec->nelems; i++) {
            JsonValue *e = spec->elems[i];
            if (!e || e->type != JSON_STR || !name_known(allnames, e->str)) {
                json_free(allnames); json_free(colnames); json_free(src);
                return result_err("select: unknown column in 'columns'");
            }
            json_arr_push(colnames, json_str(e->str));
        }
        json_free(allnames);
    }

    /* materialize projected rows */
    JsonValue *rowobjs = json_arr();
    for (int i = 0; i < limit; i++) {
        JsonValue *srow = src->elems[i];
        JsonValue *r = json_obj();
        for (int j = 0; j < colnames->nelems; j++) {
            const char *cn = colnames->elems[j]->str;
            JsonValue *cell = row_get_qualified(srow, cn);
            json_obj_set(r, cn, cell ? json_clone(cell) : json_null());
        }
        json_arr_push(rowobjs, r);
    }
    json_free(src);

    JsonValue *dv = json_obj_get(op, "distinct");
    if (dv && dv->type == JSON_BOOL && dv->boolean)
        dedupe_rows(rowobjs);

    char msg[96];
    snprintf(msg, sizeof msg, "Fetched %d row(s) from '%s'", rowobjs->nelems, table_name(t));
    return result_rows(rowobjs, colnames, msg);
}

static JsonValue *op_update(Database *db, JsonValue *op)
{
    JsonValue *nv = json_obj_get(op, "table");
    if (!nv || nv->type != JSON_STR) return result_err("update: missing 'table'");
    JsonValue *t = find_table(db, nv->str);
    if (!t) return result_err("update: no such table");

    JsonValue *values = json_obj_get(op, "values");
    if (!values || values->type != JSON_OBJ)
        return result_err("update: 'values' must be an object");
    for (int i = 0; i < values->npairs; i++)
        if (!find_column(t, values->pairs[i].key))
            return result_err("update: unknown column in 'values'");

    JsonValue *where = json_obj_get(op, "where");
    JsonValue *rows = rows_of(t);
    JsonValue *post = json_arr();
    int n = 0;
    for (int i = 0; i < rows->nelems; i++) {
        JsonValue *row = rows->elems[i];
        if (!cond_matches(where, row)) continue;
        for (int v = 0; v < values->npairs; v++) {
            const char *cname = values->pairs[v].key;
            const JsonValue *col = find_column(t, cname);
            const char *err = NULL;
            JsonValue *cv = coerce(values->pairs[v].val, col_type(col), cname, &err);
            if (err) { json_free(post); return result_err(err); }
            if (col_is_pk(col) && cv->type != JSON_NULL && pk_conflict(t, cname, cv, row))
                return result_err("update: primary key conflict");
            json_obj_set(row, cname, cv);
        }
        json_arr_push(post, json_clone(row)); /* durable post-image */
        n++;
    }
    if (n > 0) {
        JsonValue *eff = json_obj();
        json_obj_set(eff, "k", json_str("update"));
        json_obj_set(eff, "t", json_str(table_name(t)));
        json_obj_set(eff, "rows", post);
        const char *logerr = db_log(db, eff);
        if (logerr) return result_err(logerr);
    } else {
        json_free(post);
    }
    char msg[96];
    snprintf(msg, sizeof msg, "Updated %d row(s) in '%s'", n, table_name(t));
    return result_ok(msg, n);
}

static JsonValue *op_delete(Database *db, JsonValue *op)
{
    JsonValue *nv = json_obj_get(op, "table");
    if (!nv || nv->type != JSON_STR) return result_err("delete: missing 'table'");
    JsonValue *t = find_table(db, nv->str);
    if (!t) return result_err("delete: no such table");

    JsonValue *where = json_obj_get(op, "where");
    JsonValue *rows = rows_of(t);
    JsonValue *pre = json_arr();
    JsonValue *kept = json_arr();
    int removed = 0;
    for (int i = 0; i < rows->nelems; i++) {
        if (cond_matches(where, rows->elems[i])) {
            json_arr_push(pre, json_clone(rows->elems[i])); /* durable pre-image */
            removed++;
        } else {
            json_arr_push(kept, rows->elems[i]);
        }
    }
    rows->nelems = 0;
    for (int i = 0; i < kept->nelems; i++)
        json_arr_push(rows, kept->elems[i]);
    kept->nelems = 0;
    json_free(kept);

    if (removed > 0) {
        JsonValue *eff = json_obj();
        json_obj_set(eff, "k", json_str("delete"));
        json_obj_set(eff, "t", json_str(table_name(t)));
        json_obj_set(eff, "rows", pre);
        const char *logerr = db_log(db, eff);
        if (logerr) return result_err(logerr);
    } else {
        json_free(pre);
    }

    char msg[96];
    snprintf(msg, sizeof msg, "Deleted %d row(s) from '%s'", removed, table_name(t));
    return result_ok(msg, removed);
}

static JsonValue *op_show_tables(Database *db, JsonValue *op)
{
    (void)op;
    JsonValue *columns = json_arr();
    json_arr_push(columns, json_str("table"));
    JsonValue *rowobjs = json_arr();
    for (int i = 0; i < db->tables->nelems; i++) {
        JsonValue *r = json_obj();
        json_obj_set(r, "table", json_str(table_name(db->tables->elems[i])));
        json_arr_push(rowobjs, r);
    }
    char msg[64];
    snprintf(msg, sizeof msg, "%d table(s)", rowobjs->nelems);
    return result_rows(rowobjs, columns, msg);
}

static JsonValue *op_describe(Database *db, JsonValue *op)
{
    JsonValue *nv = json_obj_get(op, "table");
    if (!nv || nv->type != JSON_STR) return result_err("describe: missing 'table'");
    JsonValue *t = find_table(db, nv->str);
    if (!t) return result_err("describe: no such table");

    JsonValue *columns = json_arr();
    json_arr_push(columns, json_str("column"));
    json_arr_push(columns, json_str("type"));
    json_arr_push(columns, json_str("primary"));

    JsonValue *cols = (JsonValue *)cols_of(t);
    JsonValue *rowobjs = json_arr();
    for (int i = 0; i < cols->nelems; i++) {
        JsonValue *r = json_obj();
        json_obj_set(r, "column", json_str(col_name(cols->elems[i])));
        json_obj_set(r, "type", json_str(col_type(cols->elems[i])));
        json_obj_set(r, "primary", json_bool(col_is_pk(cols->elems[i])));
        json_arr_push(rowobjs, r);
    }
    char msg[96];
    snprintf(msg, sizeof msg, "Table '%s': %d rows", table_name(t), rows_of(t)->nelems);
    return result_rows(rowobjs, columns, msg);
}

/* ================================================================ */
/* AGGREGATE                                                          */
/* ================================================================ */

static JsonValue *compute_agg(JsonValue *grows, JsonValue *agg)
{
    JsonValue *fn = json_obj_get(agg, "func");
    JsonValue *colv = json_obj_get(agg, "column");
    if (!fn || fn->type != JSON_STR) return json_null();
    const char *func = fn->str;
    const char *col = (colv && colv->type == JSON_STR) ? colv->str : "*";

    if (strcmp(func, "count") == 0) {
        if (col[0] == '*') return json_num(grows->nelems);
        int n = 0;
        for (int i = 0; i < grows->nelems; i++) {
            JsonValue *cell = row_get_qualified(grows->elems[i], col);
            if (cell && cell->type != JSON_NULL) n++;
        }
        return json_num(n);
    }
    if (strcmp(func, "sum") == 0 || strcmp(func, "avg") == 0) {
        double total = 0;
        int n = 0;
        for (int i = 0; i < grows->nelems; i++) {
            double d;
            JsonValue *cell = col[0] == '*' ? NULL : row_get_qualified(grows->elems[i], col);
            if (cell && json_is_num_or_numeric_string(cell, &d)) { total += d; n++; }
        }
        return json_num(strcmp(func, "avg") == 0 ? (n ? total / n : 0) : total);
    }
    if (strcmp(func, "min") == 0 || strcmp(func, "max") == 0) {
        JsonValue *best = NULL;
        for (int i = 0; i < grows->nelems; i++) {
            JsonValue *cell = row_get_qualified(grows->elems[i], col);
            if (!cell || cell->type == JSON_NULL) continue;
            if (!best || (strcmp(func, "max") == 0 ? cell_cmp(cell, best) > 0 : cell_cmp(cell, best) < 0))
                best = cell;
        }
        return best ? json_clone(best) : json_null();
    }
    return json_null();
}

static JsonValue *op_aggregate(Database *db, JsonValue *op)
{
    JsonValue *nv = json_obj_get(op, "table");
    if (!nv || nv->type != JSON_STR) return result_err("aggregate: missing 'table'");
    JsonValue *t = find_table(db, nv->str);
    if (!t) return result_err("aggregate: no such table");

    JsonValue *aggs = json_obj_get(op, "aggs");
    if (!aggs || aggs->type != JSON_ARR || aggs->nelems == 0)
        return result_err("aggregate: 'aggs' (array) is required");
    for (int i = 0; i < aggs->nelems; i++) {
        JsonValue *a = aggs->elems[i];
        JsonValue *fn = a ? json_obj_get(a, "func") : NULL;
        if (!fn || fn->type != JSON_STR) return result_err("aggregate: each agg needs a 'func' string");
    }

    JsonValue *gb = json_obj_get(op, "group_by");
    int has_gb = gb && gb->type == JSON_ARR && gb->nelems > 0;
    if (has_gb) {
        for (int i = 0; i < gb->nelems; i++) {
            JsonValue *g = gb->elems[i];
            if (!g || g->type != JSON_STR || !find_column(t, g->str))
                return result_err("aggregate: unknown column in 'group_by'");
        }
    }

    JsonValue *rows = filter_rows_owned(all_rows_owned(t), json_obj_get(op, "where"));

    /* group rows by the group-by values (keep encounter order) */
    JsonValue *groups = json_arr(); /* elements: {key: str, rows: [clones]} */
    for (int i = 0; i < rows->nelems; i++) {
        char keybuf[1024];
        keybuf[0] = 0;
        if (has_gb) {
            for (int g = 0; g < gb->nelems; g++) {
                const char *gn = gb->elems[g]->str;
                JsonValue *cell = json_obj_get(rows->elems[i], gn);
                char b[64];
                if (g) strcat(keybuf, "\x1f");
                strncat(keybuf, cell ? cell_text(cell, b, sizeof b) : "NULL", sizeof keybuf - strlen(keybuf) - 1);
            }
        }
        JsonValue *grp = NULL;
        for (int g = 0; g < groups->nelems; g++) {
            JsonValue *gk = json_obj_get(groups->elems[g], "key");
            if (gk && gk->type == JSON_STR && strcmp(gk->str, keybuf) == 0) { grp = groups->elems[g]; break; }
        }
        if (!grp) {
            grp = json_obj();
            json_obj_set(grp, "key", json_str(has_gb ? keybuf : ""));
            json_obj_set(grp, "rows", json_arr());
            json_arr_push(groups, grp);
        }
        json_arr_push(json_obj_get(grp, "rows"), rows->elems[i]);
    }
    rows->nelems = 0;
    json_free(rows);

    /* if no rows at all and no groups, still emit one group so aggregate returns a row */
    if (groups->nelems == 0) {
        JsonValue *grp = json_obj();
        json_obj_set(grp, "key", json_str(""));
        json_obj_set(grp, "rows", json_arr());
        json_arr_push(groups, grp);
    }

    /* output columns: group columns then each agg's "as" (fallback: func) */
    JsonValue *colnames = json_arr();
    if (has_gb)
        for (int i = 0; i < gb->nelems; i++)
            json_arr_push(colnames, json_str(gb->elems[i]->str));
    for (int i = 0; i < aggs->nelems; i++) {
        JsonValue *asv = json_obj_get(aggs->elems[i], "as");
        JsonValue *fn = json_obj_get(aggs->elems[i], "func");
        const char *nm = (asv && asv->type == JSON_STR) ? asv->str : fn->str;
        json_arr_push(colnames, json_str(nm));
    }

    JsonValue *rowobjs = json_arr();
    for (int g = 0; g < groups->nelems; g++) {
        JsonValue *grp = groups->elems[g];
        JsonValue *grows = json_obj_get(grp, "rows");
        JsonValue *row = json_obj();
        if (has_gb) {
            JsonValue *rep = grows->nelems > 0 ? grows->elems[0] : NULL;
            for (int i = 0; i < gb->nelems; i++) {
                const char *gn = gb->elems[i]->str;
                JsonValue *cell = rep ? json_obj_get(rep, gn) : NULL;
                json_obj_set(row, gn, cell ? json_clone(cell) : json_null());
            }
        }
        for (int i = 0; i < aggs->nelems; i++) {
            JsonValue *asv = json_obj_get(aggs->elems[i], "as");
            JsonValue *fn = json_obj_get(aggs->elems[i], "func");
            const char *nm = (asv && asv->type == JSON_STR) ? asv->str : fn->str;
            json_obj_set(row, nm, compute_agg(grows, aggs->elems[i]));
        }
        json_arr_push(rowobjs, row);
        json_free(grp); /* groups no longer owns it: see detach below */
    }
    groups->nelems = 0; /* group objects already freed above */
    json_free(groups);

    char msg[96];
    snprintf(msg, sizeof msg, "Aggregated %d group(s)", rowobjs->nelems);
    return result_rows(rowobjs, colnames, msg);
}

/* ================================================================ */
/* ALTER TABLE                                                       */
/* ================================================================ */

/* shared by op_alter_table and WAL replay (replay must not re-log) */
static const char *alter_add_column(JsonValue *t, JsonValue *cv)
{
    JsonValue *cn = json_obj_get(cv, "name");
    if (!cn || cn->type != JSON_STR || cn->str[0] == 0) return "alter_table: column missing 'name'";
    if (find_column(t, cn->str)) return "alter_table: column already exists";
    JsonValue *ct = json_obj_get(cv, "type");
    const char *ctype = (ct && ct->type == JSON_STR) ? ct->str : "text";
    if (strcmp(ctype, "int") && strcmp(ctype, "real") && strcmp(ctype, "text"))
        return "alter_table: unknown column type";
    JsonValue *col = json_obj();
    json_obj_set(col, "name", json_str(cn->str));
    json_obj_set(col, "type", json_str(ctype));
    json_obj_set(col, "primary", json_bool(0));
    json_arr_push(json_obj_get(t, "columns"), col);
    return NULL;
}

static const char *alter_drop_column(JsonValue *t, const char *drop)
{
    if (!find_column(t, drop)) return "alter_table: no such column";
    JsonValue *cols = json_obj_get(t, "columns");
    JsonValue *newcols = json_arr();
    for (int i = 0; i < cols->nelems; i++) {
        if (strcmp(col_name(cols->elems[i]), drop) == 0) continue;
        json_arr_push(newcols, json_clone(cols->elems[i]));
    }
    json_obj_set(t, "columns", newcols); /* replaces; old columns freed by json_obj_set */
    JsonValue *rows = rows_of(t);
    for (int i = 0; i < rows->nelems; i++) {
        JsonValue *old = rows->elems[i];
        JsonValue *nr = json_obj();
        for (int p = 0; p < old->npairs; p++)
            if (strcmp(old->pairs[p].key, drop) != 0)
                json_obj_set(nr, old->pairs[p].key, json_clone(old->pairs[p].val));
        json_free(old);
        rows->elems[i] = nr;
    }
    return NULL;
}

static JsonValue *op_alter_table(Database *db, JsonValue *op)
{
    JsonValue *nv = json_obj_get(op, "table");
    if (!nv || nv->type != JSON_STR) return result_err("alter_table: missing 'table'");
    JsonValue *t = find_table(db, nv->str);
    if (!t) return result_err("alter_table: no such table");

    JsonValue *av = json_obj_get(op, "action");
    const char *action = (av && av->type == JSON_STR) ? av->str : "";
    const char *err = NULL;

    if (strcmp(action, "add_column") == 0) {
        JsonValue *cv = json_obj_get(op, "column");
        if (!cv || cv->type != JSON_OBJ) return result_err("alter_table: 'column' object required");
        err = alter_add_column(t, cv);
        if (err) return result_err(err);
        JsonValue *eff = json_obj();
        json_obj_set(eff, "k", json_str("alter"));
        json_obj_set(eff, "t", json_str(table_name(t)));
        json_obj_set(eff, "a", json_str("add_column"));
        json_obj_set(eff, "c", json_clone(cv));
        err = db_log(db, eff);
        if (err) return result_err(err);
        char msg[96];
        JsonValue *cn = json_obj_get(cv, "name");
        snprintf(msg, sizeof msg, "Added column '%s' to '%s'", cn->str, table_name(t));
        return result_ok(msg, 0);
    }

    if (strcmp(action, "drop_column") == 0) {
        JsonValue *cn = json_obj_get(op, "column");
        const char *drop = (cn && cn->type == JSON_STR) ? cn->str : "";
        err = alter_drop_column(t, drop);
        if (err) return result_err(err);
        JsonValue *eff = json_obj();
        json_obj_set(eff, "k", json_str("alter"));
        json_obj_set(eff, "t", json_str(table_name(t)));
        json_obj_set(eff, "a", json_str("drop_column"));
        json_obj_set(eff, "c", json_str(drop));
        err = db_log(db, eff);
        if (err) return result_err(err);
        char msg[96];
        snprintf(msg, sizeof msg, "Dropped column '%s' from '%s'", drop, table_name(t));
        return result_ok(msg, 0);
    }

    return result_err("alter_table: 'action' must be add_column or drop_column");
}

/* ================================================================ */
/* BATCH (all-or-nothing)                                            */
/* ================================================================ */

static JsonValue *op_batch(Database *db, JsonValue *ops)
{
    if (ops->nelems == 0) return result_ok("Nothing to execute", 0);
    /* WAL lines written by the sub-ops are buffered; they hit disk only on
     * commit, so a batch is atomic in the log too. */
    if (db->wal) wal_begin(db->wal);
    JsonValue *snapshot = json_clone(db->tables);
    JsonValue *items = json_arr();
    for (int i = 0; i < ops->nelems; i++) {
        JsonValue *res = db_exec(db, ops->elems[i]);
        json_arr_push(items, res);
        if (!result_is_ok(res)) {
            if (db->wal) wal_rollback(db->wal);
            /* roll back: discard the mutated tables, restore the snapshot
             * (snapshot is a deep clone, so freeing the mutated tree is safe) */
            json_free(db->tables);
            db->tables = snapshot;
            JsonValue *out = json_obj();
            json_obj_set(out, "ok", json_bool(0));
            json_obj_set(out, "rolled_back", json_bool(1));
            {
                char msg[96];
                snprintf(msg, sizeof msg, "Batch failed at op %d of %d - all changes rolled back", i + 1, ops->nelems);
                json_obj_set(out, "message", json_str(msg));
            }
            json_obj_set(out, "items", items);
            return out;
        }
    }
    if (db->wal) {
        const char *cerr = wal_commit(db->wal);
        if (cerr) return result_err(cerr);
    }
    json_free(snapshot);
    JsonValue *out = json_obj();
    json_obj_set(out, "ok", json_bool(1));
    {
        char msg[96];
        snprintf(msg, sizeof msg, "Executed %d op(s) in a batch", ops->nelems);
        json_obj_set(out, "message", json_str(msg));
    }
    json_obj_set(out, "items", items);
    return out;
}

/* ================================================================ */
/* dispatcher                                                        */
/* ================================================================ */

JsonValue *db_exec(Database *db, JsonValue *op)
{
    if (!op || op->type != JSON_OBJ)
        return result_err("bad query plan: expected a JSON object");
    JsonValue *opv = json_obj_get(op, "op");
    if (!opv || opv->type != JSON_STR) {
        /* a plan with an "ops" array is a batch */
        JsonValue *opsv = json_obj_get(op, "ops");
        if (opsv && opsv->type == JSON_ARR) return op_batch(db, opsv);
        return result_err("bad query plan: missing string field 'op'");
    }
    const char *name = opv->str;

    if (strcmp(name, "create_table")  == 0) return op_create_table(db, op);
    if (strcmp(name, "drop_table")    == 0) return op_drop_table(db, op);
    if (strcmp(name, "insert")        == 0) return op_insert(db, op);
    if (strcmp(name, "select")        == 0) return op_select(db, op);
    if (strcmp(name, "update")        == 0) return op_update(db, op);
    if (strcmp(name, "delete")        == 0) return op_delete(db, op);
    if (strcmp(name, "aggregate")     == 0) return op_aggregate(db, op);
    if (strcmp(name, "alter_table")   == 0) return op_alter_table(db, op);
    if (strcmp(name, "show_tables")   == 0) return op_show_tables(db, op);
    if (strcmp(name, "describe")      == 0) return op_describe(db, op);
    if (strcmp(name, "error")        == 0) {
        JsonValue *m = json_obj_get(op, "message");
        return result_err((m && m->type == JSON_STR) ? m->str
                                                     : "the LLM could not interpret the query");
    }
    return result_err("unknown operation");
}

/* ================================================================ */
/* persistence                                                       */
/* ================================================================ */

void db_init(Database *db, const char *filename)
{
    db->filename = filename ? strdup(filename) : NULL;
    db->tables = json_arr();
    db->wal = NULL;
    db->wal_seq = 0;
}

const char *db_load(Database *db)
{
    if (!db->filename) return NULL;
    FILE *f = fopen(db->filename, "r");
    if (!f) return NULL; /* new database */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[sz] = '\0';

    JsonValue *root = json_parse(buf);
    free(buf);
    if (!root) return "corrupt database file: invalid JSON";

    JsonValue *tables = NULL;
    if (root->type == JSON_OBJ) {
        JsonValue *t = json_obj_get(root, "tables");
        if (t && t->type == JSON_ARR) tables = t;
    }
    if (!tables) { json_free(root); return "corrupt database file: missing tables array"; }
    /* snapshot coverage marker: highest WAL seq included in this file */
    db->wal_seq = 0;
    if (root->type == JSON_OBJ) {
        JsonValue *sv = json_obj_get(root, "seq");
        if (sv && sv->type == JSON_NUM) db->wal_seq = (long)sv->num;
    }

    /* transfer the table pointers out of `root` */
    if (db->tables) { db->tables->nelems = 0; json_free(db->tables); }
    JsonValue *out = json_arr();
    for (int i = 0; i < tables->nelems; i++)
        json_arr_push(out, tables->elems[i]);
    tables->nelems = 0;
    db->tables = out;
    json_free(root);
    return NULL;
}

const char *db_save(Database *db)
{
    if (!db->filename) return NULL;
    JsonValue *root = json_obj();
    json_obj_set(root, "tables", db->tables);
    json_obj_set(root, "seq", json_num(db->wal_seq));
    char *json = json_to_string(root);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", db->filename);
    FILE *f = fopen(tmp, "w");
    if (!f) { free(json); return "cannot write database file"; }
    fputs(json, f);
    fclose(f);
    if (rename(tmp, db->filename) != 0) { free(json); return "cannot rename database file"; }
    free(json);
    return NULL;
}

/* ================================================================ */
/* WAL integration                                                   */
/* ================================================================ */

/* log the effect of a mutation; returns an error string or NULL.
 * The effect is a JSON object {"k":..., ...} with the sequence stamped in. */
static const char *db_log(Database *db, JsonValue *effect)
{
    if (!db->wal) { json_free(effect); return NULL; }
    db->wal_seq++;
    json_obj_set(effect, "s", json_num(db->wal_seq));
    char *line = json_to_string(effect);
    json_free(effect);
    const char *err = wal_append(db->wal, line);
    free(line);
    if (err) return err;
    /* periodic in-process checkpoint keeps the log from growing unbounded */
    if (wal_entries(db->wal) > 5000) {
        const char *cerr = db_checkpoint(db);
        if (cerr) return cerr;
    }
    return NULL;
}

const char *db_checkpoint(Database *db)
{
    if (db->wal) return wal_checkpoint(db->wal, db);
    return db_save(db);
}

const char *db_attach_wal(Database *db)
{
    if (!db->filename) return NULL;
    if (db->wal) return NULL;
    db->wal = wal_open(db->filename);
    if (!db->wal) return "WAL: cannot open (falling back to whole-file saves)";
    return wal_replay(db->wal, db);
}

/* ---------- replay application ---------- */

static int remove_table_preserve(Database *db, const char *name)
{
    int idx = -1;
    for (int i = 0; i < db->tables->nelems; i++) {
        if (strcmp(table_name(db->tables->elems[i]), name) == 0) { idx = i; break; }
    }
    if (idx < 0) return 0;
    for (int i = idx; i < db->tables->nelems - 1; i++)
        db->tables->elems[i] = db->tables->elems[i + 1];
    db->tables->nelems--;
    return 1;
}

/* match a row by deep equality (rows were logged as pre/post images) */
static int find_equal_row(JsonValue *t, const JsonValue *target)
{
    JsonValue *rows = rows_of(t);
    for (int i = 0; i < rows->nelems; i++)
        if (json_equal(rows->elems[i], target)) return i;
    return -1;
}

const char *db_wal_apply(Database *db, JsonValue *entry)
{
    JsonValue *sv = json_obj_get(entry, "s");
    if (sv && sv->type == JSON_NUM && (long)sv->num <= db->wal_seq)
        return NULL; /* already covered by the snapshot */
    if (sv && sv->type == JSON_NUM) db->wal_seq = (long)sv->num;

    JsonValue *kv = json_obj_get(entry, "k");
    if (!kv || kv->type != JSON_STR) return "corrupt WAL: entry missing kind";
    const char *kind = kv->str;

    if (strcmp(kind, "create") == 0) {
        JsonValue *tbl = json_obj_get(entry, "table");
        if (!tbl || tbl->type != JSON_OBJ) return "corrupt WAL: create entry";
        JsonValue *n = json_obj_get(tbl, "name");
        const char *nm = (n && n->type == JSON_STR) ? n->str : "";
        remove_table_preserve(db, nm);
        json_arr_push(db->tables, json_clone(tbl));
        return NULL;
    }
    if (strcmp(kind, "drop") == 0) {
        JsonValue *tv = json_obj_get(entry, "t");
        if (!tv || tv->type != JSON_STR) return "corrupt WAL: drop entry";
        JsonValue *t = find_table(db, tv->str);
        if (!t) return NULL; /* idempotent */
        remove_table_preserve(db, tv->str);
        json_free(t);
        return NULL;
    }
    if (strcmp(kind, "insert") == 0) {
        JsonValue *tv = json_obj_get(entry, "t");
        JsonValue *row = json_obj_get(entry, "row");
        if (!tv || tv->type != JSON_STR || !row || row->type != JSON_OBJ)
            return "corrupt WAL: insert entry";
        JsonValue *t = find_table(db, tv->str);
        if (!t) return "corrupt WAL: insert into missing table";
        json_arr_push(rows_of(t), json_clone(row));
        return NULL;
    }
    if (strcmp(kind, "update") == 0 || strcmp(kind, "delete") == 0) {
        JsonValue *tv = json_obj_get(entry, "t");
        JsonValue *rowsv = json_obj_get(entry, "rows");
        if (!tv || tv->type != JSON_STR || !rowsv || rowsv->type != JSON_ARR)
            return "corrupt WAL: update/delete entry";
        JsonValue *t = find_table(db, tv->str);
        if (!t) return "corrupt WAL: table missing";
        JsonValue *rows = rows_of(t);
        for (int i = 0; i < rowsv->nelems; i++) {
            int idx = find_equal_row(t, rowsv->elems[i]);
            if (strcmp(kind, "delete") == 0) {
                if (idx >= 0) {
                    json_free(rows->elems[idx]);
                    for (int j = idx; j < rows->nelems - 1; j++)
                        rows->elems[j] = rows->elems[j + 1];
                    rows->nelems--;
                }
            } else { /* update: replace with the logged post-image */
                if (idx >= 0) {
                    json_free(rows->elems[idx]);
                    rows->elems[idx] = json_clone(rowsv->elems[i]);
                } else {
                    json_arr_push(rows, json_clone(rowsv->elems[i]));
                }
            }
        }
        return NULL;
    }
    if (strcmp(kind, "alter") == 0) {
        JsonValue *tv = json_obj_get(entry, "t");
        JsonValue *av = json_obj_get(entry, "a");
        if (!tv || tv->type != JSON_STR || !av || av->type != JSON_STR)
            return "corrupt WAL: alter entry";
        JsonValue *t = find_table(db, tv->str);
        if (!t) return NULL; /* idempotent on missing table */
        if (strcmp(av->str, "add_column") == 0) {
            JsonValue *cv = json_obj_get(entry, "c");
            if (!cv || cv->type != JSON_OBJ) return "corrupt WAL: alter add column";
            return alter_add_column(t, cv);
        }
        if (strcmp(av->str, "drop_column") == 0) {
            JsonValue *cv = json_obj_get(entry, "c");
            if (!cv || cv->type != JSON_STR) return "corrupt WAL: alter drop column";
            return alter_drop_column(t, cv->str);
        }
        return "corrupt WAL: unknown alter action";
    }
    return "corrupt WAL: unknown kind";
}

/* ================================================================ */
/* schema description (fed to the LLM as context)                    */
/* ================================================================ */

char *db_schema_prompt(Database *db)
{
    char *out = NULL; /* NULL until first append */
    const char *prefix = "The current database schema:\n";
    size_t len = strlen(prefix);
    for (int i = 0; i < db->tables->nelems; i++) {
        JsonValue *t = db->tables->elems[i];
        JsonValue *cols = (JsonValue *)cols_of(t);
        JsonValue *rows = rows_of(t);
        const char *tn = table_name(t);

        char line[512];
        snprintf(line, sizeof line, "TABLE %s (%d rows)\n", tn, rows ? rows->nelems : 0);
        len += strlen(line);
        for (int j = 0; j < cols->nelems; j++) {
            char col[256];
            snprintf(col, sizeof col, "  - %s : %s%s\n", col_name(cols->elems[j]),
                     col_type(cols->elems[j]), col_is_pk(cols->elems[j]) ? " (primary key)" : "");
            len += strlen(col);
        }
    }
    if (!out) return strdup(prefix);

    out = malloc(len + 1);
    char *p = out;
    memcpy(p, prefix, strlen(prefix)); p += strlen(prefix);
    for (int i = 0; i < db->tables->nelems; i++) {
        JsonValue *t = db->tables->elems[i];
        JsonValue *cols = (JsonValue *)cols_of(t);
        JsonValue *rows = rows_of(t);
        const char *tn = table_name(t);
        char line[512];
        snprintf(line, sizeof line, "TABLE %s (%d rows)\n", tn, rows ? rows->nelems : 0);
        memcpy(p, line, strlen(line)); p += strlen(line);
        for (int j = 0; j < cols->nelems; j++) {
            char col[256];
            snprintf(col, sizeof col, "  - %s : %s%s\n", col_name(cols->elems[j]),
                     col_type(cols->elems[j]), col_is_pk(cols->elems[j]) ? " (primary key)" : "");
            memcpy(p, col, strlen(col)); p += strlen(col);
        }
    }
    *p = '\0';
    return out;
}