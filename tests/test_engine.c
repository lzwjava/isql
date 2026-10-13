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

    /* ================= Phase 3: WAL durability & crash recovery =========== */
    const char *tmp = "/tmp/isql_test.json";
    remove(tmp);
    remove("/tmp/isql_test.json.wal");
    db.filename = strdup(tmp);

    /* snapshot of current state (3 users), then attach the WAL */
    check(db_save(&db) == NULL, "snapshot save ok");
    check(db_attach_wal(&db) == NULL, "wal attach ok");

    /* mutation goes to the WAL only (no snapshot rewrite) */
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"walman\",\"age\":99}}");
    check(ok_code(r), "insert with wal ok");
    json_free(r);

    /* --- simulated crash: fresh process loads snapshot + replays WAL --- */
    Database db3;
    db_init(&db3, tmp);
    check(db_load(&db3) == NULL, "crash: snapshot loads");
    check(db_attach_wal(&db3) == NULL, "crash: wal replays");
    r = run(&db3, "{\"op\":\"select\",\"table\":\"users\"}");
    check(ok_code(r) && rows_of_result(r) == 4, "crash recovery: 4 users (3 snapshot + 1 wal)");
    json_free(r);
    r = run(&db3, "{\"op\":\"select\",\"table\":\"users\",\"where\":{\"name\":\"walman\"}}");
    check(ok_code(r) && rows_of_result(r) == 1, "crash recovery: walman present after replay");
    json_free(r);

    /* --- rollback: a failed batch must NOT leave wal entries behind --- */
    r = run(&db, "{\"ops\":[{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"txn1\",\"age\":1}},"
                   "{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"txn2\",\"age\":2,\"id\":14}}]}"); /* id 14 = walman, duplicate */
    check(!ok_code(r), "failing batch rejected");
    json_free(r);
    r = run(&db, "{\"op\":\"create_table\",\"table\":\"probe\",\"columns\":[{\"name\":\"x\",\"type\":\"int\"}]}");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"users\"}");
    check(ok_code(r) && rows_of_result(r) == 4, "in-memory rollback: still 4 users");
    json_free(r);

    /* --- successful batch IS durable in the wal --- */
    r = run(&db, "{\"ops\":[{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"x1\",\"age\":1}},"
                   "{\"op\":\"insert\",\"table\":\"users\",\"values\":{\"name\":\"x2\",\"age\":2}}]}");
    check(ok_code(r), "successful batch ok");
    json_free(r);
    r = run(&db, "{\"op\":\"drop_table\",\"table\":\"probe\"}");
    json_free(r);

    /* --- second crash: txn1 must be gone, x1/x2 present --- */
    Database db4;
    db_init(&db4, tmp);
    check(db_load(&db4) == NULL, "crash2: snapshot loads");
    check(db_attach_wal(&db4) == NULL, "crash2: wal replays");
    r = run(&db4, "{\"op\":\"select\",\"table\":\"users\"}");
    check(ok_code(r) && rows_of_result(r) == 6, "crash2: 6 users (4 + batch of 2)");
    json_free(r);
    r = run(&db4, "{\"op\":\"select\",\"table\":\"users\",\"where\":{\"name\":\"txn1\"}}");
    check(ok_code(r) && rows_of_result(r) == 0, "crash2: rolled-back txn1 never persisted");
    json_free(r);
    r = run(&db4, "{\"op\":\"select\",\"table\":\"users\",\"where\":{\"name\":\"x2\"}}");
    check(ok_code(r) && rows_of_result(r) == 1, "crash2: committed x2 survives");
    json_free(r);
    r = run(&db4, "{\"op\":\"select\",\"table\":\"probe\"}");
    check(!ok_code(r), "crash2: drop_table replayed (probe gone)");
    json_free(r);

    /* --- checkpoint truncates the log: restart replays nothing new --- */
    check(db_checkpoint(&db4) == NULL, "checkpoint ok");
    Database db5;
    db_init(&db5, tmp);
    check(db_load(&db5) == NULL, "after checkpoint: snapshot loads");
    check(db_attach_wal(&db5) == NULL, "after checkpoint: wal attaches");
    r = run(&db5, "{\"op\":\"select\",\"table\":\"users\"}");
    check(ok_code(r) && rows_of_result(r) == 6, "after checkpoint: still 6 users");
    json_free(r);

    remove(tmp);
    remove("/tmp/isql_test.json.wal");


    /* ================= Phase 2: operators, JOIN, aggregate, ALTER, batch ===== */

    /* --- new tables for phase 2 --- */
    check(ok_code(run(&db, "{\"op\":\"create_table\",\"table\":\"users2\",\"columns\":[{\"name\":\"id\",\"type\":\"int\",\"primary\":true},{\"name\":\"name\",\"type\":\"text\"},{\"name\":\"age\",\"type\":\"int\"},{\"name\":\"city\",\"type\":\"text\"}]}")), "create users2");
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users2\",\"values\":{\"id\":1,\"name\":\"alice\",\"age\":30,\"city\":\"nyc\"}}"); json_free(r);
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users2\",\"values\":{\"id\":2,\"name\":\"bob\",\"age\":25,\"city\":\"sf\"}}"); json_free(r);
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users2\",\"values\":{\"id\":3,\"name\":\"carol\",\"age\":40,\"city\":\"nyc\"}}"); json_free(r);
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users2\",\"values\":{\"id\":4,\"name\":\"anna\",\"age\":22,\"city\":\"sf\"}}"); json_free(r);
    r = run(&db, "{\"op\":\"insert\",\"table\":\"users2\",\"values\":{\"id\":5,\"name\":\"dave\",\"age\":28,\"city\":\"la\"}}"); json_free(r);

    /* --- LIKE --- */
    r = run(&db, "{\"op\":\"select\",\"table\":\"users2\",\"where\":{\"name\":{\"like\":\"a%\"}}}");
    check(ok_code(r) && rows_of_result(r) == 2, "like 'a%' matches alice, anna");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"users2\",\"where\":{\"name\":{\"like\":\"_ob\"}}}");
    check(ok_code(r) && rows_of_result(r) == 1, "like '_ob' matches bob");
    json_free(r);

    /* --- IN --- */
    r = run(&db, "{\"op\":\"select\",\"table\":\"users2\",\"where\":{\"city\":{\"in\":[\"nyc\",\"la\"]}}}");
    check(ok_code(r) && rows_of_result(r) == 3, "city in [nyc,la] -> 3");
    json_free(r);

    /* --- BETWEEN --- */
    r = run(&db, "{\"op\":\"select\",\"table\":\"users2\",\"where\":{\"age\":{\"between\":[25,30]}}}");
    check(ok_code(r) && rows_of_result(r) == 3, "age between 25 and 30 -> 3");
    json_free(r);

    /* --- DISTINCT --- */
    r = run(&db, "{\"op\":\"select\",\"table\":\"users2\",\"columns\":[\"city\"],\"distinct\":true}");
    check(ok_code(r) && rows_of_result(r) == 3, "distinct city -> 3");
    json_free(r);

    /* --- ALTER add + drop column --- */
    r = run(&db, "{\"op\":\"alter_table\",\"table\":\"users2\",\"action\":\"add_column\",\"column\":{\"name\":\"email\",\"type\":\"text\"}}");
    check(ok_code(r), "alter add_column ok");
    json_free(r);
    r = run(&db, "{\"op\":\"update\",\"table\":\"users2\",\"values\":{\"email\":\"a@x.com\"},\"where\":{\"name\":\"alice\"}}");
    check(ok_code(r), "update new column ok");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"users2\",\"where\":{\"email\":\"a@x.com\"}}");
    check(ok_code(r) && rows_of_result(r) == 1, "filter on new column works");
    json_free(r);
    r = run(&db, "{\"op\":\"alter_table\",\"table\":\"users2\",\"action\":\"drop_column\",\"column\":\"email\"}");
    check(ok_code(r), "alter drop_column ok");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"users2\",\"columns\":[\"email\"]}");
    check(!ok_code(r), "dropped column no longer selectable");
    json_free(r);

    /* --- AGGREGATE --- */
    r = run(&db, "{\"op\":\"aggregate\",\"table\":\"users2\",\"aggs\":[{\"as\":\"n\",\"func\":\"count\",\"column\":\"*\"},{\"as\":\"total\",\"func\":\"sum\",\"column\":\"age\"},{\"as\":\"avg\",\"func\":\"avg\",\"column\":\"age\"},{\"as\":\"oldest\",\"func\":\"max\",\"column\":\"age\"}]}");
    check(ok_code(r) && rows_of_result(r) == 1, "aggregate no group -> 1 row");
    JsonValue *a0 = rows_of_result(r) > 0 ? json_obj_get(json_obj_get(r, "rows")->elems[0], "n") : NULL;
    JsonValue *a1 = rows_of_result(r) > 0 ? json_obj_get(json_obj_get(r, "rows")->elems[0], "total") : NULL;
    JsonValue *a2 = rows_of_result(r) > 0 ? json_obj_get(json_obj_get(r, "rows")->elems[0], "oldest") : NULL;
    check(a0 && a0->num == 5, "count(*) == 5");
    check(a1 && a1->num == 145, "sum(age) == 145");
    check(a2 && a2->num == 40, "max(age) == 40");
    json_free(r);

    r = run(&db, "{\"op\":\"aggregate\",\"table\":\"users2\",\"group_by\":[\"city\"],\"aggs\":[{\"as\":\"n\",\"func\":\"count\",\"column\":\"*\"},{\"as\":\"avg\",\"func\":\"avg\",\"column\":\"age\"}]}");
    check(ok_code(r) && rows_of_result(r) == 3, "aggregate group by city -> 3 groups");
    json_free(r);
    /* empty table aggregate returns 0 on count */
    r = run(&db, "{\"op\":\"create_table\",\"table\":\"empty\",\"columns\":[{\"name\":\"x\",\"type\":\"int\"}]}");
    json_free(r);
    r = run(&db, "{\"op\":\"aggregate\",\"table\":\"empty\",\"aggs\":[{\"as\":\"n\",\"func\":\"count\",\"column\":\"*\"}]}");
    JsonValue *n0 = rows_of_result(r) > 0 ? json_obj_get(json_obj_get(r, "rows")->elems[0], "n") : NULL;
    check(ok_code(r) && n0 && n0->num == 0, "count on empty table == 0");
    json_free(r);

    /* --- JOIN --- */
    check(ok_code(run(&db, "{\"op\":\"create_table\",\"table\":\"orders\",\"columns\":[{\"name\":\"id\",\"type\":\"int\",\"primary\":true},{\"name\":\"user_id\",\"type\":\"int\"},{\"name\":\"total\",\"type\":\"real\"}]}")), "create orders");
    r = run(&db, "{\"op\":\"insert\",\"table\":\"orders\",\"values\":{\"id\":1,\"user_id\":1,\"total\":150.0}}"); json_free(r);
    r = run(&db, "{\"op\":\"insert\",\"table\":\"orders\",\"values\":{\"id\":2,\"user_id\":1,\"total\":30.0}}"); json_free(r);
    r = run(&db, "{\"op\":\"insert\",\"table\":\"orders\",\"values\":{\"id\":3,\"user_id\":3,\"total\":90.0}}"); json_free(r);
    r = run(&db, "{\"op\":\"insert\",\"table\":\"orders\",\"values\":{\"id\":4,\"user_id\":99,\"total\":500.0}}"); json_free(r);

    r = run(&db, "{\"op\":\"select\",\"table\":\"orders\",\"join\":{\"table\":\"users2\",\"on\":{\"users2.id\":\"orders.user_id\"}},\"columns\":[\"users2.name\",\"orders.total\"]}");
    check(ok_code(r) && rows_of_result(r) == 3, "inner join -> 3 rows (user 99 has no match)");
    JsonValue *jr = rows_of_result(r) > 0 ? json_obj_get(json_obj_get(r, "rows")->elems[0], "users2.name") : NULL;
    check(jr && jr->type == JSON_STR && strcmp(jr->str, "alice") == 0, "join first row name = alice");
    json_free(r);

    /* join + where on joined columns + order */
    r = run(&db, "{\"op\":\"select\",\"table\":\"orders\",\"join\":{\"table\":\"users2\",\"on\":{\"users2.id\":\"orders.user_id\"}},\"columns\":[\"users2.name\",\"orders.total\"],\"where\":{\"orders.total\":{\"gt\":80}},\"order_by\":[{\"column\":\"orders.total\",\"desc\":true}]}");
    check(ok_code(r) && rows_of_result(r) == 2, "join where total>80 -> 2");
    JsonValue *jtop = rows_of_result(r) > 0 ? json_obj_get(json_obj_get(r, "rows")->elems[0], "orders.total") : NULL;
    check(jtop && jtop->type == JSON_NUM && jtop->num == 150, "join order by total desc -> 150 first");
    json_free(r);

    /* --- BATCH: success keeps changes --- */
    r = run(&db, "{\"ops\":[{\"op\":\"insert\",\"table\":\"orders\",\"values\":{\"id\":5,\"user_id\":2,\"total\":10.0}},{\"op\":\"insert\",\"table\":\"orders\",\"values\":{\"id\":6,\"user_id\":4,\"total\":20.0}}]}");
    check(ok_code(r) && r->npairs && json_obj_get(r, "items") != NULL, "batch ok has items");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"orders\"}");
    check(ok_code(r) && rows_of_result(r) == 6, "after successful batch -> 6 orders");
    json_free(r);

    /* --- BATCH: on failure rolls back the earlier op --- */
    r = run(&db, "{\"ops\":[{\"op\":\"insert\",\"table\":\"orders\",\"values\":{\"id\":7,\"user_id\":1,\"total\":1.0}},{\"op\":\"insert\",\"table\":\"orders\",\"values\":{\"id\":7,\"user_id\":2,\"total\":2.0}}]}");
    check(!ok_code(r), "batch with duplicate pk fails");
    JsonValue *rbv = json_obj_get(r, "rolled_back");
    check(rbv && rbv->type == JSON_BOOL && rbv->boolean, "rolled_back flag set");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"orders\"}");
    check(ok_code(r) && rows_of_result(r) == 6, "rollback keeps order count at 6");
    json_free(r);

    /* ================= Phase 4: primary-key B+ index ===================== */
    check(ok_code(run(&db,"{\"op\":\"create_table\",\"table\":\"people\",\"columns\":[{\"name\":\"id\",\"type\":\"int\",\"primary\":true},{\"name\":\"name\",\"type\":\"text\"},{\"name\":\"city\",\"type\":\"text\"}]}")), "create people");
    for (int i = 1; i <= 200; i++) {
        char plan[512];
        snprintf(plan, sizeof plan,
                 "{\"op\":\"insert\",\"table\":\"people\",\"values\":{\"id\":%d,\"name\":\"n%d\",\"city\":\"c%d\"}}",
                 i, i, i % 5);
        r = run(&db, plan);
        json_free(r);
    }
    r = run(&db, "{\"op\":\"select\",\"table\":\"people\",\"where\":{\"id\":42}}");
    check(ok_code(r) && rows_of_result(r) == 1, "pk-equality select finds its row");
    check(db.idx_probes > 0, "pk-equality query actually used the index");
    JsonValue *row42 = rows_of_result(r) > 0 ? json_obj_get(json_obj_get(r, "rows")->elems[0], "city") : NULL;
    check(row42 && row42->type == JSON_STR && strcmp(row42->str, "c2") == 0, "indexed lookup returned the right row");
    json_free(r);

    /* update by pk; index stays consistent */
    r = run(&db, "{\"op\":\"update\",\"table\":\"people\",\"values\":{\"city\":\"zz\"},\"where\":{\"id\":42}}");
    check(ok_code(r), "update by pk ok");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"people\",\"where\":{\"id\":42}}");
    JsonValue *row42b = rows_of_result(r) > 0 ? json_obj_get(json_obj_get(r, "rows")->elems[0], "city") : NULL;
    check(row42b && row42b->type == JSON_STR && strcmp(row42b->str, "zz") == 0, "index still points at the updated row");
    json_free(r);

    /* duplicate pk rejected via the index fast path */
    r = run(&db, "{\"op\":\"insert\",\"table\":\"people\",\"values\":{\"id\":42,\"name\":\"x\"}}");
    check(!ok_code(r), "duplicate pk rejected (index conflict check)");
    json_free(r);

    /* auto-increment via bt_max continues after the indexed inserts */
    r = run(&db, "{\"op\":\"insert\",\"table\":\"people\",\"values\":{\"name\":\"last\",\"city\":\"z\"}}");
    check(ok_code(r), "auto-pk insert ok");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"people\",\"where\":{\"name\":\"last\"}}");
    JsonValue *lastid = rows_of_result(r) > 0 ? json_obj_get(json_obj_get(r, "rows")->elems[0], "id") : NULL;
    check(lastid && lastid->type == JSON_NUM && lastid->num == 201, "auto-increment continued via index max (id 201)");
    json_free(r);

    /* non-pk predicates do NOT touch the index (full scan) */
    long probes_before = db.idx_probes;
    r = run(&db, "{\"op\":\"select\",\"table\":\"people\",\"where\":{\"city\":\"c1\"}}");
    check(ok_code(r) && rows_of_result(r) == 40, "non-pk full scan still correct (40 rows)");
    check(db.idx_probes == probes_before, "non-pk predicate used no index probes");
    json_free(r);

    /* delete + re-probe stays correct */
    r = run(&db, "{\"op\":\"delete\",\"table\":\"people\",\"where\":{\"id\":1}}");
    check(ok_code(r), "delete by pk ok");
    json_free(r);
    r = run(&db, "{\"op\":\"select\",\"table\":\"people\",\"where\":{\"id\":1}}");
    check(ok_code(r) && rows_of_result(r) == 0, "deleted row gone via index");
    json_free(r);

    /* ================= Phase 5: plan validator =========================== */
    JsonValue *bad = json_parse("{\"op\":\"select\",\"table\":\"nope\",\"where\":{\"id\":1}}");
    check(db_validate_plan(&db, bad) != NULL, "validator rejects an unknown table");
    json_free(bad);
    bad = json_parse("{\"op\":\"insert\",\"table\":\"people\",\"values\":{\"bogus\":1}}");
    check(db_validate_plan(&db, bad) != NULL, "validator rejects unknown column in values");
    json_free(bad);
    bad = json_parse("{\"op\":\"select\",\"table\":\"people\",\"columns\":[\"id\"],\"where\":{\"id\":1}}");
    check(db_validate_plan(&db, bad) == NULL, "validator accepts a valid plan");
    json_free(bad);
    bad = json_parse("{\"op\":\"select\",\"table\":\"people\",\"where\":{\"and\":[{\"id\":1},{\"city\":\"c1\"}]}}");
    check(db_validate_plan(&db, bad) == NULL, "validator accepts nested and/of conditions");
    json_free(bad);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}