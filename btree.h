#ifndef ISQL_BTREE_H
#define ISQL_BTREE_H

#include "json.h"

/* In-memory B+ tree used as the primary-key index.
 *
 * This is deliberately small: it only supports insert, exact search and
 * max. Updates/deletes mark the owning index "dirty" and the tree is rebuilt
 * lazily from the table rows before the next indexed lookup, so no delete /
 * merge / compaction machinery is needed. The tree is a cache - correctness
 * never depends on it.
 *
 * Keys are doubles (pk columns are int/real). Values are row pointers
 * (JsonValue*), stored only in leaves. Internal nodes hold routing keys.
 */

typedef struct BtNode BtNode;
typedef struct BtTree BtTree;

BtTree *bt_create(void);
void bt_free(BtTree *t);

/* exact lookup; on success sets *row_out to the stored row and returns 1 */
int bt_search(BtTree *t, double key, JsonValue **row_out);

/* insert key -> row, replacing any previous row at the same key */
void bt_insert(BtTree *t, double key, JsonValue *row);

/* largest key in the tree; returns 0 and leaves *out untouched when empty */
double bt_max(const BtTree *t, int *found);

int bt_size(const BtTree *t);
long bt_lookups(const BtTree *t);

#endif