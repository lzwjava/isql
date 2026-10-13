#ifndef ISQL_DB_H
#define ISQL_DB_H

#include "json.h"

/* The whole database is a JSON tree, which makes load/save trivial and lets
 * the LLM's structured query plan drive the engine directly. */

struct Wal; /* forward declaration (wal.c owns the implementation) */
struct BtTree; /* primary-key index (btree.c) */

typedef struct {
    char *table;
    struct BtTree *tree; /* NULL = not built yet / dirty */
    int dirty;
} IndexSlot;

typedef struct {
    char *filename;      /* persistence file, or NULL for memory-only */
    JsonValue *tables;   /* array of table objects */
    struct Wal *wal;     /* write-ahead log (NULL when disabled/failed) */
    long wal_seq;        /* sequence of the last logged mutation */
    IndexSlot *idx;      /* primary-key indexes (lazily built caches) */
    int nidx;
    long idx_probes;     /* observability: pk lookups served by the index */
    long idx_builds;     /* observability: index (re)builds */
} Database;

void db_init(Database *db, const char *filename);

/* returns an error message string or NULL on success */
const char *db_load(Database *db);
const char *db_save(Database *db);  /* writes the snapshot only */

/* index stats exposure (used by EXPLAIN and tests) */
long db_index_probes(Database *db);
long db_index_builds(Database *db);
void db_index_invalidate_all(Database *db); /* after rollback/replay */
void db_index_invalidate_table(Database *db, const char *table);

/* plan analysis: schema validation + EXPLAIN cost estimates */
const char *db_validate_plan(Database *db, JsonValue *op);
int db_plan_source_rows(Database *db, JsonValue *plan);
int db_plan_uses_index(Database *db, JsonValue *plan);

/* WAL integration: attaches the log and replays entries newer than the
 * snapshot's seq. db_wal_apply applies a single log entry during recovery. */
const char *db_attach_wal(Database *db);
const char *db_wal_apply(Database *db, JsonValue *entry);
const char *db_checkpoint(Database *db); /* snapshot + truncate log */

/* Executes one JSON query plan ({"op": "...", ...}).
 * Returns a result object:
 *   {"ok": bool, "message": "...", "affected": n,
 *    "columns": [...], "rows": [...]}   (columns/rows present for SELECT/SHOW/DESCRIBE)
 * The caller owns the result and must json_free() it. */
JsonValue *db_exec(Database *db, JsonValue *op);

/* Human/machine readable schema summary, used to build the LLM system prompt. */
char *db_schema_prompt(Database *db);

#endif