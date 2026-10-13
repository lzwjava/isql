/* test_engine.c — offline unit tests for the JSON layer + database engine.
 * No LLM, no network: exercise the exact JSON plans the LLM would emit. */
#include "../db.h"
#include "../json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* build a plan from a JSON literal and exec it */
static JsonValue *run(Database *db, const char *planjson)
{
    JsonValue *plan = json_parse(planjson);
    JsonValue *res = db_exec(db, plan);
    json_free(plan);
    return res;
}

static int ok_code(JsonValue *res)
{
    JsonValue *ok = json_obj_get(res, "ok");
    return ok && ok->type == JSON_BOOL && ok->boolean;
}

static int rows_of_result(JsonValue *res)
{
    JsonValue *rows = json_obj_get(res, "rows");
    return rows ? rows->nelems : 0;
}

int main(void)
{
    Database db;
    db_init(&db, NULL);

    /* --- create table --- */
    JsonValue *r = run(&db,
        "{\"op\":\"create_table\",\"table\":\"users\",\"columns\":["
        "{\"name\":\"id\",\"type\":\"int\",\"primary\":true},"
        "{\"name\":\"name\",\"type\":\"text\"},"
        "{\"name\":\"age\",\"type\":\"int\"}]}");
    check(ok_code(r), "create_table ok");
    json_free(r);

    /* duplicate table rejected */
    r = run(&db, "{\"op\":\"create_table\",\"table\":\"users\",\"columns\":[{\"name\":\"x\",\"type\":\"text\"}]}");
    check(!ok_code(r), "duplicate create_table rejected");
    json_free(r);

    /* --- insert with explicit pk + auto pk --- */
    check(ok_code(run(&db, "{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"id\":10,\"name\":\"alice\",\"age\":30}}")), "insert alice");
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"bob\",\"age\":25}}");
    json_free(r);
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"carol\",\"age\":40}}");
    json_free(r);
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"dave\",\"age\":20}}");
    json_free(r);

    /* duplicate pk rejected */
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"id\":10,\"name\":\"eve\"}}");
    check(!ok_code(r), "duplicate primary key rejected");
    json_free(r);

    /* --- select all / where --- */
    r = run(&db, "{\"op\":\"select\",\"table\":\"users\",\"columns\":\"*\"}");
    check(ok_code(r) && rows_of_result(r) == 4, "select * returns 4 rows");
    json_free(r);

    r = run(&db, "{\"op\":\"select\",\"table\":\"users\",\"where\":{\"age\":{\"gt\":25}}}");
    check(ok_code(r) && rows_of_result(r) == 2, "age > 25 matches 2");
    json_free(r);

    r = run(&db, "{\"op\":\"select\",\"table\":\"users\",\"where\":{\"and\":[{\"age\":{\"gte\":20}},{\"age\":{\"lte\":25}}]}}");
    check(ok_code(r) && rows_of_result(r) == 2, "20 <= age <= 25 matches 2");
    json_free(r);

    r = run(&db, "{\"op\":\"select\",\"table\":\"users\",\"where\":{\"or\":[{\"name\":\"alice\"},{\"name\":\"dave\"}]}}");
    check(ok_code(r) && rows_of_result(r) == 2, "name in {alice,dave} matches 2");
    json_free(r);

    /* order by + limit */
    r = run(&db, "{\"op\":\"select\",\"table\":\"users\",\"order_by\":[{\"column\":\"age\",\"desc\":true}],\"limit\":2}");
    JsonValue *rows = json_obj_get(r, "rows");
    JsonValue *top = rows && rows->nelems > 0 ? rows->elems[0] : NULL;
    JsonValue *topname = top ? json_obj_get(top, "name") : NULL;
    check(ok_code(r) && rows_of_result(r) == 2 &&
          topname && topname->type == JSON_STR && strcmp(topname->str, "carol") == 0,
          "order by age desc limit 2 -> carol first");
    json_free(r);

    /* --- update --- */
    r = run(&db, "{\"op\":\"update\",\"table\":\"users\",\"values\":{\"age\":26},\"where\":{\"name\":\"bob\"}}");
    check(ok_code(r), "update ok");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"users\",\"where\":{\"age\":26}}");
    check(ok_code(r) && rows_of_result(r) == 1, "bob now 26");
    json_free(r);

    /* --- delete --- */
    r = run(&db, "{\"op\":\"delete\",\"table\":\"users\",\"where\":{\"name\":\"dave\"}}");
    check(ok_code(r), "delete ok");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"users\"}");
    check(ok_code(r) && rows_of_result(r) == 3, "after delete 3 rows left");
    json_free(r);

    /* --- type safety --- */
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"oops\",\"age\":\"notanumber\"}}");
    check(!ok_code(r), "non-numeric age rejected");
    json_free(r);
    r = run(&db, "{\"op\":\"update\",\"table\":\"users\",\"values\":{\"nope\":1}}");
    check(!ok_code(r), "unknown column update rejected");
    json_free(r);

    /* --- show tables / describe --- */
    r = run(&db, "{\"op\":\"show_tables\"}");
    check(ok_code(r) && rows_of_result(r) == 1, "show_tables lists 1 table");
    json_free(r);
    r = run(&db, "{\"op\":\"describe\",\"table\":\"users\"}");
    JsonValue *cols = json_obj_get(r, "columns");
    JsonValue *rows2 = json_obj_get(r, "rows");
    check(ok_code(r) && cols && cols->nelems == 3 && rows2 && rows2->nelems == 3,
          "describe shows columns");
    json_free(r);

    /* --- unknown op / error op --- */
    r = run(&db, "{\"op\":\"nonsense\"}");
    check(!ok_code(r), "unknown op rejected");
    json_free(r);
    r = run(&db, "{\"op\":\"error\",\"message\":\"cannot do that\"}");
    check(!ok_code(r), "error op surfaces message");
    json_free(r);

    /* --- persistence round-trip --- */
    const char *tmp = "/tmp/isql_test.json";
    db.filename = strdup(tmp);
    check(db_save(&db) == NULL, "save ok");

    Database db2;
    db_init(&db2, tmp);
    check(db_load(&db2) == NULL, "load ok");
    r = run(&db2, "{\"op\":\"select\",\"table\":\"users\"}");
    check(ok_code(r) && rows_of_result(r) == 3, "reloaded db has 3 rows");
    JsonValue *schema = json_parse(db_schema_prompt(&db2));
    (void)schema;
    json_free(r);
    remove(tmp);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}