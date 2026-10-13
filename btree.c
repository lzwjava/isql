/* btree.c — a minimal B+ tree (primary-key index).
 *
 * Layout: every node holds up to 2T-1 keys; children up to 2T. Values live
 * only in leaf nodes. insert() splits full nodes on the way down (B+ copy-up
 * for leaf splits, move-up for internal); search() descends to a leaf.
 */
#include "btree.h"

#include <stdlib.h>

#define T 64 /* min degree: >=64 keys per node, very shallow trees */

#define MAXK (2 * T - 1)
#define MAXC (2 * T)

struct BtNode {
    int n;          /* number of keys in this node */
    int leaf;       /* 1 for leaves */
    double *keys;   /* [MAXK] */
    JsonValue **vals; /* [MAXK] leaf values (NULL in internal nodes) */
    BtNode **child; /* [MAXC] internal children (NULL in leaves) */
};

struct BtTree {
    BtNode *root;
    int size;
    long lookups;
};

static BtNode *node_new(int leaf)
{
    BtNode *n = calloc(1, sizeof *n);
    n->leaf = leaf;
    n->keys = malloc(sizeof(double) * MAXK);
    n->vals = calloc((size_t)MAXK, sizeof(JsonValue *));
    if (!leaf) n->child = calloc((size_t)MAXC, sizeof(BtNode *));
    return n;
}

static void node_free(BtNode *n)
{
    if (!n) return;
    if (!n->leaf) {
        for (int i = 0; i <= n->n; i++)
            node_free(n->child[i]);
    }
    free(n->child);
    free(n->vals);
    free(n->keys);
    free(n);
}

BtTree *bt_create(void)
{
    BtTree *t = calloc(1, sizeof *t);
    t->root = node_new(1);
    return t;
}

void bt_free(BtTree *t)
{
    if (!t) return;
    node_free(t->root);
    free(t);
}

int bt_size(const BtTree *t)
{
    return t ? t->size : 0;
}

long bt_lookups(const BtTree *t)
{
    return t ? t->lookups : 0;
}

int bt_search(BtTree *t, double key, JsonValue **row_out)
{
    if (!t || !t->root) return 0;
    t->lookups++;
    BtNode *n = t->root;
    while (!n->leaf) {
        /* descend: the first child whose routing key is > key holds <key>;
         * otherwise take the last child */
        int i = 0;
        while (i < n->n && !(key < n->keys[i])) i++;
        n = n->child[i];
    }
    /* binary search in the leaf */
    int lo = 0, hi = n->n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (n->keys[mid] == key) {
            if (row_out) *row_out = n->vals[mid];
            return 1;
        }
        if (n->keys[mid] < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

/* split child y = x->child[i]; assumes y is full (n == MAXK) */
static void split_child(BtNode *x, int i, BtNode *y)
{
    BtNode *z = node_new(y->leaf);

    if (y->leaf) {
        /* B+ copy-up for leaves: the median (y->keys[T-1]) is z's LOWEST key,
         * so the parent routing key points at the sub-tree that contains it.
         * z therefore holds T keys: y->keys[T-1 .. 2T-2]. */
        for (int j = 0; j < T; j++) {
            z->keys[j] = y->keys[j + T - 1];
            z->vals[j] = y->vals[j + T - 1];
        }
        z->n = T;
    } else {
        /* B+ move-up for internal nodes: z takes the T-1 keys above the
         * median and their T children; the median itself moves to x.
         * (a node with k keys has k+1 children, so a full node's children
         * are indexed 0..2T-1) */
        for (int j = 0; j < T - 1; j++) {
            z->keys[j] = y->keys[j + T];
            z->vals[j] = NULL;
        }
        for (int j = 0; j < T; j++)
            z->child[j] = y->child[j + T];
        z->n = T - 1;
    }

    /* y keeps the T-1 smallest keys */
    y->n = T - 1;

    /* make room for z in x */
    for (int j = x->n + 1; j > i + 1; j--)
        x->child[j] = x->child[j - 1];
    x->child[i + 1] = z;

    for (int j = x->n; j > i; j--) {
        x->keys[j] = x->keys[j - 1];
        x->vals[j] = x->vals[j - 1];
    }
    /* the median becomes the routing key; internal nodes carry no values */
    x->keys[i] = y->keys[T - 1];
    x->vals[i] = NULL;
    x->n++;
}

/* returns 1 if a new key was inserted, 0 if an existing key was replaced */
static int insert_nonfull(BtNode *n, double key, JsonValue *row)
{
    int i = n->n - 1;
    if (n->leaf) {
        /* find the insertion slot first: i = last index with keys[i] <= key */
        while (i >= 0 && key < n->keys[i]) i--;
        if (i >= 0 && n->keys[i] == key) {
            n->vals[i] = row; /* replacing an existing key keeps the size */
            return 0;
        }
        for (int j = n->n - 1; j >= i + 1; j--) {
            n->keys[j + 1] = n->keys[j];
            n->vals[j + 1] = n->vals[j];
        }
        n->keys[i + 1] = key;
        n->vals[i + 1] = row;
        n->n++;
        return 1;
    }
    while (i >= 0 && key < n->keys[i]) i--;
    i++;
    if (n->child[i] && n->child[i]->n == MAXK) {
        split_child(n, i, n->child[i]);
        /* split_child just promoted the median into keys[i]; keys >= it now
         * live in child[i+1] (B+ copy-up keeps the median in the right
         * leaf), so steer the insertion there. */
        if (key >= n->keys[i]) i++;
    }
    return insert_nonfull(n->child[i], key, row);
}

void bt_insert(BtTree *t, double key, JsonValue *row)
{
    if (!t) return;
    if (t->root->n == MAXK) {
        BtNode *s = node_new(0);
        s->child[0] = t->root;
        split_child(s, 0, t->root);
        t->root = s;
    }
    if (insert_nonfull(t->root, key, row)) t->size++;
}

double bt_max(const BtTree *t, int *found)
{
    *found = 0;
    if (!t || !t->root || t->size == 0) return 0;
    BtNode *n = t->root;
    while (!n->leaf) n = n->child[n->n];
    if (n->n == 0) return 0;
    *found = 1;
    return n->keys[n->n - 1];
}