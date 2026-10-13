#ifndef ISQL_JSON_H
#define ISQL_JSON_H

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUM,
    JSON_STR,
    JSON_ARR,
    JSON_OBJ
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct JsonPair {
    char *key;        /* owned by this node */
    JsonValue *val;
} JsonPair;

struct JsonValue {
    JsonType type;
    double num;
    int boolean;
    char *str;          /* owned */
    JsonValue **elems;  /* array elements */
    int nelems;
    JsonPair *pairs;    /* object pairs */
    int npairs;
};

/* --- constructors --- */
JsonValue *json_null(void);
JsonValue *json_bool(int b);
JsonValue *json_num(double n);
JsonValue *json_str(const char *s);
JsonValue *json_arr(void);
JsonValue *json_obj(void);

void json_arr_push(JsonValue *arr, JsonValue *v);
void json_obj_set(JsonValue *obj, const char *key, JsonValue *v);

/* --- access --- */
JsonValue *json_obj_get(const JsonValue *obj, const char *key);
int json_is_num_or_numeric_string(const JsonValue *v, double *out);
JsonValue *json_clone(const JsonValue *v); /* deep copy */
int json_equal(const JsonValue *a, const JsonValue *b); /* deep structural equality */

/* --- parse / stringify --- */
JsonValue *json_parse(const char *text);      /* NULL on error */
JsonValue *json_parse_len(const char *text, int len); /* parse exactly len bytes (may be unterminated) */
char *json_to_string(const JsonValue *v);     /* compact, single line */
void json_free(JsonValue *v);

#endif