#ifndef ISQL_DB_H
#define ISQL_DB_H

#include "json.h"

/* The whole database is a JSON tree, which makes load/save trivial and lets
 * the LLM's structured query plan drive the engine directly. */

typedef struct {
    char *filename;    /* persistence file, or NULL for memory-only */
    JsonValue *tables; /* array of table objects */
} Database;

void db_init(Database *db, const char *filename);

/* returns an error message string or NULL on success */
const char *db_load(Database *db);
const char *db_save(Database *db);

/* Executes one JSON query plan ({"op": "...", ...}).
 * Returns a result object:
 *   {"ok": bool, "message": "...", "affected": n,
 *    "columns": [...], "rows": [...]}   (columns/rows present for SELECT/SHOW/DESCRIBE)
 * The caller owns the result and must json_free() it. */
JsonValue *db_exec(Database *db, JsonValue *op);

/* Human/machine readable schema summary, used to build the LLM system prompt. */
char *db_schema_prompt(Database *db);

#endif