#ifndef ISQL_LLM_SSE_H
#define ISQL_LLM_SSE_H

/* Minimal Server-Sent-Events parser for chat completions (stream=true).
 * Feed raw wire bytes in any chunking; deltas are extracted from
 * "data:" lines ("choices[0].delta.content") and delivered through a
 * callback as they arrive. Tolerates non-SSE responses (a proxy/server
 * that ignores "stream"): such input produces no deltas and the caller
 * falls back to parsing the whole body as plain JSON.
 */

typedef void (*SseDelta)(const char *delta, void *arg);

typedef struct {
    char *content;     /* full assembled assistant text */
    int content_len;
    char *line;        /* partial SSE line buffer */
    int line_len;
    char *err;         /* non-NULL when an "error" event arrived */
    int finished;      /* [DONE] seen or error */
} SseState;

SseState *sse_state_new(void);
/* returns 1 once the stream is finished (caller may stop feeding) */
int sse_feed(SseState *st, const char *data, int len, SseDelta cb, void *arg);
void sse_state_free(SseState *st);

#endif