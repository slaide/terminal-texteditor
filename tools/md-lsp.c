/*
 * md-lsp: A simple Markdown Language Server
 *
 * Provides:
 * - Diagnostics: broken heading links, duplicate headings
 * - Semantic tokens: headings, bold, italic, code, links
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <stdarg.h>

// ============== JSON Parser (minimal, inline) ==============

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;
typedef struct { char *key; JsonValue *value; } JsonKV;

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        double number;
        char *string;
        struct { JsonValue **items; int count, cap; } array;
        struct { JsonKV *pairs; int count, cap; } object;
    } d;
};

static JsonValue *json_new(JsonType t) {
    JsonValue *v = calloc(1, sizeof(JsonValue));
    v->type = t;
    return v;
}

static void json_free(JsonValue *v) {
    if (!v) return;
    if (v->type == JSON_STRING) free(v->d.string);
    else if (v->type == JSON_ARRAY) {
        for (int i = 0; i < v->d.array.count; i++) json_free(v->d.array.items[i]);
        free(v->d.array.items);
    } else if (v->type == JSON_OBJECT) {
        for (int i = 0; i < v->d.object.count; i++) {
            free(v->d.object.pairs[i].key);
            json_free(v->d.object.pairs[i].value);
        }
        free(v->d.object.pairs);
    }
    free(v);
}

static JsonValue *json_string(const char *s) {
    JsonValue *v = json_new(JSON_STRING);
    v->d.string = strdup(s);
    return v;
}

static JsonValue *json_number(double n) {
    JsonValue *v = json_new(JSON_NUMBER);
    v->d.number = n;
    return v;
}

static JsonValue *json_bool(bool b) {
    JsonValue *v = json_new(JSON_BOOL);
    v->d.boolean = b;
    return v;
}

static JsonValue *json_array(void) { return json_new(JSON_ARRAY); }
static JsonValue *json_object(void) { return json_new(JSON_OBJECT); }

static void json_array_push(JsonValue *arr, JsonValue *val) {
    if (arr->d.array.count >= arr->d.array.cap) {
        arr->d.array.cap = arr->d.array.cap ? arr->d.array.cap * 2 : 8;
        arr->d.array.items = realloc(arr->d.array.items, arr->d.array.cap * sizeof(JsonValue*));
    }
    arr->d.array.items[arr->d.array.count++] = val;
}

static void json_object_set(JsonValue *obj, const char *key, JsonValue *val) {
    // Check if key exists
    for (int i = 0; i < obj->d.object.count; i++) {
        if (strcmp(obj->d.object.pairs[i].key, key) == 0) {
            json_free(obj->d.object.pairs[i].value);
            obj->d.object.pairs[i].value = val;
            return;
        }
    }
    if (obj->d.object.count >= obj->d.object.cap) {
        obj->d.object.cap = obj->d.object.cap ? obj->d.object.cap * 2 : 8;
        obj->d.object.pairs = realloc(obj->d.object.pairs, obj->d.object.cap * sizeof(JsonKV));
    }
    obj->d.object.pairs[obj->d.object.count].key = strdup(key);
    obj->d.object.pairs[obj->d.object.count].value = val;
    obj->d.object.count++;
}

static JsonValue *json_object_get(JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->d.object.count; i++) {
        if (strcmp(obj->d.object.pairs[i].key, key) == 0)
            return obj->d.object.pairs[i].value;
    }
    return NULL;
}

static const char *json_get_string(JsonValue *v) {
    return (v && v->type == JSON_STRING) ? v->d.string : NULL;
}

static double json_get_number(JsonValue *v) {
    return (v && v->type == JSON_NUMBER) ? v->d.number : 0;
}

// Simple JSON parser
static const char *json_skip_ws(const char *s) {
    while (*s && isspace(*s)) s++;
    return s;
}

static JsonValue *json_parse_value(const char **s);

static char *json_parse_string_val(const char **s) {
    if (**s != '"') return NULL;
    (*s)++;
    const char *start = *s;
    while (**s && **s != '"') {
        if (**s == '\\' && *(*s + 1)) (*s)++;
        (*s)++;
    }
    int len = *s - start;
    char *result = malloc(len + 1);
    // Simple unescape
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (start[i] == '\\' && i + 1 < len) {
            i++;
            switch (start[i]) {
                case 'n': result[j++] = '\n'; break;
                case 't': result[j++] = '\t'; break;
                case 'r': result[j++] = '\r'; break;
                case '\\': result[j++] = '\\'; break;
                case '"': result[j++] = '"'; break;
                default: result[j++] = start[i]; break;
            }
        } else {
            result[j++] = start[i];
        }
    }
    result[j] = '\0';
    if (**s == '"') (*s)++;
    return result;
}

static JsonValue *json_parse_value(const char **s) {
    *s = json_skip_ws(*s);
    if (**s == '"') {
        char *str = json_parse_string_val(s);
        JsonValue *v = json_new(JSON_STRING);
        v->d.string = str;
        return v;
    } else if (**s == '{') {
        (*s)++;
        JsonValue *obj = json_object();
        *s = json_skip_ws(*s);
        while (**s && **s != '}') {
            *s = json_skip_ws(*s);
            char *key = json_parse_string_val(s);
            *s = json_skip_ws(*s);
            if (**s == ':') (*s)++;
            JsonValue *val = json_parse_value(s);
            if (key && val) json_object_set(obj, key, val);
            free(key);
            *s = json_skip_ws(*s);
            if (**s == ',') (*s)++;
        }
        if (**s == '}') (*s)++;
        return obj;
    } else if (**s == '[') {
        (*s)++;
        JsonValue *arr = json_array();
        *s = json_skip_ws(*s);
        while (**s && **s != ']') {
            JsonValue *val = json_parse_value(s);
            if (val) json_array_push(arr, val);
            *s = json_skip_ws(*s);
            if (**s == ',') (*s)++;
        }
        if (**s == ']') (*s)++;
        return arr;
    } else if (strncmp(*s, "true", 4) == 0) {
        *s += 4;
        return json_bool(true);
    } else if (strncmp(*s, "false", 5) == 0) {
        *s += 5;
        return json_bool(false);
    } else if (strncmp(*s, "null", 4) == 0) {
        *s += 4;
        return json_new(JSON_NULL);
    } else if (**s == '-' || isdigit(**s)) {
        char *end;
        double n = strtod(*s, &end);
        *s = end;
        return json_number(n);
    }
    return NULL;
}

static JsonValue *json_parse(const char *s) {
    return json_parse_value(&s);
}

// JSON stringify
static void json_stringify_to(JsonValue *v, char **buf, int *len, int *cap) {
    #define APPEND(...) do { \
        int n = snprintf(NULL, 0, __VA_ARGS__); \
        while (*len + n + 1 >= *cap) { *cap *= 2; *buf = realloc(*buf, *cap); } \
        *len += sprintf(*buf + *len, __VA_ARGS__); \
    } while(0)

    if (!v) { APPEND("null"); return; }
    switch (v->type) {
        case JSON_NULL: APPEND("null"); break;
        case JSON_BOOL: APPEND(v->d.boolean ? "true" : "false"); break;
        case JSON_NUMBER:
            if (v->d.number == (int)v->d.number)
                APPEND("%d", (int)v->d.number);
            else
                APPEND("%g", v->d.number);
            break;
        case JSON_STRING: {
            APPEND("\"");
            for (char *p = v->d.string; *p; p++) {
                if (*p == '"') APPEND("\\\"");
                else if (*p == '\\') APPEND("\\\\");
                else if (*p == '\n') APPEND("\\n");
                else if (*p == '\r') APPEND("\\r");
                else if (*p == '\t') APPEND("\\t");
                else APPEND("%c", *p);
            }
            APPEND("\"");
            break;
        }
        case JSON_ARRAY:
            APPEND("[");
            for (int i = 0; i < v->d.array.count; i++) {
                if (i > 0) APPEND(",");
                json_stringify_to(v->d.array.items[i], buf, len, cap);
            }
            APPEND("]");
            break;
        case JSON_OBJECT:
            APPEND("{");
            for (int i = 0; i < v->d.object.count; i++) {
                if (i > 0) APPEND(",");
                APPEND("\"%s\":", v->d.object.pairs[i].key);
                json_stringify_to(v->d.object.pairs[i].value, buf, len, cap);
            }
            APPEND("}");
            break;
    }
    #undef APPEND
}

static char *json_stringify(JsonValue *v) {
    int len = 0, cap = 256;
    char *buf = malloc(cap);
    buf[0] = '\0';
    json_stringify_to(v, &buf, &len, &cap);
    return buf;
}

// ============== LSP Protocol ==============

static FILE *log_file = NULL;

static void log_msg(const char *fmt, ...) {
    if (!log_file) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(log_file, fmt, args);
    fprintf(log_file, "\n");
    fflush(log_file);
    va_end(args);
}

static void send_response(int id, JsonValue *result) {
    JsonValue *response = json_object();
    json_object_set(response, "jsonrpc", json_string("2.0"));
    json_object_set(response, "id", json_number(id));
    json_object_set(response, "result", result);

    char *json = json_stringify(response);
    printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
    fflush(stdout);

    log_msg("Sent response: %s", json);
    free(json);
    json_free(response);
}

static void send_notification(const char *method, JsonValue *params) {
    JsonValue *notif = json_object();
    json_object_set(notif, "jsonrpc", json_string("2.0"));
    json_object_set(notif, "method", json_string(method));
    json_object_set(notif, "params", params);

    char *json = json_stringify(notif);
    printf("Content-Length: %zu\r\n\r\n%s", strlen(json), json);
    fflush(stdout);

    log_msg("Sent notification: %s", json);
    free(json);
    json_free(notif);
}

// ============== Document Storage ==============

typedef struct {
    char *uri;
    char *content;
    char **lines;
    int line_count;
} Document;

static Document *documents = NULL;
static int doc_count = 0;
static int doc_cap = 0;

static void split_lines(Document *doc) {
    // Free old lines
    if (doc->lines) {
        for (int i = 0; i < doc->line_count; i++) free(doc->lines[i]);
        free(doc->lines);
    }

    // Count lines
    int count = 1;
    for (char *p = doc->content; *p; p++) if (*p == '\n') count++;

    doc->lines = calloc(count, sizeof(char*));
    doc->line_count = 0;

    char *start = doc->content;
    for (char *p = doc->content; ; p++) {
        if (*p == '\n' || *p == '\0') {
            int len = p - start;
            doc->lines[doc->line_count] = malloc(len + 1);
            memcpy(doc->lines[doc->line_count], start, len);
            doc->lines[doc->line_count][len] = '\0';
            doc->line_count++;
            if (*p == '\0') break;
            start = p + 1;
        }
    }
}

static Document *find_document(const char *uri) {
    for (int i = 0; i < doc_count; i++) {
        if (strcmp(documents[i].uri, uri) == 0) return &documents[i];
    }
    return NULL;
}

static Document *add_document(const char *uri, const char *content) {
    if (doc_count >= doc_cap) {
        doc_cap = doc_cap ? doc_cap * 2 : 8;
        documents = realloc(documents, doc_cap * sizeof(Document));
    }
    Document *doc = &documents[doc_count++];
    doc->uri = strdup(uri);
    doc->content = strdup(content);
    doc->lines = NULL;
    doc->line_count = 0;
    split_lines(doc);
    return doc;
}

static void update_document(Document *doc, const char *content) {
    free(doc->content);
    doc->content = strdup(content);
    split_lines(doc);
}

// ============== Markdown Analysis ==============

typedef struct {
    char *text;      // Heading text
    char *anchor;    // Slugified anchor
    int line;
} Heading;

static Heading *headings = NULL;
static int heading_count = 0;
static int heading_cap = 0;

static char *slugify(const char *text) {
    int len = strlen(text);
    char *slug = malloc(len + 1);
    int j = 0;

    for (int i = 0; i < len; i++) {
        char c = text[i];
        if (isalnum(c)) {
            slug[j++] = tolower(c);
        } else if (c == ' ' || c == '-') {
            if (j > 0 && slug[j-1] != '-') slug[j++] = '-';
        }
    }
    // Trim trailing dash
    while (j > 0 && slug[j-1] == '-') j--;
    slug[j] = '\0';
    return slug;
}

static void clear_headings(void) {
    for (int i = 0; i < heading_count; i++) {
        free(headings[i].text);
        free(headings[i].anchor);
    }
    heading_count = 0;
}

static void add_heading(const char *text, int line) {
    if (heading_count >= heading_cap) {
        heading_cap = heading_cap ? heading_cap * 2 : 16;
        headings = realloc(headings, heading_cap * sizeof(Heading));
    }
    headings[heading_count].text = strdup(text);
    headings[heading_count].anchor = slugify(text);
    headings[heading_count].line = line;
    heading_count++;
}

static void extract_headings(Document *doc) {
    clear_headings();

    for (int i = 0; i < doc->line_count; i++) {
        char *line = doc->lines[i];

        // Skip leading whitespace
        while (*line == ' ' || *line == '\t') line++;

        // Check for heading
        if (*line == '#') {
            while (*line == '#') line++;
            if (*line == ' ' || *line == '\t') {
                line++;  // Skip space after #
                // Trim trailing # and whitespace
                char *end = line + strlen(line) - 1;
                while (end > line && (*end == '#' || *end == ' ' || *end == '\t' || *end == '\n'))
                    end--;
                int len = end - line + 1;
                char *text = malloc(len + 1);
                memcpy(text, line, len);
                text[len] = '\0';
                add_heading(text, i);
                free(text);
            }
        }
    }
}

static bool heading_anchor_exists(const char *anchor) {
    for (int i = 0; i < heading_count; i++) {
        if (strcmp(headings[i].anchor, anchor) == 0) return true;
    }
    return false;
}

// ============== Diagnostics ==============

static JsonValue *create_diagnostic(int line, int start_col, int end_col,
                                     int severity, const char *message) {
    JsonValue *diag = json_object();

    JsonValue *range = json_object();
    JsonValue *start_pos = json_object();
    json_object_set(start_pos, "line", json_number(line));
    json_object_set(start_pos, "character", json_number(start_col));
    JsonValue *end_pos = json_object();
    json_object_set(end_pos, "line", json_number(line));
    json_object_set(end_pos, "character", json_number(end_col));
    json_object_set(range, "start", start_pos);
    json_object_set(range, "end", end_pos);

    json_object_set(diag, "range", range);
    json_object_set(diag, "severity", json_number(severity));
    json_object_set(diag, "source", json_string("md-lsp"));
    json_object_set(diag, "message", json_string(message));

    return diag;
}

static void publish_diagnostics(Document *doc) {
    extract_headings(doc);

    JsonValue *diagnostics = json_array();

    // Check for duplicate headings
    for (int i = 0; i < heading_count; i++) {
        for (int j = i + 1; j < heading_count; j++) {
            if (strcmp(headings[i].anchor, headings[j].anchor) == 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Duplicate heading anchor '#%s'", headings[i].anchor);
                json_array_push(diagnostics, create_diagnostic(
                    headings[j].line, 0, strlen(doc->lines[headings[j].line]),
                    2, msg));  // 2 = Warning
            }
        }
    }

    // Check for broken heading links
    for (int i = 0; i < doc->line_count; i++) {
        char *line = doc->lines[i];
        char *p = line;

        while ((p = strstr(p, "](#")) != NULL) {
            char *start = p + 3;  // Skip ](#
            char *end = strchr(start, ')');
            if (end) {
                int len = end - start;
                char *anchor = malloc(len + 1);
                memcpy(anchor, start, len);
                anchor[len] = '\0';

                if (!heading_anchor_exists(anchor)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Broken heading link '#%s'", anchor);
                    int col = p - line;
                    json_array_push(diagnostics, create_diagnostic(
                        i, col, col + len + 4,
                        1, msg));  // 1 = Error
                }
                free(anchor);
            }
            p++;
        }
    }

    // Check for unclosed formatting
    for (int i = 0; i < doc->line_count; i++) {
        char *line = doc->lines[i];
        int bold_count = 0, italic_count = 0, code_count = 0;

        for (int j = 0; line[j]; j++) {
            if (line[j] == '`' && (j == 0 || line[j-1] != '\\')) {
                code_count++;
            } else if (line[j] == '*' && line[j+1] == '*') {
                bold_count++;
                j++;
            } else if (line[j] == '*' && (j == 0 || line[j-1] != '*') && line[j+1] != '*') {
                italic_count++;
            }
        }

        if (code_count % 2 != 0) {
            json_array_push(diagnostics, create_diagnostic(
                i, 0, strlen(line), 2, "Unclosed inline code"));
        }
        if (bold_count % 2 != 0) {
            json_array_push(diagnostics, create_diagnostic(
                i, 0, strlen(line), 2, "Unclosed bold formatting"));
        }
        if (italic_count % 2 != 0) {
            json_array_push(diagnostics, create_diagnostic(
                i, 0, strlen(line), 2, "Unclosed italic formatting"));
        }
    }

    // Send diagnostics
    JsonValue *params = json_object();
    json_object_set(params, "uri", json_string(doc->uri));
    json_object_set(params, "diagnostics", diagnostics);
    send_notification("textDocument/publishDiagnostics", params);
}

// ============== Semantic Tokens ==============

// Token types (must match legend)
enum {
    TT_HEADING = 0,
    TT_BOLD,
    TT_ITALIC,
    TT_CODE,
    TT_LINK,
    TT_STRING,  // For link URLs
};

static int *token_data = NULL;
static int token_data_count = 0;
static int token_data_cap = 0;

static void clear_tokens(void) {
    token_data_count = 0;
}

static void add_token(int delta_line, int delta_start, int length, int type, int modifiers) {
    if (token_data_count + 5 > token_data_cap) {
        token_data_cap = token_data_cap ? token_data_cap * 2 : 256;
        token_data = realloc(token_data, token_data_cap * sizeof(int));
    }
    token_data[token_data_count++] = delta_line;
    token_data[token_data_count++] = delta_start;
    token_data[token_data_count++] = length;
    token_data[token_data_count++] = type;
    token_data[token_data_count++] = modifiers;
}

static void compute_semantic_tokens(Document *doc) {
    clear_tokens();

    int prev_line = 0, prev_char = 0;
    bool in_code_block = false;

    for (int i = 0; i < doc->line_count; i++) {
        char *line = doc->lines[i];
        int len = strlen(line);

        // Check for code fence
        char *trimmed = line;
        while (*trimmed == ' ') trimmed++;
        if (strncmp(trimmed, "```", 3) == 0 || strncmp(trimmed, "~~~", 3) == 0) {
            in_code_block = !in_code_block;
            // Highlight the fence line as code
            int delta_line = i - prev_line;
            int delta_start = (delta_line == 0) ? 0 - prev_char : 0;
            add_token(delta_line, delta_start, len, TT_CODE, 0);
            prev_line = i;
            prev_char = 0;
            continue;
        }

        if (in_code_block) {
            // Entire line is code
            int delta_line = i - prev_line;
            int delta_start = (delta_line == 0) ? 0 - prev_char : 0;
            add_token(delta_line, delta_start, len, TT_CODE, 0);
            prev_line = i;
            prev_char = 0;
            continue;
        }

        // Check for heading
        if (*trimmed == '#') {
            int delta_line = i - prev_line;
            int delta_start = (delta_line == 0) ? 0 - prev_char : 0;
            add_token(delta_line, delta_start, len, TT_HEADING, 0);
            prev_line = i;
            prev_char = 0;
            continue;
        }

        // Scan for inline formatting
        for (int j = 0; j < len; j++) {
            // Inline code
            if (line[j] == '`') {
                int start = j;
                j++;
                while (j < len && line[j] != '`') j++;
                if (j < len) {
                    int delta_line = i - prev_line;
                    int delta_start = (delta_line == 0) ? start - prev_char : start;
                    add_token(delta_line, delta_start, j - start + 1, TT_CODE, 0);
                    prev_line = i;
                    prev_char = start;
                }
                continue;
            }

            // Bold **text**
            if (line[j] == '*' && j + 1 < len && line[j+1] == '*') {
                int start = j;
                j += 2;
                while (j + 1 < len && !(line[j] == '*' && line[j+1] == '*')) j++;
                if (j + 1 < len) {
                    j++;
                    int delta_line = i - prev_line;
                    int delta_start = (delta_line == 0) ? start - prev_char : start;
                    add_token(delta_line, delta_start, j - start + 1, TT_BOLD, 0);
                    prev_line = i;
                    prev_char = start;
                }
                continue;
            }

            // Italic *text*
            if (line[j] == '*' && (j == 0 || line[j-1] != '*') &&
                j + 1 < len && line[j+1] != '*' && line[j+1] != ' ') {
                int start = j;
                j++;
                while (j < len && line[j] != '*') j++;
                if (j < len) {
                    int delta_line = i - prev_line;
                    int delta_start = (delta_line == 0) ? start - prev_char : start;
                    add_token(delta_line, delta_start, j - start + 1, TT_ITALIC, 0);
                    prev_line = i;
                    prev_char = start;
                }
                continue;
            }

            // Links [text](url)
            if (line[j] == '[') {
                int start = j;
                j++;
                while (j < len && line[j] != ']') j++;
                if (j < len && j + 1 < len && line[j+1] == '(') {
                    j += 2;
                    while (j < len && line[j] != ')') j++;
                    if (j < len) {
                        int delta_line = i - prev_line;
                        int delta_start = (delta_line == 0) ? start - prev_char : start;
                        add_token(delta_line, delta_start, j - start + 1, TT_LINK, 0);
                        prev_line = i;
                        prev_char = start;
                    }
                }
                continue;
            }
        }
    }
}

// ============== LSP Handlers ==============

static bool initialized = false;

static void handle_initialize(int id, JsonValue *params) {
    (void)params;

    JsonValue *result = json_object();

    JsonValue *capabilities = json_object();

    // Text document sync - full sync
    json_object_set(capabilities, "textDocumentSync", json_number(1));

    // Semantic tokens
    JsonValue *semantic = json_object();
    JsonValue *legend = json_object();

    JsonValue *token_types = json_array();
    json_array_push(token_types, json_string("keyword"));    // heading
    json_array_push(token_types, json_string("macro"));      // bold
    json_array_push(token_types, json_string("comment"));    // italic
    json_array_push(token_types, json_string("string"));     // code
    json_array_push(token_types, json_string("function"));   // link
    json_array_push(token_types, json_string("string"));     // url
    json_object_set(legend, "tokenTypes", token_types);

    JsonValue *token_modifiers = json_array();
    json_object_set(legend, "tokenModifiers", token_modifiers);

    json_object_set(semantic, "legend", legend);
    json_object_set(semantic, "full", json_bool(true));
    json_object_set(capabilities, "semanticTokensProvider", semantic);

    json_object_set(result, "capabilities", capabilities);

    JsonValue *server_info = json_object();
    json_object_set(server_info, "name", json_string("md-lsp"));
    json_object_set(server_info, "version", json_string("0.1.0"));
    json_object_set(result, "serverInfo", server_info);

    send_response(id, result);
}

static void handle_initialized(void) {
    initialized = true;
    log_msg("Server initialized");
}

static void handle_did_open(JsonValue *params) {
    JsonValue *td = json_object_get(params, "textDocument");
    if (!td) return;

    const char *uri = json_get_string(json_object_get(td, "uri"));
    const char *text = json_get_string(json_object_get(td, "text"));

    if (uri && text) {
        Document *doc = add_document(uri, text);
        publish_diagnostics(doc);
    }
}

static void handle_did_change(JsonValue *params) {
    JsonValue *td = json_object_get(params, "textDocument");
    if (!td) return;

    const char *uri = json_get_string(json_object_get(td, "uri"));
    JsonValue *changes = json_object_get(params, "contentChanges");

    if (!uri || !changes || changes->type != JSON_ARRAY || changes->d.array.count == 0)
        return;

    // Full sync - take the last change
    JsonValue *change = changes->d.array.items[changes->d.array.count - 1];
    const char *text = json_get_string(json_object_get(change, "text"));

    if (text) {
        Document *doc = find_document(uri);
        if (doc) {
            update_document(doc, text);
            publish_diagnostics(doc);
        }
    }
}

static void handle_did_close(JsonValue *params) {
    JsonValue *td = json_object_get(params, "textDocument");
    if (!td) return;

    const char *uri = json_get_string(json_object_get(td, "uri"));
    if (!uri) return;

    // Clear diagnostics
    JsonValue *diag_params = json_object();
    json_object_set(diag_params, "uri", json_string(uri));
    json_object_set(diag_params, "diagnostics", json_array());
    send_notification("textDocument/publishDiagnostics", diag_params);

    // Remove document (simplified - just mark as removed)
    for (int i = 0; i < doc_count; i++) {
        if (strcmp(documents[i].uri, uri) == 0) {
            free(documents[i].uri);
            free(documents[i].content);
            for (int j = 0; j < documents[i].line_count; j++)
                free(documents[i].lines[j]);
            free(documents[i].lines);
            // Shift remaining
            memmove(&documents[i], &documents[i+1], (doc_count - i - 1) * sizeof(Document));
            doc_count--;
            break;
        }
    }
}

static void handle_semantic_tokens_full(int id, JsonValue *params) {
    JsonValue *td = json_object_get(params, "textDocument");
    if (!td) {
        send_response(id, json_object());
        return;
    }

    const char *uri = json_get_string(json_object_get(td, "uri"));
    Document *doc = find_document(uri);

    if (!doc) {
        send_response(id, json_object());
        return;
    }

    compute_semantic_tokens(doc);

    JsonValue *result = json_object();
    JsonValue *data = json_array();

    for (int i = 0; i < token_data_count; i++) {
        json_array_push(data, json_number(token_data[i]));
    }

    json_object_set(result, "data", data);
    send_response(id, result);
}

static void handle_shutdown(int id) {
    send_response(id, json_new(JSON_NULL));
}

static void handle_exit(void) {
    exit(0);
}

// ============== Main Loop ==============

static char *read_message(void) {
    // Read headers
    int content_length = 0;
    char header[256];

    while (fgets(header, sizeof(header), stdin)) {
        if (strcmp(header, "\r\n") == 0 || strcmp(header, "\n") == 0)
            break;

        if (strncmp(header, "Content-Length:", 15) == 0) {
            content_length = atoi(header + 15);
        }
    }

    if (content_length == 0) return NULL;

    char *content = malloc(content_length + 1);
    if (fread(content, 1, content_length, stdin) != (size_t)content_length) {
        free(content);
        return NULL;
    }
    content[content_length] = '\0';

    return content;
}

static void process_message(const char *msg) {
    log_msg("Received: %s", msg);

    JsonValue *request = json_parse(msg);
    if (!request) return;

    const char *method = json_get_string(json_object_get(request, "method"));
    JsonValue *id_val = json_object_get(request, "id");
    int id = id_val ? (int)json_get_number(id_val) : -1;
    JsonValue *params = json_object_get(request, "params");

    if (!method) {
        json_free(request);
        return;
    }

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(id, params);
    } else if (strcmp(method, "initialized") == 0) {
        handle_initialized();
    } else if (strcmp(method, "shutdown") == 0) {
        handle_shutdown(id);
    } else if (strcmp(method, "exit") == 0) {
        handle_exit();
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
        handle_did_open(params);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        handle_did_change(params);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        handle_did_close(params);
    } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
        handle_semantic_tokens_full(id, params);
    } else if (id >= 0) {
        // Unknown request - send null response
        send_response(id, json_new(JSON_NULL));
    }

    json_free(request);
}

int main(int argc, char **argv) {
    // Optional: enable logging
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_file = fopen(argv[i + 1], "w");
        }
    }

    log_msg("md-lsp started");

    // Disable buffering
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    while (1) {
        char *msg = read_message();
        if (!msg) break;
        process_message(msg);
        free(msg);
    }

    log_msg("md-lsp exiting");
    if (log_file) fclose(log_file);

    return 0;
}
