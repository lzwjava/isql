/* fuzz.c — deterministic fuzz harness for the JSON layer and the engine.
 *
 * Three targets, all driven by a seedable PRNG:
 *   1. json_parse over adversarial byte streams (deep nesting, truncations,
 *      escapes, UTF-8-ish garbage) — must never crash;
 *   2. db_exec over randomly generated plans (valid and nonsense op names,
 *      random wheres including deep and/or nesting) — must never crash;
 *   3. serializer round-trips (build random value -> string -> parse ->
 *      deep-equal) — must always be true.
 *
 * Run: ./fuzz [iterations] [seed]
 */
#include "../db.h"
#include "../json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_state;
static unsigned long long rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static int rnd_range(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }

static Database db;

/* ---------------- target 1: raw JSON parsing ---------------- */
static char *make_adversarial_text(int which)
{
    char *buf = malloc(65536);
    int n;
    switch (which % 6) {
    case 0: /* deep array nesting: exercises the depth guard */
        n = rnd_range(1, 200000);
        {
            int i = 0;
            for (; i < n && i < 200000; i++) {
                buf[i] = '[';
            }
            buf[i] = 0;
        }
        break;
    case 1: /* deep object nesting */
        {
            int i = 0, d = rnd_range(1, 5000);
            for (; i < d; i++) { buf[i] = '{'; buf[++i] = '"'; buf[++i] = 'a'; buf[++i] = '"'; buf[++i] = ':'; }
            buf[i] = 0;
        }
        break;
    case 2: /* valid-ish document with random junk */
        {
            int i = 0;
            buf[i++] = '{';
            buf[i++] = '"';
            buf[i++] = (char)('a' + rnd_range(0, 25));
            buf[i++] = '"';
            buf[i++] = ':';
            buf[i++] = '1';
            buf[i++] = '}';
            buf[i++] = ' ';
            n = rnd_range(8, 2048);
            for (; i < n; i++) buf[i] = (char)rnd_range(0, 255);
            buf[i] = 0;
        }
        break;
    case 3: /* escapes + unicode escapes */
        {
            int i = 0;
            strcpy(buf + i, "{\"x\":\"\\u1234\\uD83D\\uDE00\\\\\\\"\\n\"}");
            i = (int)strlen(buf);
            if (rnd() % 2) { strcpy(buf + i, "\\u"); i += 2; }
            buf[i] = 0;
        }
        break;
    case 4: /* truncated valid JSON (crash-mid-append simulation) */
        {
            const char *tpl = "{\"tables\":[{\"name\":\"users\",\"columns\":[{\"name\":\"id\"}],\"rows\":[{\"id\":1}]}]}";
            n = rnd_range(0, (int)strlen(tpl));
            memcpy(buf, tpl, (size_t)n);
            buf[n] = 0;
        }
        break;
    default: /* random numbers/strings/boron */
        {
            int i = 0;
            n = rnd_range(1, 4096);
            for (; i < n; i++) {
                char c;
                switch (rnd_range(0, 7)) {
                case 0: c = '"'; break;
                case 1: c = '\\'; break;
                case 2: c = (char)(rnd_range(0, 9) + '0'); break;
                case 3: c = (char)(rnd_range(0, 25) + 'a'); break;
                case 4: c = ','; break;
                case 5: c = ':'; break;
                case 6: c = '['; break;
                default: c = ']';
                }
                buf[i] = c;
            }
            buf[i] = 0;
        }
    }
    return buf;
}

/* ---------------- target 2: random engine plans ---------------- */

static void gen_value(char *buf, int depth);
static void gen_where(char *buf, int depth)
{
    if (depth <= 0 || (rnd() % 3) == 0) {
        /* a single column condition */
        static const char *cols[] = { "id", "name", "age", "city", "bogus", "" };
        static const char *ops[] = { "\"gt\":", "\"gte\":", "\"lt\":", "\"lte\":", "\"ne\":",
                                     "\"in\":", "\"like\":", "\"between\":" };
        const char *c = cols[rnd_range(0, 4)];
        int op = (int)(rnd() % 8);
        *buf++ = '"';
        while (*c) *buf++ = *c++;
        *buf++ = '"';
        *buf++ = ':';
        *buf++ = '{';
        strcpy(buf, ops[op]);
        buf += strlen(ops[op]);
        if (op == 5) { /* in -> array */
            *buf++ = '[';
            int k = rnd_range(0, 3);
            for (int i = 0; i < k; i++) {
                if (i) *buf++ = ',';
                gen_value(buf, depth + 1);
                buf = buf + strlen(buf);
            }
            *buf++ = ']';
        } else if (op == 6) { /* like -> string pattern */
            strcpy(buf, (rnd() % 2) ? "\"a%\"" : "\"_o\"");
            buf += (rnd() % 2) ? 4 : 5;
        } else if (op == 7) { /* between -> [lo,hi] */
            strcpy(buf, "[");
            buf++;
            gen_value(buf, depth + 1);
            buf = buf + strlen(buf);
            *buf++ = ',';
            gen_value(buf, depth + 1);
            buf = buf + strlen(buf);
            *buf++ = ']';
        } else {
            gen_value(buf, depth + 1);
            buf = buf + strlen(buf);
        }
        *buf++ = '}';
        *buf = 0;
        return;
    }
    /* boolean nesting */
    if (rnd() % 2) strcpy(buf, "\"and\":[");
    else strcpy(buf, "\"or\":[");
    buf += strlen(buf);
    for (int i = 0; i < 3; i++) {
        if (i) *buf++ = ',';
        *buf++ = '{';
        gen_where(buf, depth - 1);
        buf = buf + strlen(buf);
        *buf++ = '}';
    }
    *buf++ = ']';
    *buf = 0;
}

static void gen_value(char *buf, int depth)
{
    switch (rnd_range(0, 7)) {
    case 0: strcpy(buf, "5"); break;
    case 1: strcpy(buf, "3.14"); break;
    case 2: strcpy(buf, "-2.5e3"); break;
    case 3: strcpy(buf, "\"text\""); break;
    case 4: strcpy(buf, "\"\""); break;
    case 5: strcpy(buf, "null"); break;
    case 6: strcpy(buf, "true"); break;
    default:
        if (depth > 4) { strcpy(buf, "1"); break; }
        *buf++ = '[';
        for (int i = 0; i < 2; i++) {
            if (i) *buf++ = ',';
            gen_value(buf, depth + 1);
            buf = buf + strlen(buf);
        }
        *buf++ = ']';
        *buf = 0;
    }
}

static void fuzz_plan_into(char *buf, int iter);

static void fuzz_plan_into(char *buf, int iter)
{
    static const char *ops[] = { "select", "update", "delete", "aggregate", "insert",
                                 "alter_table", "describe", "show_tables", "drop_table",
                                 "create_table", "error", "bogus_op", "" };
    char *p = buf;
    if (iter > 0 && (rnd() % 12 == 0)) {
        /* a batch of random sub-plans (all-or-nothing) */
        strcpy(p, "{\"ops\":[");
        p += strlen(p);
        for (int i = 0; i < rnd_range(1, 3); i++) {
            if (i) *p++ = ',';
            char sub[16384];
            fuzz_plan_into(sub, iter - 1);
            strcpy(p, sub);
            p += strlen(p);
        }
        *p++ = ']';
        *p++ = '}';
        *p = 0;
        return;
    }
    const char *op = ops[rnd_range(0, 12)];
    strcpy(p, "{\"op\":\"");
    strcpy(p + strlen(p), op);
    static const char *tables[] = { "users", "orders", "ghosts", "" };
    strcpy(p + strlen(p), "\",\"table\":\"");
    strcpy(p + strlen(p), tables[rnd_range(0, 2)]);
    strcpy(p + strlen(p), "\"");
    if (rnd() % 3 == 0) {
        static const char *cols[] = { "name", "bogus", "id", "age", "num" };
        strcpy(p + strlen(p), ",\"columns\":[\"");
        strcpy(p + strlen(p), cols[rnd_range(0, 4)]);
        strcpy(p + strlen(p), "\"]");
    }
    if (rnd() % 3 == 0) {
        strcpy(p + strlen(p), ",\"where\":{");
        char w[8192];
        gen_where(w, rnd_range(1, 6));
        strcpy(p + strlen(p), w);
        strcpy(p + strlen(p), "}");
    }
    if (rnd() % 3 == 0)
        strcpy(p + strlen(p), ",\"limit\":99999999999999999999");
    if (iter % 7 == 0)
        strcpy(p + strlen(p), ",\"values\":{\"id\":1,\"name\":\"x\"}");
    if (rnd() % 4 == 0)
        strcpy(p + strlen(p), ",\"group_by\":[\"city\"],\"aggs\":[{\"as\":\"n\",\"func\":\"count\",\"column\":\"*\"}]");
    *p++ = '}';
    *p = 0;
}

static void fuzz_plan(int iter)
{
    char buf[16384];
    fuzz_plan_into(buf, iter);
    JsonValue *plan = json_parse(buf);
    if (plan) {
        JsonValue *r = db_exec(&db, plan);
        if (r) json_free(r);
        json_free(plan);
    }
}

/* ---------------- target 3: round-trips ---------------- */
static JsonValue *random_value(int depth)
{
    switch (rnd_range(0, 6)) {
    case 0: return json_null();
    case 1: return json_bool(rnd() % 2);
    case 2: return json_num((double)(long long)(rnd() % 1000000) / 7.0);
    case 3: {
        char s[64];
        snprintf(s, sizeof s, "s%llu", rnd());
        return json_str(s);
    }
    default:
        if (depth > 8) return json_num(1);
        if (rnd() % 2) {
            JsonValue *a = json_arr();
            int n = rnd_range(0, 5);
            for (int i = 0; i < n; i++) json_arr_push(a, random_value(depth + 1));
            return a;
        }
        JsonValue *o = json_obj();
        int n = rnd_range(0, 5);
        for (int i = 0; i < n; i++) {
            char k[32];
            snprintf(k, sizeof k, "k%d", i);
            json_obj_set(o, k, random_value(depth + 1));
        }
        return o;
    }
}

int main(int argc, char **argv)
{
    int N = argc > 1 ? atoi(argv[1]) : 100000;
    rng_state = argv[2] ? strtoull(argv[2], NULL, 10) : 0x9e3779b97f4a7c15ULL;

    db_init(&db, NULL);
    db_exec(&db, json_parse("{\"op\":\"create_table\",\"table\":\"users\",\"columns\":["
                            "{\"name\":\"id\",\"type\":\"int\",\"primary\":true},"
                            "{\"name\":\"name\",\"type\":\"text\"},"
                            "{\"name\":\"age\",\"type\":\"int\"},"
                            "{\"name\":\"city\",\"type\":\"text\"},"
                            "{\"name\":\"num\",\"type\":\"real\"}]}"));

    int json_ok = 0, plan_ok = 0, roundtrip = 0, max_json_depth = 0;
    for (int iter = 0; iter < N; iter++) {
        /* target 1 */
        char *txt = make_adversarial_text(iter);
        JsonValue *v = json_parse(txt);
        if (v) { json_ok++; json_free(v); }
        int len = rnd_range(0, (int)strlen(txt));
        v = json_parse_len(txt, len);
        if (v) { json_ok++; json_free(v); }
        free(txt);

        /* target 2 */
        fuzz_plan(iter);
        plan_ok++;

        /* target 3 */
        JsonValue *src = random_value(0);
        char *s = json_to_string(src);
        JsonValue *back = json_parse(s);
        if (!back || !json_equal(src, back)) {
            fprintf(stderr, "fuzz: ROUNDTRIP FAILURE at iter %d:\n", iter);
            fprintf(stderr, "  fwd: %s\n", s);
            fprintf(stderr, "  rev: %s\n", back ? json_to_string(back) : "(null)");
            FILE *wf = fopen("/tmp/fwd.txt", "w");
            if (wf) { fputs(s, wf); fclose(wf); }
            /* dump the tree shape to understand the mismatch */
            FILE *tf = fopen("/tmp/tree.txt", "w");
            if (tf) {
                char buf[65536];
                JsonValue *x = src;
                int type = x ? x->type : -1;
                snprintf(buf, sizeof buf, "top type=%d nelems=%d npairs=%d\n",
                         type, x->nelems, x->npairs);
                fputs(buf, tf);
                fclose(tf);
            }
            free(s);
            return 1;
        }
        roundtrip++;
        json_free(back);
        free(s);
        json_free(src);
    }
    (void)max_json_depth;
    printf("fuzz ok: %d iterations, %d JSON parses, %d plans, %d roundtrips, seed=%llu\n",
           N, json_ok, plan_ok, roundtrip, rng_state);
    printf("  no crashes and no roundtrip violations.\n");
    return 0;
}