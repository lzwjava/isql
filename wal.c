/* wal.c — write-ahead log for isql.
 *
 * File format: one JSON object per line:
 *   {"s":<seq>,"k":<kind>,"t":<table>, ...}
 * The snapshot file owns the checkpointed state; the WAL owns everything
 * since the last checkpoint. Recovery: load snapshot, replay WAL entries
 * whose seq is newer than the snapshot's seq.
 */
#include "wal.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WAL_LINE_MAX (1024 * 1024)

struct Wal {
    char *path;
    FILE *f;          /* open "a" */
    char *buf;        /* pending lines while a transaction is open */
    long buf_entries;
    int depth;        /* begin/commit nesting */
    long entries;     /* un-checkpointed entries (in file) */
};

Wal *wal_open(const char *snapshot_path)
{
    if (!snapshot_path) return NULL;
    Wal *w = calloc(1, sizeof *w);
    char path[4096];
    snprintf(path, sizeof path, "%s.wal", snapshot_path);
    w->path = strdup(path);
    w->f = fopen(path, "a");
    if (!w->f) { free(w->path); free(w); return NULL; }
    return w;
}

void wal_close(Wal *w)
{
    if (!w) return;
    if (w->f) fclose(w->f);
    free(w->buf);
    free(w->path);
    free(w);
}

const char *wal_begin(Wal *w)
{
    if (!w) return NULL;
    w->depth++;
    return NULL;
}

const char *wal_commit(Wal *w)
{
    if (!w) return NULL;
    if (w->depth > 1) { w->depth--; return NULL; } /* nested: not the outer commit */
    w->depth = 0;
    if (w->buf && w->buf[0]) {
        if (fputs(w->buf, w->f) == EOF) return "wal: write failed";
        w->entries += w->buf_entries;
        free(w->buf);
        w->buf = NULL;
        w->buf_entries = 0;
    }
    if (fflush(w->f) != 0) return "wal: flush failed";
    if (fsync(fileno(w->f)) != 0) return "wal: fsync failed";
    return NULL;
}

void wal_rollback(Wal *w)
{
    if (!w) return;
    w->depth = 0;
    free(w->buf);
    w->buf = NULL;
    w->buf_entries = 0;
}

const char *wal_append(Wal *w, const char *line)
{
    if (!w) return NULL;
    if (w->depth > 0) {
        /* inside a transaction: queue until commit */
        size_t cur = w->buf ? strlen(w->buf) : 0;
        w->buf = realloc(w->buf, cur + strlen(line) + 2);
        strcpy(w->buf + cur, line);
        strcpy(w->buf + cur + strlen(line), "\n");
        w->buf_entries++;
        return NULL;
    }
    if (fputs(line, w->f) == EOF) return "wal: write failed";
    if (fputc('\n', w->f) == EOF) return "wal: write failed";
    w->entries++;
    if (fflush(w->f) != 0) return "wal: flush failed";
    if (fsync(fileno(w->f)) != 0) return "wal: fsync failed";
    return NULL;
}

int wal_entries(const Wal *w)
{
    return w ? (int)w->entries : 0;
}

/* ---------------------------------------------------------------- */
/* replay                                                            */
/* ---------------------------------------------------------------- */

const char *wal_replay(Wal *w, Database *db)
{
    if (!w) return NULL;
    FILE *rf = fopen(w->path, "r");
    if (!rf) return NULL; /* no log yet */
    if (fseek(rf, 0, SEEK_END) != 0) { fclose(rf); return NULL; }
    long sz = ftell(rf);
    if (sz <= 0) { fclose(rf); return NULL; }
    fseek(rf, 0, SEEK_SET);
    char *data = malloc((size_t)sz + 1);
    if (!data) { fclose(rf); return "wal: out of memory"; }
    fread(data, 1, (size_t)sz, rf);
    fclose(rf);

    const char *err = NULL;
    size_t pos = 0;
    long replayed = 0;
    while (pos < (size_t)sz) {
        size_t nl = pos;
        while (nl < (size_t)sz && data[nl] != '\n') nl++;
        int is_last = (nl == (size_t)sz); /* no trailing newline at EOF */
        char *line = data + pos;
        size_t len = nl - pos;
        pos = nl + 1;
        if (len == 0) continue;

        JsonValue *entry = json_parse_len(line, (int)len);
        if (!entry) {
            /* tolerate a torn final line (crash while appending an entry) */
            if (is_last) break;
            err = "corrupt WAL: unparsable entry";
            break;
        }
        const char *aerr = db_wal_apply(db, entry); /* also advances db->wal_seq */
        json_free(entry);
        if (aerr) { err = aerr; break; }
        replayed++;
    }
    free(data);
    w->entries = replayed;
    return err;
}

const char *wal_checkpoint(Wal *w, Database *db)
{
    if (!w) return NULL;
    const char *err = db_save(db); /* snapshot becomes durable first */
    if (err) return err;
    /* now safe to clear the log (snapshot seq covers all logged entries) */
    if (w->buf) { free(w->buf); w->buf = NULL; w->buf_entries = 0; }
    fclose(w->f);
    w->f = fopen(w->path, "w");
    if (!w->f) return "wal: cannot truncate log";
    fclose(w->f);
    w->f = fopen(w->path, "a");
    if (!w->f) return "wal: cannot reopen log";
    w->entries = 0;
    return NULL;
}