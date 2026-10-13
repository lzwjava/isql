/* sqllogic.c — golden-logic corpus runner (mini sqllogictest).
 *
 * Reads a corpus file of natural-language cases paired with their recorded
 * JSON plans, executes each plan on the engine and checks the expectation.
 * New failures here mean "the engine regressed against a golden plan that an
 * LLM produced" — the corpus doubles as prompt-example source for the LLM.
 *
 * Format (one statement per line, '#' comments):
 *   table <name>: <col:type[:pk]>, ...          creates the table now
 *   seed <name>: <v>, <v>, ...                  positional values by column order
 *   case <label>                                starts a new case
 *   plan <json>                                 the golden plan for the case
 *   expect <n> | error | message <substring>    runs the plan and checks it
 */
#include "../db.h"
#include "../json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;
static char cur_case[256] = "__setup__";
static Database db;

/* ---- tiny column registry (to map seed values by position) ---- */
#define MAXCOLS 64
typedef struct { char name[64]; char type[16]; int pk; } ColSpec;
typedef struct { char name[64]; ColSpec cols[MAXCOLS]; int ncols; } TSpec;
static TSpec tables[64];
static int ntables = 0;

static TSpec *find_spec(const char *name)
{
    for (int i = 0; i < ntables; i++)
        if (strcmp(tables[i].name, name) == 0) return &tables[i];
    return NULL;
}

static JsonValue *run(const char *json)
{
    JsonValue *p = json_parse(json);
    if (!p) { printf("   ! cannot parse plan JSON: %s\n", json); failures++; return NULL; }
    JsonValue *r = db_exec(&db, p);
    json_free(p);
    return r;
}

static void expect_check(const char *line, JsonValue *res)
{
    checks++;
    if (!res) return;
    JsonValue *okv = json_obj_get(res, "ok");
    int ok = okv && okv->type == JSON_BOOL && okv->boolean;

    if (strncmp(line, "message ", 8) == 0) {
        const char *want = line + 8;
        while (*want == ' ') want++;
        JsonValue *m = json_obj_get(res, "message");
        const char *msg = (m && m->type == JSON_STR) ? m->str : "";
        if (!ok || !strstr(msg, want)) {
            failures++;
            printf("FAIL case[%s]: expected message containing '%s', got '%s'\n",
                   cur_case, want, msg);
        }
        return;
    }
    if (strncmp(line, "error", 5) == 0) {
        if (ok) { failures++; printf("FAIL case[%s]: expected an error, got success\n", cur_case); }
        return;
    }
    int want = atoi(line);
    JsonValue *rows = json_obj_get(res, "rows");
    JsonValue *aff = json_obj_get(res, "affected");
    int got = (rows && rows->type == JSON_ARR) ? rows->nelems
             : (aff && aff->type == JSON_NUM) ? (int)aff->num : -1;
    if (!ok || got != want) {
        failures++;
        printf("FAIL case[%s]: expected %d, got %s (%d)\n",
               cur_case, want, ok ? "ok" : "error", got);
    }
}

static void do_table(const char *line)
{
    /* table users: id:int:pk, name:text, age:int */
    const char *spec = line + 6;
    const char *colon = strchr(spec, ':');
    if (!colon) { printf("corpus: bad table line\n"); exit(1); }
    TSpec *ts = &tables[ntables];
    if ((size_t)(colon - spec) >= sizeof ts->name) { printf("corpus: table name too long\n"); exit(1); }
    snprintf(ts->name, (size_t)(colon - spec) + 1, "%s", spec);
    ntables++;

    const char *p = colon + 1;
    int ci = 0;
    while (p && *p && ci < MAXCOLS) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        const char *end = strchr(p, ',');
        const char *seg_end = end ? end : p + strlen(p);
        const char *c1 = strchr(p, ':');
        if (!c1 || c1 > seg_end) { printf("corpus: bad column '%s'\n", p); exit(1); }
        const char *c2 = strchr(c1 + 1, ':');
        const char *type_end = (c2 && c2 < seg_end) ? c2 : seg_end;
        snprintf(ts->cols[ci].name, sizeof ts->cols[ci].name, "%.*s", (int)(c1 - p), p);
        snprintf(ts->cols[ci].type, sizeof ts->cols[ci].type, "%.*s",
                 (int)(type_end - (c1 + 1)), c1 + 1);
        ts->cols[ci].pk = (c2 && c2 < seg_end && strncmp(c2 + 1, "pk", 2) == 0);
        ci++;
        p = end ? end + 1 : p + strlen(p);
    }
    ts->ncols = ci;

    /* build the create_table plan JSON */
    char buf[4096];
    snprintf(buf, sizeof buf, "{\"op\":\"create_table\",\"table\":\"%s\",\"columns\":[", ts->name);
    for (int i = 0; i < ci; i++) {
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf),
                 "%s{\"name\":\"%s\",\"type\":\"%s\",\"primary\":%s}",
                 i ? "," : "", ts->cols[i].name, ts->cols[i].type,
                 ts->cols[i].pk ? "true" : "false");
    }
    strcat(buf, "]}");
    JsonValue *r = run(buf);
    if (r) json_free(r);
}

static void do_seed(const char *line)
{
    /* seed users: 1,alice,30,nyc */
    const char *s = line + 5;
    const char *colon = strchr(s, ':');
    if (!colon) { printf("corpus: bad seed line\n"); exit(1); }
    char tname[64];
    snprintf(tname, (size_t)(colon - s) + 1, "%s", s);
    TSpec *ts = find_spec(tname);
    if (!ts) { printf("corpus: seed for unknown table '%s'\n", tname); exit(1); }

    char build[8192];
    snprintf(build, sizeof build, "{\"op\":\"insert\",\"table\":\"%s\",\"values\":{", tname);
    const char *vp = colon + 1;
    for (int vi = 0; vi < ts->ncols; vi++) {
        while (*vp == ' ' || *vp == ',') vp++;
        if (!*vp) break;
        const char *ve = strchr(vp, ',');
        const char *vEnd = ve ? ve : vp + strlen(vp);
        char val[512];
        snprintf(val, (size_t)(vEnd - vp) + 1, "%s", vp);
        while (val[0] && (val[strlen(val) - 1] == ' ' || val[strlen(val) - 1] == '\t'))
            val[strlen(val) - 1] = 0;
        JsonValue *tmp = json_parse(val);
        if (tmp && tmp->type == JSON_NUM) {
            if (tmp->num == (double)(long long)tmp->num)
                snprintf(val, sizeof val, "%lld", (long long)tmp->num);
            else
                snprintf(val, sizeof val, "%g", tmp->num);
        } else {
            char quoted[520];
            snprintf(quoted, sizeof quoted, "\"%s\"", val); /* separate buffer: */
            snprintf(val, sizeof val, "%s", quoted);          /* overlap is UB */
        }
        if (tmp) json_free(tmp);
        snprintf(build + strlen(build), sizeof build - strlen(build),
                 "%s\"%s\":%s", vi ? "," : "", ts->cols[vi].name, val);
        vp = ve ? ve + 1 : vp + strlen(vp);
    }
    strcat(build, "}}");
    JsonValue *r = run(build);
    if (r) json_free(r);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/corpus.logic";
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "sqllogic: cannot open %s\n", path); return 1; }
    db_init(&db, NULL);

    char line[16384];
    char pending_plan[16384];
    int have_plan = 0;
    int lineno = 0;

    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *t = line;
        if (strlen(t) >= sizeof line - 2) { printf("corpus line %d too long\n", lineno); return 1; }
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '#' || *t == '\n' || *t == '\r' || *t == 0) continue;
        while (t[strlen(t) - 1] == '\n' || t[strlen(t) - 1] == '\r') t[strlen(t) - 1] = 0;

        if (strncmp(t, "table ", 6) == 0) { do_table(t); continue; }
        if (strncmp(t, "seed ", 5) == 0)  { do_seed(t); continue; }
        if (strncmp(t, "case ", 5) == 0)  { snprintf(cur_case, sizeof cur_case, "%s", t + 5); continue; }
        if (strncmp(t, "plan ", 5) == 0) {
            snprintf(pending_plan, sizeof pending_plan, "%s", t + 5);
            have_plan = 1;
            continue;
        }
        if (strncmp(t, "expect ", 7) == 0) {
            if (!have_plan) { printf("corpus line %d: expect without plan\n", lineno); return 1; }
            JsonValue *res = run(pending_plan);
            expect_check(t + 7, res);
            if (res) json_free(res);
            have_plan = 0;
            continue;
        }
    }
    fclose(f);
    printf("\n%d corpus checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}