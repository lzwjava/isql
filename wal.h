#ifndef ISQL_WAL_H
#define ISQL_WAL_H

#include "db.h"

/* Write-ahead log: every mutation is appended to "<snapshot>.wal" as a line of
 * JSON ({"s":seq,"k":kind,...}) and fsync'ed before the mutation is confirmed.
 * A snapshot file written by checkpointing carries "seq" = the highest WAL
 * sequence included, so crash recovery replays only newer entries.
 *
 * Entries written between wal_begin()/wal_commit() are buffered and written
 * atomically on commit; wal_rollback() discards them (this is how both
 * {"ops":[...]} batches and explicit BEGIN/COMMIT transactions stay atomic).
 */

typedef struct Wal Wal; /* opaque */

/* opens (or creates) the WAL for a snapshot file; NULL if unavailable */
Wal *wal_open(const char *snapshot_path);
void wal_close(Wal *w);

/* transactional buffering */
const char *wal_begin(Wal *w);   /* nested-safe; increments depth */
const char *wal_commit(Wal *w);  /* flush pending entries + fsync at depth 0 */
void wal_rollback(Wal *w);       /* discard pending entries, reset depth */

/* appends one durable line (or queues it while a transaction is open);
 * returns an error string or NULL. Caller must not free the line. */
const char *wal_append(Wal *w, const char *line);

/* number of un-checkpointed entries */
int wal_entries(const Wal *w);

/* replays the log (calling db_wal_apply for each entry) into the database;
 * entries already covered by the snapshot's seq are skipped. */
const char *wal_replay(Wal *w, Database *db);

/* checkpoint: write the snapshot (via db_save) then truncate the log.
 * Order matters: the snapshot is durable BEFORE the log is cleared, so a
 * crash in between just replays entries that are now no-ops by seq. */
const char *wal_checkpoint(Wal *w, Database *db);

#endif