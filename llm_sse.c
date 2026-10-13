/* llm_sse.c — Server-Sent-Events parser for streamed chat completions. */
#include "llm_sse.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

SseState *sse_state_new(void)
{
    SseState *st = calloc(1, sizeof *st);
    st->content = NULL;
    st->content_len = 0;
    st->line = NULL;
    st->line_len = 0;
    st->err = NULL;
    st->finished = 0;
    return st;
}

void sse_state_free(SseState *st)
{
    if (!st) return;
    free(st->content);
    free(st->line);
    free(st->err);
    free(st);
}

static void sse_append_content(SseState *st, const char *delta, SseDelta cb, void *arg)
{
    if (!delta) return;
    if (cb) cb(delta, arg);
    st->content = realloc(st->content, (size_t)st->content_len + strlen(delta) + 1);
    memcpy(st->content + st->content_len, delta, strlen(delta));
    st->content_len += (int)strlen(delta);
    st->content[st->content_len] = '\0';
}

static void sse_process_line(SseState *st, SseDelta cb, void *arg)
{
    /* skip comments and blank lines */
    if (st->line_len == 0) return;
    /* a data line: "data: <payload>". Some servers pad with spaces. */
    static const char prefix[] = "data:";
    int p = 0;
    while (p < st->line_len && (st->line[p] == ' ' || st->line[p] == '\t')) p++;
    if (p + (int)strlen(prefix) > st->line_len ||
        strncmp(st->line + p, prefix, strlen(prefix)) != 0)
        return; /* event type lines, comments, etc. */
    int s = p + (int)strlen(prefix);
    while (s < st->line_len && st->line[s] == ' ') s++;

    /* trim trailing \r (from \r\n) */
    int e = st->line_len - 1;
    while (e >= s && (st->line[e] == '\r' || st->line[e] == '\n')) e--;

    if (e < s) return;
    int payload_len = e - s + 1;

    /* end-of-stream marker */
    if (payload_len == 6 && strncmp(st->line + s, "[DONE]", 6) == 0) {
        st->finished = 1;
        return;
    }

    JsonValue *obj = json_parse_len(st->line + s, payload_len);
    if (!obj) return; /* not JSON (keep-alive etc.) */
    JsonValue *errv = json_obj_get(obj, "error");
    if (errv) {
        JsonValue *mv = json_obj_get(errv, "message");
        if (mv && mv->type == JSON_STR)
            st->err = strdup(mv->str);
        else
            st->err = strdup("stream error");
        st->finished = 1;
        json_free(obj);
        return;
    }
    JsonValue *choices = json_obj_get(obj, "choices");
    if (choices && choices->type == JSON_ARR && choices->nelems > 0) {
        JsonValue *c0 = choices->elems[0];
        JsonValue *delta = c0 ? json_obj_get(c0, "delta") : NULL;
        JsonValue *content = delta ? json_obj_get(delta, "content") : NULL;
        if (content && content->type == JSON_STR)
            sse_append_content(st, content->str, cb, arg);
    }
    json_free(obj);
}

int sse_feed(SseState *st, const char *data, int len, SseDelta cb, void *arg)
{
    if (!st) return 1;
    for (int i = 0; i < len && !st->finished; i++) {
        if (data[i] == '\n') {
            sse_process_line(st, cb, arg);
            st->line_len = 0;
            continue;
        }
        st->line = realloc(st->line, (size_t)st->line_len + 2);
        st->line[st->line_len++] = data[i];
    }
    return st->finished;
}