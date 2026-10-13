/* test_sse.c — unit tests for the SSE stream parser (no network needed). */
#include "../llm_sse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;
static void check(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
}

typedef struct { char *got; } Ctx;

static void capture(const char *delta, void *arg)
{
    Ctx *c = (Ctx *)arg;
    c->got = c->got ? strcat(c->got, delta) : strdup(delta);
}

int main(void)
{
    /* 1) whole event in one feed() call */
    {
        SseState *st = sse_state_new();
        Ctx c = { NULL };
        char *wire =
            "data: {\"choices\":[{\"delta\":{\"content\":\"{\\\"op\\\":\\\"sel" /* content starts */
            "ect\\\"}\"}}]}\n\n"
            "data: [DONE]\n\n";
        sse_feed(st, wire, (int)strlen(wire), capture, &c);
        check(st->finished, "event: [DONE] marks the stream finished");
        check(c.got && strcmp(c.got, "{\"op\":\"select\"}") == 0, "event: content extracted");
        sse_state_free(st);
        free(c.got);
    }

    /* 2) byte-at-a-time delivery (curl chunk boundaries) */
    {
        SseState *st = sse_state_new();
        Ctx c = { NULL };
        const char *w1 = "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\n\n";
        const char *w2 = "data: {\"choices\":[{\"delta\":{\"content\":\"bc\"}}]}\n\n";
        const char *w3 = "data: [DONE]\n\n";
        const char *all = "\0";
        (void)all;
        for (int i = 0; i < (int)strlen(w1); i++) sse_feed(st, &w1[i], 1, capture, &c);
        for (int i = 0; i < (int)strlen(w2); i++) sse_feed(st, &w2[i], 1, capture, &c);
        sse_feed(st, w3, (int)strlen(w3), capture, &c);
        check(st->finished, "chunks: finished");
        check(c.got && strcmp(c.got, "abc") == 0, "chunks: content reassembled across boundaries");
        sse_state_free(st);
        free(c.got);
    }

    /* 3) mid-line split across feeds */
    {
        SseState *st = sse_state_new();
        Ctx c = { NULL };
        const char *part1 = "data: {\"choices\":[{\"delta\":{\"content\":\"x";
        const char *part2 = "y\"}}]}\n\ndata: [DONE]\n\n";
        sse_feed(st, part1, (int)strlen(part1), capture, &c);
        sse_feed(st, part2, (int)strlen(part2), capture, &c);
        check(c.got && strcmp(c.got, "xy") == 0, "split-line: content split across feeds reassembled");
        sse_state_free(st);
        free(c.got);
    }

    /* 4) role-only deltas and comments are ignored */
    {
        SseState *st = sse_state_new();
        Ctx c = { NULL };
        sse_feed(st, ": keep-alive comment\n", (int)strlen(": keep-alive comment\n"), capture, &c);
        sse_feed(st, "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n",
                 (int)strlen("data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"), capture, &c);
        sse_feed(st, "event: ping\ndata: {\"choices\":[{\"delta\":{\"content\":\"z\"}}]}\n\n",
                 (int)strlen("event: ping\ndata: {\"choices\":[{\"delta\":{\"content\":\"z\"}}]}\n\n"), capture, &c);
        sse_feed(st, "data: [DONE]\n\n", (int)strlen("data: [DONE]\n\n"), capture, &c);
        check(c.got && strcmp(c.got, "z") == 0, "ignore: comments and role-only deltas skipped");
        sse_state_free(st);
        free(c.got);
    }

    /* 5) stream error surfaces */
    {
        SseState *st = sse_state_new();
        const char *errwire = "data: {\"error\":{\"message\":\"rate limited\"}}\n\ndata: [DONE]\n\n";
        sse_feed(st, errwire, (int)strlen(errwire), capture, NULL);
        check(st->err && strcmp(st->err, "rate limited") == 0, "error: message captured");
        sse_state_free(st);
    }

    /* 6) non-SSE garbage produces no deltas and hands control back */
    {
        SseState *st = sse_state_new();
        Ctx c = { NULL };
        const char *plain = "{\"choices\":[{\"message\":{\"content\":\"plain\"}}]}";
        sse_feed(st, plain, (int)strlen(plain), capture, &c);
        check(c.got == NULL, "plain: no SSE deltas (caller falls back to JSON parsing)");
        sse_state_free(st);
        free(c.got);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}