/* main.c — isql: a minimal AI-driven database.
 *
 * The REPL reads a request (natural language or SQL), sends it to an LLM
 * together with the current schema, and executes the JSON query plan the
 * LLM returns. The engine itself (db.c) is a tiny SQLite-like store that
 * understands only JSON plans, never SQL text.
 *
 * Environment variables:
 *   ISQL_BASE  - OpenAI-compatible base URL (default https://api.openai.com/v1)
 *   ISQL_KEY   - API key (Bearer token). Ollama ignores it.
 *   ISQL_MODEL - model id (default gpt-4o-mini)
 *   ISQL_DB    - database file (default ./isql.db.json)
 */
#include "db.h"
#include "json.h"
#include "llm.h"
#include "wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 65536
#define MAX_STMT (8 * MAX_LINE)

static Database db;

/* explicit BEGIN/COMMIT/ROLLBACK transactions */
static int txn_active = 0;
static JsonValue *txn_snap = NULL;

static const char *LLM_RULES =
    "You are the SQL parsing engine inside a tiny embedded database (a simplified SQLite).\n"
    "Translate the user's request (plain language or SQL) into EXACTLY ONE JSON query plan.\n"
    "Reply with the raw JSON object only - no markdown fences, no explanations, no comments.\n"
    "The schema of the current database is shown below; if the request references a table or\n"
    "column that does not exist, reply with {\"op\":\"error\",\"message\":\"...\"}.\n"
    "\n"
    "%s"
    "\n"
    "Available operations (pick one):\n"
    " 1. create_table\n"
    "    {\"op\":\"create_table\",\"table\":\"name\",\"columns\":[\n"
    "       {\"name\":\"id\",\"type\":\"int\",\"primary\":true},\n"
    "       {\"name\":\"name\",\"type\":\"text\"},\n"
    "       {\"name\":\"score\",\"type\":\"real\"}]}\n"
    "    types: \"int\", \"real\", \"text\". At most one primary key column.\n"
    " 2. drop_table      {\"op\":\"drop_table\",\"table\":\"name\"}\n"
    " 3. alter_table\n"
    "    {\"op\":\"alter_table\",\"table\":\"name\",\"action\":\"add_column\",\"column\":{\"name\":\"email\",\"type\":\"text\"}}\n"
    "    action: add_column | drop_column (drop uses {\"column\":\"email\"} as a string)\n"
    " 4. insert\n"
    "    {\"op\":\"insert\",\"table\":\"name\",\"values\":{\"name\":\"alice\",\"score\":9.5}}\n"
    "    (omit the primary key to auto-increment it)\n"
    " 5. select\n"
    "    {\"op\":\"select\",\"table\":\"name\",\"columns\":[\"users.name\",\"orders.total\"],\n"
    "     \"where\":{\"score\":{\"gt\":5}},\"order_by\":[{\"column\":\"score\",\"desc\":true}],\n"
    "     \"limit\":10,\"distinct\":true}\n"
    "    \"columns\" may be \"*\" for all; \"where\", \"order_by\", \"limit\", \"distinct\" are optional.\n"
    " 6. select with a JOIN (INNER). The join is a sibling of the base table:\n"
    "    {\"op\":\"select\",\"table\":\"orders\",\"join\":{\"table\":\"users\",\"on\":{\"users.id\":\"orders.user_id\"}},\n"
    "     \"columns\":[\"users.name\",\"orders.total\"],\"where\":{\"orders.total\":{\"gt\":100}}}\n"
    "    \"on\" maps left columns to right columns (all equalities must hold). Right-side\n"
    "    columns are addressed as \"<table>.<col>\" when the bare name collides with the left.\n"
    " 7. aggregate\n"
    "    {\"op\":\"aggregate\",\"table\":\"name\",\"where\":{...},\"group_by\":[\"city\"],\n"
    "     \"aggs\":[{\"as\":\"n\",\"func\":\"count\",\"column\":\"*\"},\n"
    "              {\"as\":\"total\",\"func\":\"sum\",\"column\":\"score\"},\n"
    "              {\"as\":\"avg\",\"func\":\"avg\",\"column\":\"score\"},\n"
    "              {\"as\":\"youngest\",\"func\":\"min\",\"column\":\"age\"}]}\n"
    "    funcs: count (column \"*\" or a name), sum, avg, min, max. \"group_by\" optional.\n"
    " 8. update\n"
    "    {\"op\":\"update\",\"table\":\"name\",\"values\":{\"score\":10},\"where\":{\"name\":\"alice\"}}\n"
    " 9. delete         {\"op\":\"delete\",\"table\":\"name\",\"where\":{\"score\":{\"lt\":3}}}\n"
    " 10. show_tables   {\"op\":\"show_tables\"}\n"
    " 11. describe      {\"op\":\"describe\",\"table\":\"name\"}\n"
    " 12. error         {\"op\":\"error\",\"message\":\"why the request cannot be handled\"}\n"
    " 13. batch (for multi-step requests):\n"
    "     {\"ops\":[<any ops above>, ...]}  executes all-or-nothing (rolls back on failure)\n"
    "\n"
    "\"where\" grammar (\"and\"/\"or\" for boolean logic, nested allowed):\n"
    "  {\"col\": 5}                    equality\n"
    "  {\"col\": {\"gt\":5}}            gt | gte | lt | lte | ne\n"
    "  {\"col\": {\"in\":[1,2,3]}}      value in array\n"
    "  {\"col\": {\"like\":\"a%\"}}      pattern: % = any sequence, _ = one char\n"
    "  {\"col\": {\"between\":[lo,hi]}}  inclusive range (numeric)\n"
    "  {\"and\": [<cond>, ...]}        all must match\n"
    "  {\"or\":  [<cond>, ...]}        at least one must match\n"
    "Values must be JSON: numbers unquoted, strings in double quotes.";

/* ---------------- pretty printer ---------------- */

static const char *cell_display(const JsonValue *v, char *buf, int buflen)
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

static int res_is_ok(JsonValue *res)
{
    JsonValue *ok = res ? json_obj_get(res, "ok") : NULL;
    return ok && ok->type == JSON_BOOL && ok->boolean;
}

static void print_result(JsonValue *res)
{
    if (!res) { printf("(no result)\n"); return; }
    JsonValue *items = json_obj_get(res, "items");
    if (items && items->type == JSON_ARR) {
        JsonValue *msgv = json_obj_get(res, "message");
        JsonValue *rb   = json_obj_get(res, "rolled_back");
        printf("%s\n", (msgv && msgv->type == JSON_STR) ? msgv->str : "");
        for (int i = 0; i < items->nelems; i++)
            print_result(items->elems[i]);
        if (rb && rb->type == JSON_BOOL && rb->boolean)
            printf("BATCH ROLLED BACK - no changes were kept.\n");
        return;
    }
    JsonValue *okv  = json_obj_get(res, "ok");
    JsonValue *msgv = json_obj_get(res, "message");
    int ok = okv && okv->type == JSON_BOOL && okv->boolean;
    const char *msg = (msgv && msgv->type == JSON_STR) ? msgv->str : "";

    if (!ok) {
        printf("ERROR: %s\n", msg);
        return;
    }
    printf("%s\n", msg);

    JsonValue *cols = json_obj_get(res, "columns");
    JsonValue *rows = json_obj_get(res, "rows");
    if (!cols || !rows || cols->type != JSON_ARR || rows->type != JSON_ARR) return;

    int ncol = cols->nelems;
    if (ncol == 0) return;
    if (ncol > 64) ncol = 64; /* safety cap for the pretty printer */
    /* column display names */
    char colnames[64][256];
    for (int i = 0; i < ncol && i < 64; i++) {
        JsonValue *c = cols->elems[i];
        snprintf(colnames[i], sizeof colnames[i], "%s", (c && c->type == JSON_STR) ? c->str : "?");
    }
    /* widths */
    int width[64];
    for (int i = 0; i < ncol; i++) width[i] = (int)strlen(colnames[i]);
    for (int r = 0; r < rows->nelems && r < 200; r++) {
        JsonValue *row = rows->elems[r];
        for (int i = 0; i < ncol; i++) {
            char tmp[64];
            const char *s = cell_display(row ? json_obj_get(row, colnames[i]) : NULL, tmp, sizeof tmp);
            int l = (int)strlen(s);
            if (l > width[i]) width[i] = l;
        }
    }
    /* header */
    for (int i = 0; i < ncol; i++) printf("| %-*s ", width[i], colnames[i]);
    printf("|\n");
    for (int i = 0; i < ncol; i++) printf("+-%.*s-", width[i], "--------------------------------");
    printf("+\n");
    /* body */
    for (int r = 0; r < rows->nelems && r < 200; r++) {
        JsonValue *row = rows->elems[r];
        for (int i = 0; i < ncol; i++) {
            char tmp[64];
            const char *s = cell_display(row ? json_obj_get(row, colnames[i]) : NULL, tmp, sizeof tmp);
            printf("| %-*s ", width[i], s);
        }
        printf("|\n");
    }
    if (rows->nelems > 200) printf("... (%d more rows)\n", rows->nelems - 200);
}

/* ---------------- REPL helpers ---------------- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int e = (int)strlen(s) - 1;
    while (e >= 0 && (s[e] == ' ' || s[e] == '\t' || s[e] == '\n' || s[e] == '\r')) s[e--] = '\0';
    return s;
}

static int stmd_ends_with(const char *s, char c)
{
    return s && strlen(s) > 0 && s[strlen(s) - 1] == c;
}

static void print_help(void)
{
    printf("isql - AI-driven minimal database\n"
           "  Natural language or SQL is parsed by an LLM into a JSON plan and\n"
           "  executed by a tiny embedded engine (no SQL parser in the binary).\n\n"
           "  CREATE TABLE / DROP / INSERT / SELECT / UPDATE / DELETE\n"
           "  \"show tables\", \"describe <table>\"\n\n"
           "  End a statement with ';' or press Enter on an empty line.\n"
           "  Meta commands: help, exit/quit\n");
}

/* ---------------- fake LLM (offline demo/testing) ----------------
 * When ISQL_FAKE_LLM=1 the REPL skips the network call and substitutes a
 * deterministic canned parser, so the full pipeline works with no API key.
 * The real path uses llm_chat() and is otherwise identical. */
static double fake_user_id(const char *name)
{
    for (int t = 0; t < db.tables->nelems; t++) {
        JsonValue *tbl = db.tables->elems[t];
        JsonValue *n = json_obj_get(tbl, "name");
        if (!n || n->type != JSON_STR || strcmp(n->str, "users") != 0) continue;
        JsonValue *rows = json_obj_get(tbl, "rows");
        for (int i = 0; i < rows->nelems; i++) {
            JsonValue *nm = json_obj_get(rows->elems[i], "name");
            if (nm && nm->type == JSON_STR && strcmp(nm->str, name) == 0) {
                JsonValue *id = json_obj_get(rows->elems[i], "id");
                if (id && id->type == JSON_NUM) return id->num;
            }
        }
    }
    return -1;
}

static JsonValue *fake_parse(const char *stmt)
{
    const char *u = stmt;
    char tmp[512];

    if (strstr(u, "create") && strstr(u, "orders"))
        return json_parse("{\"op\":\"create_table\",\"table\":\"orders\",\"columns\":["
            "{\"name\":\"id\",\"type\":\"int\",\"primary\":true},"
            "{\"name\":\"user_id\",\"type\":\"int\"},"
            "{\"name\":\"total\",\"type\":\"real\"}]}");
    if (strstr(u, "create") || strstr(u, "new table"))
        return json_parse("{\"op\":\"create_table\",\"table\":\"users\",\"columns\":["
            "{\"name\":\"id\",\"type\":\"int\",\"primary\":true},"
            "{\"name\":\"name\",\"type\":\"text\"},"
            "{\"name\":\"age\",\"type\":\"int\"},"
            "{\"name\":\"city\",\"type\":\"text\"}]}");

    if ((strstr(u, "order") || strstr(u, "one ")) && (strstr(u, "for") && strstr(u, "worth"))) {
        /* "an order for <name> worth <n>" -> insert into orders */
        double uid = -1, total = 0;
        /* extract name between "for " and " worth " (or ",") */
        const char *f = strstr(u, "for ");
        if (f) {
            const char *end = strstr(f + 4, " worth");
            char nm[128];
            if (!end) end = f + 4;
            snprintf(nm, (size_t)(end - (f + 4) + 1), "%s", f + 4);
            char *tr = trim(nm);
            uid = fake_user_id(tr);
        }
        const char *w = strstr(u, "worth ");
        if (w) {
            char num[64];
            int i = 0;
            while (w[6 + i] && (w[6 + i] >= '0' && w[6 + i] <= '9') && i < 63) { num[i] = w[6 + i]; i++; }
            num[i] = 0;
            total = atof(num);
        }
        snprintf(tmp, sizeof tmp,
                 "{\"op\":\"insert\",\"table\":\"orders\",\"values\":{\"user_id\":%.0f,\"total\":%g}}",
                 uid, total);
        return json_parse(tmp);
    }
    if (strstr(u, "add column") || strstr(u, "new column"))
        return json_parse("{\"op\":\"alter_table\",\"table\":\"users\",\"action\":\"add_column\",\"column\":{\"name\":\"email\",\"type\":\"text\"}}");
    if (strstr(u, "insert") || strstr(u, "add ") || strstr(u, "add a"))
        return json_parse("{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"alice\",\"age\":30,\"city\":\"nyc\"}}");
    if (strstr(u, "update"))
        return json_parse("{\"op\":\"update\",\"table\":\"users\",\"values\":{\"age\":31},\"where\":{\"name\":\"alice\"}}");
    if (strstr(u, "delete"))
        return json_parse("{\"op\":\"delete\",\"table\":\"users\",\"where\":{\"age\":{\"gt\":40}}}");
    if (strstr(u, "batch"))
        return json_parse("{\"ops\":[{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"eve\",\"age\":29,\"city\":\"la\"}},{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"frank\",\"age\":35,\"city\":\"nyc\"}}]}");
    if (strstr(u, "join") || strstr(u, "with the buyer") || strstr(u, "customer name"))
        return json_parse("{\"op\":\"select\",\"table\":\"orders\",\"join\":{\"table\":\"users\",\"on\":{\"users.id\":\"orders.user_id\"}},\"columns\":[\"users.name\",\"orders.total\"],\"where\":{\"orders.total\":{\"gt\":50}},\"order_by\":[{\"column\":\"orders.total\",\"desc\":true}]}");
    if (strstr(u, "average") || strstr(u, "avg") || strstr(u, "sum") || strstr(u, "per city") || strstr(u, "per town"))
        return json_parse("{\"op\":\"aggregate\",\"table\":\"users\",\"group_by\":[\"city\"],\"aggs\":[{\"as\":\"n\",\"func\":\"count\",\"column\":\"*\"},{\"as\":\"avg_age\",\"func\":\"avg\",\"column\":\"age\"}]}");
    if (strstr(u, "starts with") || strstr(u, "begins"))
        return json_parse("{\"op\":\"select\",\"table\":\"users\",\"where\":{\"name\":{\"like\":\"a%\"}}}");
    if (strstr(u, "between"))
        return json_parse("{\"op\":\"select\",\"table\":\"users\",\"where\":{\"age\":{\"between\":[25,30]}}}");
    if (strstr(u, "table")) /* describe / list tables */
        return json_parse("{\"op\":\"show_tables\"}");
    return json_parse("{\"op\":\"select\",\"table\":\"users\",\"columns\":\"*\",\"order_by\":[{\"column\":\"age\",\"desc\":true}]}");
}

/* multi-turn memory: remember the last few (request -> plan) pairs so
 * follow-ups like "and those older than 40" can build on previous turns. */
#define HISTORY_MAX 6
static char history[HISTORY_MAX][1024];
static int hist_n = 0, hist_head = 0;

static void history_add(const char *stmt, const char *planjson)
{
    size_t n = strlen(stmt) + strlen(planjson) + 64;
    if (n >= sizeof history[0]) return;
    snprintf(history[hist_head], sizeof history[hist_head],
             "user: \"%s\"\n  -> %s", stmt, planjson);
    hist_head = (hist_head + 1) % HISTORY_MAX;
    if (hist_n < HISTORY_MAX) hist_n++;
}

static void history_prompt(char *buf, int buflen)
{
    buf[0] = 0;
    if (hist_n == 0) return;
    strncat(buf, "Recent conversation (context for follow-up requests like \"and the older ones\" - do NOT re-execute past requests):\n", buflen - strlen(buf) - 1);
    for (int i = 0; i < hist_n; i++) {
        int idx = (hist_head - hist_n + i + HISTORY_MAX) % HISTORY_MAX;
        if (history[idx][0]) {
            strncat(buf, history[idx], buflen - strlen(buf) - 1);
            strncat(buf, "\n", buflen - strlen(buf) - 1);
        }
    }
}

static void run_statement(const char *stmt)
{
    char *schema = db_schema_prompt(&db);
    char conv[4096];
    history_prompt(conv, sizeof conv);

    char *sysprompt = malloc(16384 + 4096 + strlen(conv) + 64);
    snprintf(sysprompt, (size_t)(16384 + 4096 + strlen(conv) + 64),
             "%s\n%s\n%s", LLM_RULES, conv, schema);
    free(schema);

    char *base  = getenv("ISQL_BASE");
    char *key   = getenv("ISQL_KEY");
    char *model = getenv("ISQL_MODEL");
    if (!base)  base  = "https://api.openai.com/v1";
    if (!model) model = "gpt-4o-mini";

    JsonValue *plan = NULL;
    JsonValue *res = NULL;
    if (getenv("ISQL_FAKE_LLM")) {
        printf("[fake-llm] %s\n", stmt);
        plan = fake_parse(stmt);
    } else {
        LlmReply r = llm_chat(base, key, model, sysprompt, stmt);
        if (r.error) {
            printf("LLM error: %s\n", r.error);
            free(r.error);
            free(sysprompt);
            return;
        }
        plan = json_parse(r.content);
        free(r.content);
        free(sysprompt);
    }

    if (!plan) {
        printf("LLM error: could not parse the plan as JSON\n");
        return;
    }

    res = db_exec(&db, plan);
    print_result(res);
    if (res_is_ok(res) && !db.wal) {
        /* no WAL (disabled or could not attach): fall back to a full snapshot */
        const char *err = db_save(&db);
        if (err) printf("WARNING: cannot save: %s\n", err);
    }

    /* remember this exchange for follow-up questions */
    char *planjson = json_to_string(plan);
    history_add(stmt, planjson);
    free(planjson);
    json_free(res);
    json_free(plan);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char *dbfile = getenv("ISQL_DB");
    if (!dbfile) dbfile = "isql.db.json";

    db_init(&db, dbfile);
    const char *lerr = db_load(&db);
    if (lerr) {
        fprintf(stderr, "isql: %s\n", lerr);
        return 1;
    }
    if (getenv("ISQL_WAL") && strcmp(getenv("ISQL_WAL"), "0") == 0) {
        fprintf(stderr, "isql: WAL disabled by ISQL_WAL=0\n");
    } else {
        const char *werr = db_attach_wal(&db);
        if (werr) fprintf(stderr, "isql: %s\n", werr);
    }

    printf("isql 0.3 - AI-driven minimal database\n"
           "  base : %s\n  model: %s\n  db   : %s\n"
           "  type 'help' for usage, 'exit' to quit\n\n",
           getenv("ISQL_BASE") ? getenv("ISQL_BASE") : "https://api.openai.com/v1",
           getenv("ISQL_MODEL") ? getenv("ISQL_MODEL") : "gpt-4o-mini",
           dbfile);

    char *stmt = NULL;
    char line[MAX_LINE];
    for (;;) {
        if (!stmt) fflush(stdout);
        printf("isql> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) {
            if (!stmt) break;
            /* EOF finalizes any pending statement */
            run_statement(stmt);
            free(stmt);
            stmt = NULL;
            break;
        }
        char *t = trim(line);
        if (t[0] == '\0') {
            if (stmt) { run_statement(stmt); free(stmt); stmt = NULL; }
            continue;
        }
        if (!stmt) {
            if (strcmp(t, "help") == 0 || strcmp(t, "?") == 0) { print_help(); continue; }
            if (strcmp(t, "exit") == 0 || strcmp(t, "quit") == 0) break;
            if (strcmp(t, "begin") == 0) {
                if (txn_active) { printf("transaction already active\n"); continue; }
                txn_snap = json_clone(db.tables);
                if (db.wal) wal_begin(db.wal);
                txn_active = 1;
                printf("BEGIN\n");
                continue;
            }
            if (strcmp(t, "commit") == 0) {
                if (!txn_active) { printf("no transaction in progress\n"); continue; }
                if (db.wal) {
                    const char *ce = wal_commit(db.wal);
                    if (ce) { printf("ERROR: %s\n", ce); continue; }
                }
                json_free(txn_snap);
                txn_snap = NULL;
                txn_active = 0;
                printf("COMMIT\n");
                continue;
            }
            if (strcmp(t, "rollback") == 0) {
                if (!txn_active) { printf("no transaction in progress\n"); continue; }
                json_free(db.tables); /* mutated tree; snapshot is a deep clone */
                db.tables = txn_snap;
                if (db.wal) wal_rollback(db.wal);
                txn_snap = NULL;
                txn_active = 0;
                printf("ROLLBACK\n");
                continue;
            }
        }
        /* append to statement */
        if (stmt == NULL)
            stmt = strdup(t);
        else {
            if (strlen(stmt) + strlen(t) + 2 > MAX_STMT) {
                printf("statement too long\n");
                free(stmt); stmt = NULL;
                continue;
            }
            stmt = realloc(stmt, strlen(stmt) + strlen(t) + 2);
            strcat(stmt, " ");
            strcat(stmt, t);
        }
        /* drop trailing ';' if present */
        if (stmd_ends_with(stmt, ';')) {
            stmt[strlen(stmt) - 1] = '\0';
            stmt = trim(stmt);
            run_statement(stmt);
            free(stmt);
            stmt = NULL;
        }
    }
    if (stmt) { free(stmt); stmt = NULL; }
    if (txn_active) {
        /* an open transaction at exit is rolled back */
        wal_rollback(db.wal);
        txn_active = 0;
        fprintf(stderr, "isql: open transaction discarded on exit\n");
    }

    const char *serr = db_checkpoint(&db);
    if (serr) fprintf(stderr, "isql: save failed: %s\n", serr);
    return 0;
}