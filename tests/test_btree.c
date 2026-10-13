/* test_btree.c — direct unit tests for the B+ tree (no database involved). */
#include "../btree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
}

int main(void)
{
    /* --- ascending inserts (heavy root/leaf splitting) --- */
    BtTree *t = bt_create();
    int N = 100000;
    for (int i = 0; i < N; i++) bt_insert(t, (double)i, (JsonValue*)(long)(i + 1));
    check(bt_size(t) == N, "ascending: size");
    JsonValue *row = NULL;
    int bad = 0;
    for (int i = 0; i < N; i++)
        if (!bt_search(t, (double)i, &row) || (long)row - 1 != i) bad++;
    check(bad == 0, "ascending: all present");
    int found = 0;
    check(bt_max(t, &found) == N - 1 && found, "ascending: max");
    bt_free(t);

    /* --- descending inserts --- */
    t = bt_create();
    for (int i = N - 1; i >= 0; i--) bt_insert(t, (double)i, (JsonValue*)(long)(i + 1));
    bad = 0;
    for (int i = 0; i < N; i++)
        if (!bt_search(t, (double)i, &row) || (long)row - 1 != i) bad++;
    check(bad == 0, "descending: all present");

    /* --- duplicate keys replace in place (the shift-scan regression) --- */
    check(bt_search(t, 5, &row) && (long)row - 1 == 5, "dupes: original value before replace");
    bt_insert(t, 5, (JsonValue*)(long)(999));
    check(bt_search(t, 5, &row) && (long)row == 999, "dupes: value replaced");
    check(bt_size(t) == N, "dupes: size unchanged after replace");
    bt_free(t);

    /* --- random order + missing keys --- */
    t = bt_create();
    unsigned s = 987654321, seen[40000] = {0};
    int uniq = 0;
    for (int k = 0; k < 50000; k++) {
        s = s * 1103515245u + 12345u;
        double key = (double)(s % 40000);
        if (!seen[(int)key]) { seen[(int)key] = 1; uniq++; }
        bt_insert(t, key, (JsonValue*)(long)(key + 1));
    }
    bad = 0;
    for (int i = 0; i < 40000; i++) {
        int present = bt_search(t, (double)i, &row);
        if (seen[i] && (!present || (long)row - 1 != i)) bad++;
        if (!seen[i] && present) bad++;
    }
    check(bad == 0 && bt_size(t) == uniq, "random: order-insensitive correctness");
    check(bt_lookups(t) > 0, "random: lookups were counted");
    bt_free(t);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}