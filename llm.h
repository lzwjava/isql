#ifndef ISQL_LLM_H
#define ISQL_LLM_H

typedef struct {
    char *content; /* assistant reply (JSON plan), or NULL */
    char *error;   /* human readable error, or NULL */
} LlmReply;

/*
 * Calls an OpenAI-compatible /chat/completions endpoint.
 *
 *   base  - e.g. "https://api.openai.com/v1" or "http://localhost:11434/v1" (Ollama)
 *   key   - Bearer token; may be NULL/empty (Ollama does not need one)
 *   model - model identifier
 *
 * Returns a reply struct; both fields are NULL-able. Caller frees strings.
 */
LlmReply llm_chat(const char *base, const char *key, const char *model,
                  const char *system_prompt, const char *user_msg);

/* streamed variant: with `stream` requests enabled, each content delta is
 * delivered to on_delta(content, arg) as it arrives. Pass on_delta=NULL for
 * a regular non-streamed completion (equivalent to llm_chat). */
LlmReply llm_chat_stream(const char *base, const char *key, const char *model,
                         const char *system_prompt, const char *user_msg,
                         void (*on_delta)(const char *delta, void *arg), void *arg);

#endif