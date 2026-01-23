#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// Helper to create a new JsonValue
static JsonValue *json_alloc(JsonType type) {
    JsonValue *v = calloc(1, sizeof(JsonValue));
    if (v) v->type = type;
    return v;
}

JsonValue *json_null(void) {
    return json_alloc(JSON_NULL);
}

JsonValue *json_bool(bool value) {
    JsonValue *v = json_alloc(JSON_BOOL);
    if (v) v->data.boolean = value;
    return v;
}

JsonValue *json_number(double value) {
    JsonValue *v = json_alloc(JSON_NUMBER);
    if (v) v->data.number = value;
    return v;
}

JsonValue *json_string(const char *value) {
    JsonValue *v = json_alloc(JSON_STRING);
    if (v && value) {
        v->data.string = strdup(value);
    }
    return v;
}

JsonValue *json_array(void) {
    JsonValue *v = json_alloc(JSON_ARRAY);
    if (v) {
        v->data.array.items = NULL;
        v->data.array.count = 0;
        v->data.array.capacity = 0;
    }
    return v;
}

JsonValue *json_object(void) {
    JsonValue *v = json_alloc(JSON_OBJECT);
    if (v) {
        v->data.object.pairs = NULL;
        v->data.object.count = 0;
        v->data.object.capacity = 0;
    }
    return v;
}

void json_array_push(JsonValue *arr, JsonValue *value) {
    if (!arr || arr->type != JSON_ARRAY || !value) return;

    if (arr->data.array.count >= arr->data.array.capacity) {
        int new_cap = arr->data.array.capacity == 0 ? 4 : arr->data.array.capacity * 2;
        JsonValue **new_items = realloc(arr->data.array.items, new_cap * sizeof(JsonValue*));
        if (!new_items) return;
        arr->data.array.items = new_items;
        arr->data.array.capacity = new_cap;
    }

    arr->data.array.items[arr->data.array.count++] = value;
}

void json_object_set(JsonValue *obj, const char *key, JsonValue *value) {
    if (!obj || obj->type != JSON_OBJECT || !key || !value) return;

    // Check if key exists
    for (int i = 0; i < obj->data.object.count; i++) {
        if (strcmp(obj->data.object.pairs[i].key, key) == 0) {
            json_free(obj->data.object.pairs[i].value);
            obj->data.object.pairs[i].value = value;
            return;
        }
    }

    // Add new key
    if (obj->data.object.count >= obj->data.object.capacity) {
        int new_cap = obj->data.object.capacity == 0 ? 4 : obj->data.object.capacity * 2;
        JsonKeyValue *new_pairs = realloc(obj->data.object.pairs, new_cap * sizeof(JsonKeyValue));
        if (!new_pairs) return;
        obj->data.object.pairs = new_pairs;
        obj->data.object.capacity = new_cap;
    }

    obj->data.object.pairs[obj->data.object.count].key = strdup(key);
    obj->data.object.pairs[obj->data.object.count].value = value;
    obj->data.object.count++;
}

JsonValue *json_object_get(JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;

    for (int i = 0; i < obj->data.object.count; i++) {
        if (strcmp(obj->data.object.pairs[i].key, key) == 0) {
            return obj->data.object.pairs[i].value;
        }
    }
    return NULL;
}

const char *json_get_string(JsonValue *v) {
    if (!v || v->type != JSON_STRING) return NULL;
    return v->data.string;
}

double json_get_number(JsonValue *v) {
    if (!v || v->type != JSON_NUMBER) return 0;
    return v->data.number;
}

bool json_get_bool(JsonValue *v) {
    if (!v || v->type != JSON_BOOL) return false;
    return v->data.boolean;
}

int json_array_length(JsonValue *v) {
    if (!v || v->type != JSON_ARRAY) return 0;
    return v->data.array.count;
}

JsonValue *json_array_get(JsonValue *v, int index) {
    if (!v || v->type != JSON_ARRAY) return NULL;
    if (index < 0 || index >= v->data.array.count) return NULL;
    return v->data.array.items[index];
}

void json_free(JsonValue *v) {
    if (!v) return;

    switch (v->type) {
        case JSON_STRING:
            free(v->data.string);
            break;
        case JSON_ARRAY:
            for (int i = 0; i < v->data.array.count; i++) {
                json_free(v->data.array.items[i]);
            }
            free(v->data.array.items);
            break;
        case JSON_OBJECT:
            for (int i = 0; i < v->data.object.count; i++) {
                free(v->data.object.pairs[i].key);
                json_free(v->data.object.pairs[i].value);
            }
            free(v->data.object.pairs);
            break;
        default:
            break;
    }
    free(v);
}

// Parser state
typedef struct {
    const char *str;
    int pos;
} Parser;

static void skip_whitespace(Parser *p) {
    while (p->str[p->pos] && isspace((unsigned char)p->str[p->pos])) {
        p->pos++;
    }
}

static JsonValue *parse_value(Parser *p);

static char *parse_string_content(Parser *p) {
    // Assumes p->str[p->pos] == '"'
    p->pos++; // skip opening quote

    int start = p->pos;
    int len = 0;

    // First pass: count length (handling escapes)
    while (p->str[p->pos] && p->str[p->pos] != '"') {
        if (p->str[p->pos] == '\\' && p->str[p->pos + 1]) {
            p->pos += 2;
            len++;
        } else {
            p->pos++;
            len++;
        }
    }

    // Allocate result
    char *result = malloc(len + 1);
    if (!result) return NULL;

    // Second pass: copy with escape handling
    p->pos = start;
    int out = 0;
    while (p->str[p->pos] && p->str[p->pos] != '"') {
        if (p->str[p->pos] == '\\' && p->str[p->pos + 1]) {
            p->pos++;
            switch (p->str[p->pos]) {
                case 'n': result[out++] = '\n'; break;
                case 'r': result[out++] = '\r'; break;
                case 't': result[out++] = '\t'; break;
                case '\\': result[out++] = '\\'; break;
                case '"': result[out++] = '"'; break;
                case '/': result[out++] = '/'; break;
                case 'u':
                    // Unicode escape - simplified handling
                    result[out++] = '?';
                    if (p->str[p->pos+1] && p->str[p->pos+2] &&
                        p->str[p->pos+3] && p->str[p->pos+4]) {
                        p->pos += 4;
                    }
                    break;
                default: result[out++] = p->str[p->pos]; break;
            }
            p->pos++;
        } else {
            result[out++] = p->str[p->pos++];
        }
    }
    result[out] = '\0';

    if (p->str[p->pos] == '"') p->pos++; // skip closing quote

    return result;
}

static JsonValue *parse_string(Parser *p) {
    char *str = parse_string_content(p);
    if (!str) return NULL;

    JsonValue *v = json_alloc(JSON_STRING);
    if (v) {
        v->data.string = str;
    } else {
        free(str);
    }
    return v;
}

static JsonValue *parse_number(Parser *p) {
    int start = p->pos;

    if (p->str[p->pos] == '-') p->pos++;

    while (isdigit((unsigned char)p->str[p->pos])) p->pos++;

    if (p->str[p->pos] == '.') {
        p->pos++;
        while (isdigit((unsigned char)p->str[p->pos])) p->pos++;
    }

    if (p->str[p->pos] == 'e' || p->str[p->pos] == 'E') {
        p->pos++;
        if (p->str[p->pos] == '+' || p->str[p->pos] == '-') p->pos++;
        while (isdigit((unsigned char)p->str[p->pos])) p->pos++;
    }

    char *numstr = strndup(p->str + start, p->pos - start);
    double val = atof(numstr);
    free(numstr);

    return json_number(val);
}

static JsonValue *parse_array(Parser *p) {
    p->pos++; // skip '['
    JsonValue *arr = json_array();
    if (!arr) return NULL;

    skip_whitespace(p);

    if (p->str[p->pos] == ']') {
        p->pos++;
        return arr;
    }

    while (1) {
        skip_whitespace(p);
        JsonValue *item = parse_value(p);
        if (!item) {
            json_free(arr);
            return NULL;
        }
        json_array_push(arr, item);

        skip_whitespace(p);
        if (p->str[p->pos] == ']') {
            p->pos++;
            break;
        }
        if (p->str[p->pos] == ',') {
            p->pos++;
        } else {
            json_free(arr);
            return NULL;
        }
    }

    return arr;
}

static JsonValue *parse_object(Parser *p) {
    p->pos++; // skip '{'
    JsonValue *obj = json_object();
    if (!obj) return NULL;

    skip_whitespace(p);

    if (p->str[p->pos] == '}') {
        p->pos++;
        return obj;
    }

    while (1) {
        skip_whitespace(p);

        if (p->str[p->pos] != '"') {
            json_free(obj);
            return NULL;
        }

        char *key = parse_string_content(p);
        if (!key) {
            json_free(obj);
            return NULL;
        }

        skip_whitespace(p);
        if (p->str[p->pos] != ':') {
            free(key);
            json_free(obj);
            return NULL;
        }
        p->pos++;

        skip_whitespace(p);
        JsonValue *value = parse_value(p);
        if (!value) {
            free(key);
            json_free(obj);
            return NULL;
        }

        json_object_set(obj, key, value);
        free(key);

        skip_whitespace(p);
        if (p->str[p->pos] == '}') {
            p->pos++;
            break;
        }
        if (p->str[p->pos] == ',') {
            p->pos++;
        } else {
            json_free(obj);
            return NULL;
        }
    }

    return obj;
}

static JsonValue *parse_value(Parser *p) {
    skip_whitespace(p);

    char c = p->str[p->pos];

    if (c == '"') {
        return parse_string(p);
    } else if (c == '{') {
        return parse_object(p);
    } else if (c == '[') {
        return parse_array(p);
    } else if (c == 't' && strncmp(p->str + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return json_bool(true);
    } else if (c == 'f' && strncmp(p->str + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return json_bool(false);
    } else if (c == 'n' && strncmp(p->str + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return json_null();
    } else if (c == '-' || isdigit((unsigned char)c)) {
        return parse_number(p);
    }

    return NULL;
}

JsonValue *json_parse(const char *str) {
    if (!str) return NULL;
    Parser p = { .str = str, .pos = 0 };
    return parse_value(&p);
}

// Stringify helpers
typedef struct {
    char *buf;
    int len;
    int capacity;
} StringBuilder;

static void sb_init(StringBuilder *sb) {
    sb->capacity = 256;
    sb->buf = malloc(sb->capacity);
    sb->len = 0;
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_append(StringBuilder *sb, const char *str) {
    if (!sb->buf || !str) return;
    int slen = strlen(str);

    while (sb->len + slen + 1 > sb->capacity) {
        sb->capacity *= 2;
        char *new_buf = realloc(sb->buf, sb->capacity);
        if (!new_buf) return;
        sb->buf = new_buf;
    }

    memcpy(sb->buf + sb->len, str, slen + 1);
    sb->len += slen;
}

static void sb_append_char(StringBuilder *sb, char c) {
    char s[2] = {c, '\0'};
    sb_append(sb, s);
}

static void stringify_value(StringBuilder *sb, JsonValue *v);

static void stringify_string(StringBuilder *sb, const char *str) {
    sb_append_char(sb, '"');
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '"': sb_append(sb, "\\\""); break;
            case '\\': sb_append(sb, "\\\\"); break;
            case '\n': sb_append(sb, "\\n"); break;
            case '\r': sb_append(sb, "\\r"); break;
            case '\t': sb_append(sb, "\\t"); break;
            default:
                if ((unsigned char)*p < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*p);
                    sb_append(sb, buf);
                } else {
                    sb_append_char(sb, *p);
                }
                break;
        }
    }
    sb_append_char(sb, '"');
}

static void stringify_value(StringBuilder *sb, JsonValue *v) {
    if (!v) {
        sb_append(sb, "null");
        return;
    }

    switch (v->type) {
        case JSON_NULL:
            sb_append(sb, "null");
            break;
        case JSON_BOOL:
            sb_append(sb, v->data.boolean ? "true" : "false");
            break;
        case JSON_NUMBER: {
            char buf[64];
            double d = v->data.number;
            if (d == (int)d) {
                snprintf(buf, sizeof(buf), "%d", (int)d);
            } else {
                snprintf(buf, sizeof(buf), "%g", d);
            }
            sb_append(sb, buf);
            break;
        }
        case JSON_STRING:
            stringify_string(sb, v->data.string ? v->data.string : "");
            break;
        case JSON_ARRAY:
            sb_append_char(sb, '[');
            for (int i = 0; i < v->data.array.count; i++) {
                if (i > 0) sb_append_char(sb, ',');
                stringify_value(sb, v->data.array.items[i]);
            }
            sb_append_char(sb, ']');
            break;
        case JSON_OBJECT:
            sb_append_char(sb, '{');
            for (int i = 0; i < v->data.object.count; i++) {
                if (i > 0) sb_append_char(sb, ',');
                stringify_string(sb, v->data.object.pairs[i].key);
                sb_append_char(sb, ':');
                stringify_value(sb, v->data.object.pairs[i].value);
            }
            sb_append_char(sb, '}');
            break;
    }
}

char *json_stringify(JsonValue *v) {
    StringBuilder sb;
    sb_init(&sb);
    stringify_value(&sb, v);
    return sb.buf;
}
