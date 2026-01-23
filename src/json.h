#ifndef JSON_H
#define JSON_H

#include <stdbool.h>
#include <stddef.h>

// JSON value types
typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

// Forward declaration
typedef struct JsonValue JsonValue;

// JSON object key-value pair
typedef struct {
    char *key;
    JsonValue *value;
} JsonKeyValue;

// JSON value structure
struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        double number;
        char *string;
        struct {
            JsonValue **items;
            int count;
            int capacity;
        } array;
        struct {
            JsonKeyValue *pairs;
            int count;
            int capacity;
        } object;
    } data;
};

// Creation functions
JsonValue *json_null(void);
JsonValue *json_bool(bool value);
JsonValue *json_number(double value);
JsonValue *json_string(const char *value);
JsonValue *json_array(void);
JsonValue *json_object(void);

// Array operations
void json_array_push(JsonValue *arr, JsonValue *value);

// Object operations
void json_object_set(JsonValue *obj, const char *key, JsonValue *value);
JsonValue *json_object_get(JsonValue *obj, const char *key);

// Convenience getters
const char *json_get_string(JsonValue *v);
double json_get_number(JsonValue *v);
bool json_get_bool(JsonValue *v);
int json_array_length(JsonValue *v);
JsonValue *json_array_get(JsonValue *v, int index);

// Parsing
JsonValue *json_parse(const char *str);

// Stringify
char *json_stringify(JsonValue *v);

// Cleanup
void json_free(JsonValue *v);

#endif
