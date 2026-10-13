/* bench.c — micro benchmark for the pk index (full scan vs B+ lookup).
 *
 *   make bench && ./bench [N]      # default N = 200000
 */
#include "../db.h"
#include "../json.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static JsonValue *run(Database *db, const char *j)
{
    JsonValue *p = json_parse(j);
    JsonValue *r = db_exec(db, p);
    json_free(p);
    return r;
}

static double ms(void)
{
    return (double)clock() / CLOCKS_PER_SEC * 1000.0;
}

static double clock_now(void) { return ms(); }

int main(int argc, char **argv)
{
    int N = argc > 1 ? atoi(argv[1]) : 50000;
    if (N < 100) N = 100;
    Database db;
    db_init(&db, NULL);
    run(&db, "{\"op\":\"create_table\",\"table\":\"big\",\"columns\":["
             "{\"name\":\"id\",\"type\":\"int\",\"primary\":true},"
             "{\"name\":\"name\",\"type\":\"text\"},"
             "{\"name\":\"val\",\"type\":\"real\"}]}");

    /* ---- inserts (index maintained incrementally) ---- */
    double t0 = clock_now();
    for (int i = 0; i < N; i++) {
        char plan[256];
        snprintf(plan, sizeof plan,
                 "{\"op\":\"insert\",\"table\":\"big\",\"values\":{\"id\":%d,\"name\":\"row%06d\",\"val\":%d.5}}",
                 i, i, i);
        JsonValue *r = run(&db, plan);
        json_free(r);
    }
    double t1 = clock_now();
    printf("insert %6d rows : %8.1f ms  (%.1f us/row, pk-conflict via B+)\n",
           N, t1 - t0, (t1 - t0) * 1000.0 / N);

    /* ---- indexed pk lookups ---- */
    char plan[256];
    t0 = clock_now();
    for (int i = 0; i < N; i++) {
        snprintf(plan, sizeof plan, "{\"op\":\"select\",\"table\":\"big\",\"where\":{\"id\":%d}}", i);
        JsonValue *r = run(&db, plan);
        json_free(r);
    }
    t1 = clock_now();
    printf("pk select  %6d rows : %8.1f ms  (%.2f us/lookup, B+ tree, %ld probes)\n",
           N, t1 - t0, (t1 - t0) * 1000.0 / N, db.idx_probes);

    /* ---- full scans on a non-indexed column (a handful, for time) ---- */
    int M = 100;
    t0 = clock_now();
    for (int k = 0; k < M; k++) {
        snprintf(plan, sizeof plan, "{\"op\":\"select\",\"table\":\"big\",\"where\":{\"val\":{\"gte\":%d.5}}}", (N / 10) + k);
        JsonValue *r = run(&db, plan);
        json_free(r);
    }
    t1 = clock_now();
    printf("scan(where)      : %8.1f ms for %d full scans of %d rows each\n",
           t1 - t0, M, N);

    /* ---- joins are nested-loop by design ---- */
    printf("\nindex stats: %ld probes served, %ld builds (lazy rebuilds)\n",
           db.idx_probes, db.idx_builds);
    return 0;
}