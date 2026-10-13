/* json.c — a small self-contained JSON parser/serializer. */
#include "json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* constructors                                                      */
/* ---------------------------------------------------------------- */

static JsonValue *json_new(JsonType t)
{
    JsonValue *v = calloc(1, sizeof *v);
    v->type = t;
    return v;
}

JsonValue *json_null(void)              { return json_new(JSON_NULL); }
JsonValue *json_bool(int b)             { JsonValue *v = json_new(JSON_BOOL); v->boolean = b; return v; }
JsonValue *json_num(double n)           { JsonValue *v = json_new(JSON_NUM); v->num = n; return v; }
JsonValue *json_str(const char *s)      { JsonValue *v = json_new(JSON_STR); v->str = strdup(s ? s : ""); return v; }
JsonValue *json_arr(void)               { return json_new(JSON_ARR); }
JsonValue *json_obj(void)               { return json_new(JSON_OBJ); }

void json_arr_push(JsonValue *arr, JsonValue *v)
{
    JsonValue **n = realloc(arr->elems, sizeof(JsonValue *) * (arr->nelems + 1));
    n[arr->nelems++] = v;
    arr->elems = n;
}

/* Takes ownership: the key is strdup'ed and later freed with the object.
 * If the key already exists the old value is replaced (and freed). */
void json_obj_set(JsonValue *obj, const char *key, JsonValue *v)
{
    if (obj && obj->type == JSON_OBJ) {
        for (int i = 0; i < obj->npairs; i++) {
            if (strcmp(obj->pairs[i].key, key) == 0) {
                json_free(obj->pairs[i].val);
                obj->pairs[i].val = v;
                return;
            }
        }
    }
    JsonPair *n = realloc(obj->pairs, sizeof(JsonPair) * (obj->npairs + 1));
    n[obj->npairs].key = strdup(key ? key : "");
    n[obj->npairs].val = v;
    obj->pairs = n;
    obj->npairs++;
}

JsonValue *json_obj_get(const JsonValue *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJ) return NULL;
    for (int i = 0; i < obj->npairs; i++)
        if (strcmp(obj->pairs[i].key, key) == 0)
            return obj->pairs[i].val;
    return NULL;
}

int json_is_num_or_numeric_string(const JsonValue *v, double *out)
{
    if (!v) return 0;
    if (v->type == JSON_NUM) { *out = v->num; return 1; }
    if (v->type == JSON_STR) {
        const char *s = v->str;
        char *end = NULL;
        double d = strtod(s, &end);
        if (end && end != s) {
            /* require the whole string to be numeric (allow surrounding spaces) */
            while (*end) {
                if (!isspace((unsigned char)*end)) return 0;
                end++;
            }
            *out = d;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* parser                                                            */
/* ---------------------------------------------------------------- */

typedef struct {
    const char *s;
    int i;
    int len;
    int depth;
} Parser;

static void skip_ws(Parser *p)
{
    while (p->i < p->len && isspace((unsigned char)p->s[p->i])) p->i++;
}

static void json_error(const char *msg)
{
    (void)msg; /* errors reported as NULL from json_parse */
}

static JsonValue *parse_value(Parser *p);
static JsonValue *parse_object(Parser *p);
static JsonValue *parse_array(Parser *p);
static JsonValue *parse_string(Parser *p);
static JsonValue *parse_number(Parser *p);
static JsonValue *parse_literal(Parser *p);

static JsonValue *parse_literal(Parser *p)
{
    const char *tests[] = { "true", "false", "null" };
    for (int t = 0; t < 3; t++) {
        int l = (int)strlen(tests[t]);
        if (p->i + l <= p->len && strncmp(p->s + p->i, tests[t], l) == 0) {
            p->i += l;
            switch (t) {
            case 0: return json_bool(1);
            case 1: return json_bool(0);
            default: return json_null();
            }
        }
    }
    json_error("bad literal");
    return NULL;
}

/* append codepoint as UTF-8 into a growing buffer */
static void append_utf8(char **buf, int *len, int *cap, unsigned cp)
{
    int n;
    unsigned char out[4];
    if (cp < 0x80) { out[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); n = 2; }
    else if (cp < 0x10000) { out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F); n = 3; }
    else { out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F); out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F); n = 4; }
    if (*len + n > *cap) {
        *cap = (*cap + 16) * 2;
        *buf = realloc(*buf, (size_t)*cap);
    }
    memcpy(*buf + *len, out, (size_t)n);
    *len += n;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static JsonValue *parse_string(Parser *p)
{
    if (p->s[p->i] != '"') { json_error("expected string"); return NULL; }
    p->i++;
    char *buf = NULL;
    int blen = 0, bcap = 0;
    while (p->i < p->len) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') { p->i++; break; }
        if (c == '\\') {
            p->i++;
            if (p->i >= p->len) break;
            char e = p->s[p->i];
            if (e == '"' || e == '\\' || e == '/') append_utf8(&buf, &blen, &bcap, (unsigned char)e);
            else if (e == 'b') append_utf8(&buf, &blen, &bcap, 0x08);
            else if (e == 'f') append_utf8(&buf, &blen, &bcap, 0x0C);
            else if (e == 'n') append_utf8(&buf, &blen, &bcap, 0x0A);
            else if (e == 'r') append_utf8(&buf, &blen, &bcap, 0x0D);
            else if (e == 't') append_utf8(&buf, &blen, &bcap, 0x09);
            else if (e == 'u') {
                if (p->i + 4 >= p->len) break;
                int hi = 0;
                for (int k = 1; k <= 4; k++) {
                    int h = hexval(p->s[p->i + k]);
                    if (h < 0) { hi = -1; break; }
                    hi = (hi << 4) | h;
                }
                p->i += 4;
                if (hi < 0) break;
                /* handle surrogate pairs */
                if (hi >= 0xD800 && hi <= 0xDBFF && p->i + 6 < p->len) {
                    if (p->s[p->i + 1] == '\\' && p->s[p->i + 2] == 'u') {
                        int lo = 0; int ok = 1;
                        for (int k = 3; k <= 6; k++) {
                            int h = hexval(p->s[p->i + k]);
                            if (h < 0) { ok = 0; break; }
                            lo = (lo << 4) | h;
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            hi = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
                            p->i += 6;
                        }
                    }
                }
                append_utf8(&buf, &blen, &bcap, (unsigned)hi);
            }
            p->i++;
            continue;
        }
        if (c < 0x20) { json_error("control char in string"); break; }
        append_utf8(&buf, &blen, &bcap, c);
        p->i++;
    }
    if (!buf) buf = strdup("");
    JsonValue *v = json_new(JSON_STR);
    v->str = buf;
    return v;
}

static JsonValue *parse_number(Parser *p)
{
    int start = p->i;
    if (p->i < p->len && p->s[p->i] == '-') p->i++;
    while (p->i < p->len && isdigit((unsigned char)p->s[p->i])) p->i++;
    if (p->i < p->len && p->s[p->i] == '.') {
        p->i++;
        while (p->i < p->len && isdigit((unsigned char)p->s[p->i])) p->i++;
    }
    if (p->i < p->len && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->len && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        while (p->i < p->len && isdigit((unsigned char)p->s[p->i])) p->i++;
    }
    if (p->i == start) { json_error("bad number"); return NULL; }
    double d = strtod(p->s + start, NULL);
    return json_num(d);
}

static JsonValue *parse_array(Parser *p)
{
    p->i++; /* [ */
    JsonValue *arr = json_arr();
    for (;;) {
        skip_ws(p);
        if (p->i >= p->len) return NULL;
        if (p->s[p->i] == ']') { p->i++; break; }
        JsonValue *v = parse_value(p);
        if (!v) return NULL;
        json_arr_push(arr, v);
        skip_ws(p);
        if (p->i < p->len && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->len && p->s[p->i] == ']') { p->i++; break; }
        json_error("expected , or ]");
        return NULL;
    }
    return arr;
}

static JsonValue *parse_object(Parser *p)
{
    p->i++; /* { */
    JsonValue *obj = json_obj();
    for (;;) {
        skip_ws(p);
        if (p->i >= p->len) return NULL;
        if (p->s[p->i] == '}') { p->i++; break; }
        JsonValue *k = parse_string(p);
        if (!k || k->type != JSON_STR) return NULL;
        skip_ws(p);
        if (p->i >= p->len || p->s[p->i] != ':') return NULL;
        p->i++;
        JsonValue *v = parse_value(p);
        if (!v) return NULL;
        JsonPair *n = realloc(obj->pairs, sizeof(JsonPair) * (obj->npairs + 1));
        n[obj->npairs].key = k->str;   /* transfer ownership of key string */
        n[obj->npairs].val = v;
        obj->pairs = n;
        obj->npairs++;
        k->str = NULL;                 /* so json_free(k) won't free it twice */
        json_free(k);
        skip_ws(p);
        if (p->i < p->len && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->len && p->s[p->i] == '}') { p->i++; break; }
        json_error("expected , or }");
        return NULL;
    }
    return obj;
}

static int max_depth_seen = 0;

static JsonValue *parse_value(Parser *p)
{
    skip_ws(p);
    if (p->i >= p->len) { json_error("empty input"); return NULL; }
    char c = p->s[p->i];
    if (p->depth >= 400) { json_error("nesting too deep"); return NULL; }
    p->depth++;
    if (p->depth > max_depth_seen) max_depth_seen = p->depth;
    JsonValue *v = NULL;
    if (c == '{') v = parse_object(p);
    else if (c == '[') v = parse_array(p);
    else if (c == '"') v = parse_string(p);
    else if (c == '-' || isdigit((unsigned char)c)) v = parse_number(p);
    else if (c == 't' || c == 'f' || c == 'n') v = parse_literal(p);
    else json_error("unexpected char");
    p->depth--;
    return v;
}

JsonValue *json_parse(const char *text)
{
    if (!text) return NULL;
    Parser p = { text, 0, (int)strlen(text), 0 };
    JsonValue *v = parse_value(&p);
    if (!v) return NULL;
    skip_ws(&p);
    if (p.i != p.len) { json_error("trailing data"); return NULL; }
    return v;
}

JsonValue *json_parse_len(const char *text, int len)
{
    if (!text) return NULL;
    if (len < 0) return NULL;
    /* normalize to a NUL-terminated slice (no copying if safe is impossible) */
    char *tmp = malloc((size_t)len + 1);
    if (!tmp) return NULL;
    memcpy(tmp, text, (size_t)len);
    tmp[len] = '\0';
    JsonValue *v = json_parse(tmp);
    free(tmp);
    return v;
}

/* ---------------------------------------------------------------- */
/* stringify                                                         */
/* ---------------------------------------------------------------- */

typedef struct {
    char *buf;      /* NULL until first allocation */
    int buf_len;    /* used bytes */
    int cap;        /* allocated capacity */
} WB;

static void emit(WB *w, const char *s)
{
    int extra = (int)strlen(s);
    if (w->buf == NULL) {
        w->cap = extra + 64;
        w->buf = calloc(1, (size_t)w->cap);
    } else if (w->buf_len + extra + 16 > w->cap) {
        while (w->buf_len + extra + 16 > w->cap) w->cap *= 2;
        w->buf = realloc(w->buf, (size_t)w->cap);
    }
    memcpy(w->buf + w->buf_len, s, (size_t)extra);
    w->buf_len += extra;
    w->buf[w->buf_len] = '\0'; /* always keep the buffer terminated */
}

static void emit_str(WB *w, const char *s)
{
    emit(w, "\"");
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  emit(w, "\\\""); break;
        case '\\': emit(w, "\\\\"); break;
        case '\n': emit(w, "\\n"); break;
        case '\r': emit(w, "\\r"); break;
        case '\t': emit(w, "\\t"); break;
        case '\b': emit(w, "\\b"); break;
        case '\f': emit(w, "\\f"); break;
        default:
            if (c < 0x20) emit(w, "\\u0000");
            else {
                char tmp[2] = { (char)c, 0 };
                emit(w, tmp);
            }
        }
    }
    emit(w, "\"");
}

static void emit_num(WB *w, double d)
{
    char tmp[64];
    if (d == (double)(long long)d && fabs(d) < 9.0e18) {
        snprintf(tmp, sizeof tmp, "%lld", (long long)d);
    } else {
        snprintf(tmp, sizeof tmp, "%.17g", d);
    }
    emit(w, tmp);
}

static void render(const JsonValue *v, WB *w);

static void render(const JsonValue *v, WB *w)
{
    if (!v) { emit(w, "null"); return; }
    switch (v->type) {
    case JSON_NULL: emit(w, "null"); break;
    case JSON_BOOL: emit(w, v->boolean ? "true" : "false"); break;
    case JSON_NUM:  emit_num(w, v->num); break;
    case JSON_STR:  emit_str(w, v->str); break;
    case JSON_ARR:
        emit(w, "[");
        for (int i = 0; i < v->nelems; i++) {
            if (i) emit(w, ",");
            render(v->elems[i], w);
        }
        emit(w, "]");
        break;
    case JSON_OBJ:
        emit(w, "{");
        for (int i = 0; i < v->npairs; i++) {
            if (i) emit(w, ",");
            emit_str(w, v->pairs[i].key);
            emit(w, ":");
            render(v->pairs[i].val, w);
        }
        emit(w, "}");
        break;
    }
}

char *json_to_string(const JsonValue *v)
{
    WB w = { NULL, 0, 0 };
    render(v, &w);
    if (!w.buf) w.buf = strdup("");
    return w.buf; /* caller frees */
}

JsonValue *json_clone(const JsonValue *v)
{
    if (!v) return json_null();
    JsonValue *out = json_new(v->type);
    switch (v->type) {
    case JSON_NULL: break;
    case JSON_BOOL: out->boolean = v->boolean; break;
    case JSON_NUM:  out->num = v->num; break;
    case JSON_STR:  out->str = strdup(v->str); break;
    case JSON_ARR:
        for (int i = 0; i < v->nelems; i++)
            json_arr_push(out, json_clone(v->elems[i]));
        break;
    case JSON_OBJ:
        for (int i = 0; i < v->npairs; i++)
            json_obj_set(out, v->pairs[i].key, json_clone(v->pairs[i].val));
        break;
    }
    return out;
}

int json_equal(const JsonValue *a, const JsonValue *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->type != b->type) return 0;
    switch (a->type) {
    case JSON_NULL: return 1;
    case JSON_BOOL: return a->boolean == b->boolean;
    case JSON_NUM:  return a->num == b->num;
    case JSON_STR:  return strcmp(a->str, b->str) == 0;
    case JSON_ARR:
        if (a->nelems != b->nelems) return 0;
        for (int i = 0; i < a->nelems; i++)
            if (!json_equal(a->elems[i], b->elems[i])) return 0;
        return 1;
    case JSON_OBJ:
        if (a->npairs != b->npairs) return 0;
        for (int i = 0; i < a->npairs; i++) {
            JsonValue *bv = json_obj_get(b, a->pairs[i].key);
            if (!bv || !json_equal(a->pairs[i].val, bv)) return 0;
        }
        return 1;
    }
    return 0;
}

void json_free(JsonValue *v)
{
    if (!v) return;
    if (v->type == JSON_STR) free(v->str);
    for (int i = 0; i < v->nelems; i++) json_free(v->elems[i]);
    free(v->elems);
    for (int i = 0; i < v->npairs; i++) {
        free(v->pairs[i].key);
        json_free(v->pairs[i].val);
    }
    free(v->pairs);
    free(v);
}