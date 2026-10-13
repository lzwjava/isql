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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 65536
#define MAX_STMT (8 * MAX_LINE)

static Database db;

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
    " 3. insert\n"
    "    {\"op\":\"insert\",\"table\":\"name\",\"values\":{\"name\":\"alice\",\"score\":9.5}}\n"
    "    (omit the primary key to auto-increment it)\n"
    " 4. select\n"
    "    {\"op\":\"select\",\"table\":\"name\",\"columns\":[\"name\",\"score\"],\n"
    "     \"where\":{\"score\":{\"gt\":5}},\"order_by\":[{\"column\":\"score\",\"desc\":true}],\n"
    "     \"limit\":10}\n"
    "    \"columns\" may be \"*\" for all; \"where\", \"order_by\", \"limit\" are optional.\n"
    " 5. update\n"
    "    {\"op\":\"update\",\"table\":\"name\",\"values\":{\"score\":10},\"where\":{\"name\":\"alice\"}}\n"
    " 6. delete         {\"op\":\"delete\",\"table\":\"name\",\"where\":{\"score\":{\"lt\":3}}}\n"
    " 7. show_tables    {\"op\":\"show_tables\"}\n"
    " 8. describe       {\"op\":\"describe\",\"table\":\"name\"}\n"
    " 9. error          {\"op\":\"error\",\"message\":\"why the request cannot be handled\"}\n"
    "\n"
    "\"where\" grammar (\"and\"/\"or\" for boolean logic, nested allowed):\n"
    "  {\"col\": 5}                 equality\n"
    "  {\"col\": {\"gt\":5}}         gt | gte | lt | lte | ne\n"
    "  {\"and\": [<cond>, ...]}     all must match\n"
    "  {\"or\":  [<cond>, ...]}     at least one must match\n"
    "  {\"col\": \"text\"}           text equality\n"
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

static void print_result(JsonValue *res)
{
    if (!res) { printf("(no result)\n"); return; }
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
static JsonValue *fake_parse(const char *stmt)
{
    const char *u = stmt;
    if (strstr(u, "create"))
        return json_parse("{\"op\":\"create_table\",\"table\":\"users\",\"columns\":["
            "{\"name\":\"id\",\"type\":\"int\",\"primary\":true},"
            "{\"name\":\"name\",\"type\":\"text\"},"
            "{\"name\":\"age\",\"type\":\"int\"}]}");
    if (strstr(u, "insert"))
        return json_parse("{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"alice\",\"age\":30}}");
    if (strstr(u, "update"))
        return json_parse("{\"op\":\"update\",\"table\":\"users\",\"values\":{\"age\":31},\"where\":{\"name\":\"alice\"}}");
    if (strstr(u, "delete"))
        return json_parse("{\"op\":\"delete\",\"table\":\"users\",\"where\":{\"age\":{\"gt\":40}}}");
    if (strstr(u, "table")) /* describe / list tables */
        return json_parse("{\"op\":\"show_tables\"}");
    return json_parse("{\"op\":\"select\",\"table\":\"users\",\"columns\":\"*\",\"order_by\":[{\"column\":\"age\",\"desc\":true}]}");
}

static void run_statement(const char *stmt)
{
    char *schema = db_schema_prompt(&db);

    char sysprompt[16384];
    snprintf(sysprompt, sizeof sysprompt, LLM_RULES, schema);
    free(schema);

    char *base  = getenv("ISQL_BASE");
    char *key   = getenv("ISQL_KEY");
    char *model = getenv("ISQL_MODEL");
    if (!base)  base  = "https://api.openai.com/v1";
    if (!model) model = "gpt-4o-mini";

    JsonValue *plan = NULL;
    if (getenv("ISQL_FAKE_LLM")) {
        printf("[fake-llm] %s\n", stmt);
        plan = fake_parse(stmt);
    } else {
        LlmReply r = llm_chat(base, key, model, sysprompt, stmt);
        if (r.error) {
            printf("LLM error: %s\n", r.error);
            free(r.error);
            return;
        }
        plan = json_parse(r.content);
        free(r.content);
    }

    if (!plan) {
        printf("LLM error: could not parse the plan as JSON\n");
        return;
    }

    /* execute single op or a batch of ops */
    JsonValue *ops = json_obj_get(plan, "ops");
    int batch = (ops && ops->type == JSON_ARR);
    if (!batch) {
        JsonValue *res = db_exec(&db, plan);
        print_result(res);
        if (json_obj_get(res, "ok")) {
            const char *err = db_save(&db);
            if (err) printf("WARNING: cannot save: %s\n", err);
        }
        json_free(res);
    } else {
        for (int i = 0; i < ops->nelems; i++) {
            JsonValue *res = db_exec(&db, ops->elems[i]);
            print_result(res);
            if (json_obj_get(res, "ok")) {
                const char *err = db_save(&db);
                if (err) printf("WARNING: cannot save: %s\n", err);
            }
            json_free(res);
        }
    }
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

    printf("isql 0.1 - AI-driven minimal database\n"
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

    const char *serr = db_save(&db);
    if (serr) fprintf(stderr, "isql: save failed: %s\n", serr);
    return 0;
}