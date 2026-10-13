/* llm.c — minimal OpenAI-compatible chat-completion client built on libcurl.
 * The LLM acts as the SQL parser: it receives the user's request plus the
 * current schema and answers with a JSON query plan that db_exec executes. */
#include "llm.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

typedef struct {
    char *data;   /* cumulative response body */
    int len;
} WriteAccum;

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t n = size * nmemb;
    WriteAccum *acc = (WriteAccum *)userp;
    char *nd = realloc(acc->data, (size_t)acc->len + n + 1);
    if (!nd) return 0; /* signal error to libcurl */
    acc->data = nd;
    memcpy(acc->data + acc->len, contents, n);
    acc->len += (int)n;
    acc->data[acc->len] = '\0';
    return n;
}

static LlmReply reply_ok_char(const char *content)
{
    LlmReply r = { NULL, NULL };
    r.content = strdup(content ? content : "");
    return r;
}

static LlmReply reply_err(const char *err)
{
    LlmReply r = { NULL, NULL };
    r.error = strdup(err ? err : "unknown error");
    return r;
}

/* strip optional ```json ... ``` fences the model may add */
static void strip_fences(char *s)
{
    if (!s) return;
    /* trailing fence */
    while (strlen(s) >= 3 && strncmp(s + strlen(s) - 3, "```", 3) == 0) {
        s[strlen(s) - 3] = '\0';
        while (strlen(s) > 0 && (s[strlen(s) - 1] == '\n' || s[strlen(s) - 1] == '\r'))
            s[strlen(s) - 1] = '\0';
    }
    /* leading fence line, e.g. ```json\n{...} */
    if (strncmp(s, "```", 3) == 0) {
        const char *nl = strchr(s, '\n');
        if (nl) {
            size_t rest = strlen(nl + 1);
            memmove(s, nl + 1, rest);
            s[rest] = '\0';
        }
    }
}

LlmReply llm_chat(const char *base, const char *key, const char *model,
                  const char *system_prompt, const char *user_msg)
{
    /* ---- build the request body ---- */
    JsonValue *sys = json_obj();
    json_obj_set(sys, "role", json_str("system"));
    json_obj_set(sys, "content", json_str(system_prompt));

    JsonValue *usr = json_obj();
    json_obj_set(usr, "role", json_str("user"));
    json_obj_set(usr, "content", json_str(user_msg));

    JsonValue *messages = json_arr();
    json_arr_push(messages, sys);
    json_arr_push(messages, usr);

    JsonValue *body = json_obj();
    json_obj_set(body, "model", json_str(model));
    json_obj_set(body, "messages", messages);
    json_obj_set(body, "temperature", json_num(0));

    char *payload = json_to_string(body);
    json_free(body);

    /* ---- prepare URL ---- */
    const char *b = base;
    size_t blen = strlen(b);
    while (blen > 0 && b[blen - 1] == '/') blen--;
    char url[2048];
    snprintf(url, sizeof url, "%.*s/chat/completions", (int)blen, b);

    /* ---- headers ---- */
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    char auth_hdr[1024];
    if (key && key[0]) {
        snprintf(auth_hdr, sizeof auth_hdr, "Authorization: Bearer %s", key);
        headers = curl_slist_append(headers, auth_hdr);
    }

    WriteAccum acc = { NULL, 0 };
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(payload);
        curl_slist_free_all(headers);
        return reply_err("curl init failed");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &acc);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "isql/0.1");
    /* never route local endpoints through a proxy */
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "127.0.0.1,localhost,::1");

    CURLcode rc = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(payload);

    if (rc != CURLE_OK) {
        if (acc.data) free(acc.data);
        return reply_err(curl_easy_strerror(rc));
    }

    JsonValue *resp = acc.data ? json_parse(acc.data) : NULL;
    if (acc.data) free(acc.data);
    if (!resp) return reply_err("LLM returned invalid JSON");

    if (http_code >= 400) {
        JsonValue *e = json_obj_get(resp, "error");
        JsonValue *m = e ? json_obj_get(e, "message") : NULL;
        char buf[512];
        if (m && m->type == JSON_STR)
            snprintf(buf, sizeof buf, "LLM API error (%ld): %s", http_code, m->str);
        else
            snprintf(buf, sizeof buf, "LLM API error (HTTP %ld)", http_code);
        json_free(resp);
        return reply_err(buf);
    }

    JsonValue *content = NULL;
    JsonValue *choices = json_obj_get(resp, "choices");
    if (choices && choices->type == JSON_ARR && choices->nelems > 0) {
        JsonValue *c0 = choices->elems[0];
        JsonValue *msgv = c0 ? json_obj_get(c0, "message") : NULL;
        JsonValue *cv = msgv ? json_obj_get(msgv, "content") : NULL;
        if (cv && cv->type == JSON_STR) content = cv;
    }
    if (!content) {
        JsonValue *errv = json_obj_get(resp, "error");
        JsonValue *em = errv ? json_obj_get(errv, "message") : NULL;
        if (em && em->type == JSON_STR) {
            char buf[512];
            snprintf(buf, sizeof buf, "LLM error: %s", em->str);
            json_free(resp);
            return reply_err(buf);
        }
        json_free(resp);
        return reply_err("LLM reply had no content");
    }

    char *c = strdup(content->str);
    json_free(resp);
    strip_fences(c);
    return reply_ok_char(c);
}