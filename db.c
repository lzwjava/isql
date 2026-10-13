/* db.c — the minimal database engine. Tables are JSON objects:
 *
 *   {"name": "...",
 *    "columns": [{"name": "...", "type": "int|real|text", "primary": true}],
 *    "rows": [ {"col": value, ...}, ... ]}
 *
 * Queries arrive as JSON plans produced by the LLM parser (db_exec below).
 */
#include "db.h"

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
    JsonValue *cell = row ? json_obj_get(row, col) : NULL;
    if (cond && cond->type == JSON_OBJ) {
        const char *ops[] = { "gt", "gte", "lt", "lte", "ne", NULL };
        for (int i = 0; ops[i]; i++) {
            JsonValue *cv = json_obj_get(cond, ops[i]);
            if (cv) {
                int c = cell_cmp(cell, cv);
                switch (i) {
                case 0: return c > 0;   /* gt  */
                case 1: return c >= 0;  /* gte */
                case 2: return c < 0;   /* lt  */
                case 3: return c <= 0;  /* lte */
                default: return c != 0; /* ne  */
                }
            }
        }
    }
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

    char msg[96];
    snprintf(msg, sizeof msg, "Inserted 1 row into '%s'", table_name(t));
    return result_ok(msg, 1);
}

/* which columns does the SELECT return?
 * returns (a) the table's full columns array for "*",
 *         (b) a validated array of column-name strings, or
 *         (c) NULL with *err set. */
static JsonValue *select_column_spec(JsonValue *t, JsonValue *spec, int *all_cols, const char **err)
{
    *err = NULL;
    *all_cols = 0;
    JsonValue *cols = (JsonValue *)cols_of(t);
    if (!spec || (spec->type == JSON_STR && spec->str[0] == '*')) {
        *all_cols = 1;
        return cols;
    }
    if (spec->type != JSON_ARR) {
        *err = "select: 'columns' must be '*' or an array of names";
        return NULL;
    }
    for (int i = 0; i < spec->nelems; i++) {
        JsonValue *cn = spec->elems[i];
        if (!cn || cn->type != JSON_STR || strcmp(cn->str, "*") == 0 || !find_column(t, cn->str)) {
            *err = "select: unknown column in 'columns'";
            return NULL;
        }
    }
    return spec;
}

/* returns a new array of row objects matching `where` */
static JsonValue *filter_rows(JsonValue *t, JsonValue *where)
{
    JsonValue *rows = rows_of(t);
    JsonValue *out = json_arr();
    for (int i = 0; i < rows->nelems; i++) {
        JsonValue *r = rows->elems[i];
        if (cond_matches(where, r))
            json_arr_push(out, r);
    }
    return out;
}

/* stable insertion sort by the order_by entries */
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
                JsonValue *a = json_obj_get(matched->elems[j], c->str);
                JsonValue *b = json_obj_get(key, c->str);
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

static JsonValue *op_select(Database *db, JsonValue *op)
{
    JsonValue *nv = json_obj_get(op, "table");
    if (!nv || nv->type != JSON_STR) return result_err("select: missing 'table'");
    JsonValue *t = find_table(db, nv->str);
    if (!t) return result_err("select: no such table");

    const char *err = NULL;
    int all_cols = 0;
    JsonValue *spec = select_column_spec(t, json_obj_get(op, "columns"), &all_cols, &err);
    if (err) return result_err(err);

    JsonValue *matched = filter_rows(t, json_obj_get(op, "where"));

    JsonValue *ord = json_obj_get(op, "order_by");
    if (ord && ord->type == JSON_ARR && ord->nelems > 0)
        sort_rows(matched, ord);

    JsonValue *lim = json_obj_get(op, "limit");
    int limit = (lim && lim->type == JSON_NUM && lim->num >= 0) ? (int)lim->num : matched->nelems;
    if (limit > matched->nelems) limit = matched->nelems;

    JsonValue *cols = (JsonValue *)cols_of(t);
    JsonValue *columns = json_arr();
    if (all_cols) {
        for (int i = 0; i < cols->nelems; i++)
            json_arr_push(columns, json_str(col_name(cols->elems[i])));
    } else {
        for (int i = 0; i < spec->nelems; i++) {
            if (spec->elems[i]->type != JSON_STR) continue;
            json_arr_push(columns, json_str(spec->elems[i]->str));
        }
    }

    JsonValue *rowobjs = json_arr();
    for (int i = 0; i < limit; i++) {
        JsonValue *src = matched->elems[i];
        JsonValue *r = json_obj();
        if (all_cols) {
            for (int j = 0; j < cols->nelems; j++) {
                const char *cn = col_name(cols->elems[j]);
                JsonValue *cell = json_obj_get(src, cn);
                json_obj_set(r, cn, cell ? json_clone(cell) : json_null());
            }
        } else {
            for (int j = 0; j < spec->nelems; j++) {
                if (spec->elems[j]->type != JSON_STR) continue;
                const char *cn = spec->elems[j]->str;
                JsonValue *cell = json_obj_get(src, cn);
                json_obj_set(r, cn, cell ? json_clone(cell) : json_null());
            }
        }
        json_arr_push(rowobjs, r);
    }
    matched->nelems = 0; /* matched is a view over table rows: don't free the rows */
    json_free(matched);

    char msg[96];
    snprintf(msg, sizeof msg, "Fetched %d row(s) from '%s'", rowobjs->nelems, table_name(t));
    return result_rows(rowobjs, columns, msg);
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
    int n = 0;
    for (int i = 0; i < rows->nelems; i++) {
        JsonValue *row = rows->elems[i];
        if (!cond_matches(where, row)) continue;
        for (int v = 0; v < values->npairs; v++) {
            const char *cname = values->pairs[v].key;
            const JsonValue *col = find_column(t, cname);
            const char *err = NULL;
            JsonValue *cv = coerce(values->pairs[v].val, col_type(col), cname, &err);
            if (err) return result_err(err);
            if (col_is_pk(col) && cv->type != JSON_NULL && pk_conflict(t, cname, cv, row))
                return result_err("update: primary key conflict");
            json_obj_set(row, cname, cv);
        }
        n++;
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
    JsonValue *kept = json_arr();
    int removed = 0;
    for (int i = 0; i < rows->nelems; i++) {
        if (cond_matches(where, rows->elems[i]))
            removed++; /* row matches the WHERE: delete it */
        else
            json_arr_push(kept, rows->elems[i]);
    }
    rows->nelems = 0;
    for (int i = 0; i < kept->nelems; i++)
        json_arr_push(rows, kept->elems[i]);
    kept->nelems = 0;
    json_free(kept);

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
/* dispatcher                                                        */
/* ================================================================ */

JsonValue *db_exec(Database *db, JsonValue *op)
{
    if (!op || op->type != JSON_OBJ)
        return result_err("bad query plan: expected a JSON object");
    JsonValue *opv = json_obj_get(op, "op");
    if (!opv || opv->type != JSON_STR)
        return result_err("bad query plan: missing string field 'op'");
    const char *name = opv->str;

    if (strcmp(name, "create_table") == 0) return op_create_table(db, op);
    if (strcmp(name, "drop_table")   == 0) return op_drop_table(db, op);
    if (strcmp(name, "insert")       == 0) return op_insert(db, op);
    if (strcmp(name, "select")       == 0) return op_select(db, op);
    if (strcmp(name, "update")       == 0) return op_update(db, op);
    if (strcmp(name, "delete")       == 0) return op_delete(db, op);
    if (strcmp(name, "show_tables")  == 0) return op_show_tables(db, op);
    if (strcmp(name, "describe")     == 0) return op_describe(db, op);
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
    char *json = json_to_string(root);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", db->filename);
    FILE *f = fopen(tmp, "w");
    if (!f) { free(json); return "cannot write database file"; }
    fputs(json, f);
    fclose(f);
    rename(tmp, db->filename);
    free(json);
    return NULL;
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