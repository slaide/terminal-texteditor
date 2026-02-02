#define _GNU_SOURCE
#include "lsp.h"
#include "editor_config.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

// Pending request tracking
typedef enum {
    REQ_SEMANTIC_TOKENS,
    REQ_HOVER,
    REQ_COMPLETION
} PendingRequestType;

typedef struct {
    int id;
    char *uri;  // For semantic tokens and hover, track which file
    PendingRequestType type;
    int line;
    int col;
} PendingRequest;

// LSP client state
static struct {
    pid_t pid;
    int stdin_fd;   // Write to clangd
    int stdout_fd;  // Read from clangd
    int request_id;
    bool initialized;
    bool running;
    char *command;
    lsp_diagnostics_callback diagnostics_cb;
    lsp_semantic_tokens_callback semantic_cb;
    lsp_hover_callback hover_cb;
    lsp_completion_callback completion_cb;
    bool hover_supported;
    bool completion_supported;

    // Read buffer for incoming messages
    char *read_buf;
    int read_buf_len;
    int read_buf_capacity;

    // Pending requests (for matching responses)
    PendingRequest *pending;
    int pending_count;
    int pending_capacity;

    // Semantic token type legend (from server capabilities)
    char **token_types;
    int token_type_count;
} lsp = {0};

// Forward declarations
static bool send_message(JsonValue *msg);
static void handle_message(JsonValue *msg);
static char *completion_doc_to_text(JsonValue *doc);
static void free_completion_items(LspCompletionItem *items, int count);

// Add a pending request
static void add_pending_request(int id, const char *uri, PendingRequestType type, int line, int col) {
    if (lsp.pending_count >= lsp.pending_capacity) {
        int new_cap = lsp.pending_capacity == 0 ? 8 : lsp.pending_capacity * 2;
        PendingRequest *new_pending = realloc(lsp.pending, new_cap * sizeof(PendingRequest));
        if (!new_pending) return;
        lsp.pending = new_pending;
        lsp.pending_capacity = new_cap;
    }
    lsp.pending[lsp.pending_count].id = id;
    lsp.pending[lsp.pending_count].uri = uri ? strdup(uri) : NULL;
    lsp.pending[lsp.pending_count].type = type;
    lsp.pending[lsp.pending_count].line = line;
    lsp.pending[lsp.pending_count].col = col;
    lsp.pending_count++;
}

// Find and remove a pending request by ID
static bool pop_pending_request(int id, PendingRequest *out) {
    for (int i = 0; i < lsp.pending_count; i++) {
        if (lsp.pending[i].id == id) {
            if (out) {
                *out = lsp.pending[i];
            }
            // Remove by shifting
            for (int j = i; j < lsp.pending_count - 1; j++) {
                lsp.pending[j] = lsp.pending[j + 1];
            }
            lsp.pending_count--;
            return true;
        }
    }
    return false;
}

// Map clangd token type string to our enum
static SemanticTokenType map_token_type(int index) {
    if (index < 0 || index >= lsp.token_type_count || !lsp.token_types) {
        return TOKEN_UNKNOWN;
    }
    const char *type = lsp.token_types[index];
    if (!type) return TOKEN_UNKNOWN;

    if (strcmp(type, "variable") == 0) return TOKEN_VARIABLE;
    if (strcmp(type, "parameter") == 0) return TOKEN_PARAMETER;
    if (strcmp(type, "function") == 0) return TOKEN_FUNCTION;
    if (strcmp(type, "method") == 0) return TOKEN_METHOD;
    if (strcmp(type, "property") == 0) return TOKEN_PROPERTY;
    if (strcmp(type, "class") == 0) return TOKEN_CLASS;
    if (strcmp(type, "enum") == 0) return TOKEN_ENUM;
    if (strcmp(type, "enumMember") == 0) return TOKEN_ENUM_MEMBER;
    if (strcmp(type, "type") == 0) return TOKEN_TYPE;
    if (strcmp(type, "namespace") == 0) return TOKEN_NAMESPACE;
    if (strcmp(type, "keyword") == 0) return TOKEN_KEYWORD;
    if (strcmp(type, "modifier") == 0) return TOKEN_MODIFIER;
    if (strcmp(type, "comment") == 0) return TOKEN_COMMENT;
    if (strcmp(type, "string") == 0) return TOKEN_STRING;
    if (strcmp(type, "number") == 0) return TOKEN_NUMBER;
    if (strcmp(type, "operator") == 0) return TOKEN_OPERATOR;
    if (strcmp(type, "macro") == 0) return TOKEN_MACRO;

    return TOKEN_UNKNOWN;
}

char *lsp_path_to_uri(const char *path) {
    if (!path) return NULL;

    // Get absolute path
    char *abs_path = realpath(path, NULL);
    if (!abs_path) {
        // File doesn't exist yet, use the path as-is
        abs_path = strdup(path);
    }

    // URI format: file:///path/to/file
    int len = strlen(abs_path) + 8; // "file://" + path + null
    char *uri = malloc(len);
    if (uri) {
        snprintf(uri, len, "file://%s", abs_path);
    }
    free(abs_path);
    return uri;
}

char *lsp_uri_to_path(const char *uri) {
    if (!uri) return NULL;

    // Strip "file://" prefix
    if (strncmp(uri, "file://", 7) == 0) {
        return strdup(uri + 7);
    }
    return strdup(uri);
}

static bool send_raw(const char *data, int len) {
    int written = 0;
    while (written < len) {
        int n = write(lsp.stdin_fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += n;
    }
    return true;
}

static bool send_message(JsonValue *msg) {
    if (!lsp.running || !msg) return false;

    char *content = json_stringify(msg);
    if (!content) return false;

    int content_len = strlen(content);

    // Send header
    char header[64];
    int header_len = snprintf(header, sizeof(header),
                              "Content-Length: %d\r\n\r\n", content_len);

    bool ok = send_raw(header, header_len) && send_raw(content, content_len);
    free(content);
    return ok;
}

static JsonValue *create_request(const char *method, JsonValue *params) {
    JsonValue *msg = json_object();
    json_object_set(msg, "jsonrpc", json_string("2.0"));
    json_object_set(msg, "id", json_number(++lsp.request_id));
    json_object_set(msg, "method", json_string(method));
    if (params) {
        json_object_set(msg, "params", params);
    }
    return msg;
}

static JsonValue *create_notification(const char *method, JsonValue *params) {
    JsonValue *msg = json_object();
    json_object_set(msg, "jsonrpc", json_string("2.0"));
    json_object_set(msg, "method", json_string(method));
    if (params) {
        json_object_set(msg, "params", params);
    }
    return msg;
}

static void handle_semantic_tokens_response(const char *uri, JsonValue *result) {
    if (!result || !lsp.semantic_cb) return;

    JsonValue *data = json_object_get(result, "data");
    if (!data) return;

    int data_len = json_array_length(data);
    if (data_len == 0 || data_len % 5 != 0) {
        // Empty or invalid data
        lsp.semantic_cb(uri, NULL, 0);
        return;
    }

    int token_count = data_len / 5;
    SemanticToken *tokens = calloc(token_count, sizeof(SemanticToken));
    if (!tokens) return;

    // Decode delta-encoded tokens
    // Format: [deltaLine, deltaStartChar, length, tokenType, tokenModifiers] * N
    int line = 0;
    int col = 0;

    for (int i = 0; i < token_count; i++) {
        int deltaLine = (int)json_get_number(json_array_get(data, i * 5 + 0));
        int deltaCol = (int)json_get_number(json_array_get(data, i * 5 + 1));
        int length = (int)json_get_number(json_array_get(data, i * 5 + 2));
        int tokenType = (int)json_get_number(json_array_get(data, i * 5 + 3));
        // int tokenModifiers = (int)json_get_number(json_array_get(data, i * 5 + 4));

        if (deltaLine > 0) {
            line += deltaLine;
            col = deltaCol;
        } else {
            col += deltaCol;
        }

        tokens[i].line = line;
        tokens[i].col = col;
        tokens[i].length = length;
        tokens[i].type = map_token_type(tokenType);
    }

    lsp.semantic_cb(uri, tokens, token_count);
    free(tokens);
}

static void append_text(char **buf, int *len, int *cap, const char *text) {
    if (!text || !buf || !len || !cap) return;
    int add_len = (int)strlen(text);
    if (add_len == 0) return;
    if (*len + add_len + 1 > *cap) {
        int new_cap = *cap == 0 ? 128 : *cap;
        while (new_cap < *len + add_len + 1) new_cap *= 2;
        char *new_buf = realloc(*buf, new_cap);
        if (!new_buf) return;
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, text, add_len);
    *len += add_len;
    (*buf)[*len] = '\0';
}

static char *completion_doc_to_text(JsonValue *doc) {
    if (!doc) return NULL;
    if (doc->type == JSON_STRING) {
        const char *s = json_get_string(doc);
        return s ? strdup(s) : NULL;
    }
    if (doc->type == JSON_OBJECT) {
        JsonValue *value = json_object_get(doc, "value");
        if (value && value->type == JSON_STRING) {
            const char *s = json_get_string(value);
            return s ? strdup(s) : NULL;
        }
    }
    return NULL;
}

static void free_completion_items(LspCompletionItem *items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) {
        free(items[i].label);
        free(items[i].detail);
        free(items[i].documentation);
    }
}

static void append_segment(char **buf, int *len, int *cap, const char *text) {
    if (!text || !buf || !len || !cap) return;
    if (*len > 0 && (*buf)[*len - 1] != '\n') {
        append_text(buf, len, cap, "\n");
    }
    append_text(buf, len, cap, text);
}

static char *strip_markdown_fences(const char *text) {
    if (!text) return NULL;
    char *out = NULL;
    int len = 0;
    int cap = 0;
    const char *line = text;
    while (line && *line) {
        const char *next = strchr(line, '\n');
        int line_len = next ? (int)(next - line) : (int)strlen(line);
        bool fence = (line_len >= 3 && strncmp(line, "```", 3) == 0);
        if (!fence) {
            if (len > 0) append_text(&out, &len, &cap, "\n");
            if (line_len > 0) {
                char *tmp = strndup(line, line_len);
                if (tmp) {
                    append_text(&out, &len, &cap, tmp);
                    free(tmp);
                }
            }
        }
        if (!next) break;
        line = next + 1;
    }
    return out;
}

static void append_marked_string(char **buf, int *len, int *cap, JsonValue *val) {
    if (!val) return;
    if (val->type == JSON_STRING) {
        append_segment(buf, len, cap, json_get_string(val));
        return;
    }
    if (val->type == JSON_OBJECT) {
        JsonValue *value = json_object_get(val, "value");
        if (value && value->type == JSON_STRING) {
            append_segment(buf, len, cap, json_get_string(value));
        }
    }
}

static char *hover_contents_to_text(JsonValue *contents) {
    if (!contents) return NULL;
    char *out = NULL;
    int len = 0;
    int cap = 0;

    if (contents->type == JSON_STRING) {
        append_segment(&out, &len, &cap, json_get_string(contents));
        return out;
    }
    if (contents->type == JSON_ARRAY) {
        int count = json_array_length(contents);
        for (int i = 0; i < count; i++) {
            JsonValue *item = json_array_get(contents, i);
            append_marked_string(&out, &len, &cap, item);
        }
        return out;
    }
    if (contents->type == JSON_OBJECT) {
        JsonValue *kind = json_object_get(contents, "kind");
        JsonValue *value = json_object_get(contents, "value");
        const char *kind_str = kind ? json_get_string(kind) : NULL;
        const char *value_str = value ? json_get_string(value) : NULL;
        if (value_str && kind_str && strcmp(kind_str, "markdown") == 0) {
            char *stripped = strip_markdown_fences(value_str);
            if (stripped) {
                append_segment(&out, &len, &cap, stripped);
                free(stripped);
                return out;
            }
        }
        if (value_str) {
            append_segment(&out, &len, &cap, value_str);
        }
        return out;
    }

    return out;
}

static void handle_diagnostics(JsonValue *params) {
    if (!params || !lsp.diagnostics_cb) return;

    JsonValue *uri_val = json_object_get(params, "uri");
    JsonValue *diags_arr = json_object_get(params, "diagnostics");

    if (!uri_val || !diags_arr) return;

    const char *uri = json_get_string(uri_val);
    int count = json_array_length(diags_arr);

    Diagnostic *diags = NULL;
    if (count > 0) {
        diags = calloc(count, sizeof(Diagnostic));
        if (!diags) return;

        for (int i = 0; i < count; i++) {
            JsonValue *d = json_array_get(diags_arr, i);
            if (!d) continue;

            // Get range
            JsonValue *range = json_object_get(d, "range");
            if (range) {
                JsonValue *start = json_object_get(range, "start");
                JsonValue *end = json_object_get(range, "end");
                if (start) {
                    JsonValue *line = json_object_get(start, "line");
                    JsonValue *col = json_object_get(start, "character");
                    if (line) diags[i].line = (int)json_get_number(line);
                    if (col) diags[i].col = (int)json_get_number(col);
                }
                if (end) {
                    JsonValue *line = json_object_get(end, "line");
                    JsonValue *col = json_object_get(end, "character");
                    if (line) diags[i].end_line = (int)json_get_number(line);
                    if (col) diags[i].end_col = (int)json_get_number(col);
                }
            }

            // Get severity
            JsonValue *severity = json_object_get(d, "severity");
            if (severity) {
                diags[i].severity = (DiagnosticSeverity)(int)json_get_number(severity);
            } else {
                diags[i].severity = DIAG_ERROR;
            }

            // Get message
            JsonValue *message = json_object_get(d, "message");
            if (message) {
                const char *msg = json_get_string(message);
                if (msg) diags[i].message = strdup(msg);
            }

            // Get source
            JsonValue *source = json_object_get(d, "source");
            if (source) {
                const char *src = json_get_string(source);
                if (src) diags[i].source = strdup(src);
            }
        }
    }

    // Call the callback
    lsp.diagnostics_cb(uri, diags, count);

    // Clean up (callback should copy what it needs)
    for (int i = 0; i < count; i++) {
        free(diags[i].message);
        free(diags[i].source);
    }
    free(diags);
}

static void handle_hover_response(const PendingRequest *req, JsonValue *result) {
    if (!req || !lsp.hover_cb) return;
    if (!result) {
        lsp.hover_cb(req->uri, req->line, req->col, NULL);
        return;
    }
    JsonValue *contents = json_object_get(result, "contents");
    if (!contents) {
        lsp.hover_cb(req->uri, req->line, req->col, NULL);
        return;
    }
    char *text = hover_contents_to_text(contents);
    if (!text || text[0] == '\0') {
        free(text);
        lsp.hover_cb(req->uri, req->line, req->col, NULL);
        return;
    }
    lsp.hover_cb(req->uri, req->line, req->col, text);
    free(text);
}

static void handle_message(JsonValue *msg) {
    if (!msg) return;

    // Check if it's a notification
    JsonValue *method = json_object_get(msg, "method");
    if (method) {
        const char *method_str = json_get_string(method);
        if (method_str) {
            if (strcmp(method_str, "textDocument/publishDiagnostics") == 0) {
                JsonValue *params = json_object_get(msg, "params");
                handle_diagnostics(params);
            }
            // Handle other notifications as needed
        }
    }

    // Check if it's a response
    JsonValue *id = json_object_get(msg, "id");
    if (id) {
        int req_id = (int)json_get_number(id);
        JsonValue *result = json_object_get(msg, "result");
        JsonValue *error = json_object_get(msg, "error");
        const char *error_msg = NULL;
        if (error && error->type == JSON_OBJECT) {
            JsonValue *err_message = json_object_get(error, "message");
            if (err_message && err_message->type == JSON_STRING) {
                error_msg = json_get_string(err_message);
            }
        }

        // Check if this is the initialize response
        if (req_id == 1 && result) {
            // Parse semantic token legend from capabilities
            JsonValue *caps = json_object_get(result, "capabilities");
            if (caps) {
                JsonValue *hoverProvider = json_object_get(caps, "hoverProvider");
                if (hoverProvider) {
                    if (hoverProvider->type == JSON_BOOL) {
                        lsp.hover_supported = json_get_bool(hoverProvider);
                    } else if (hoverProvider->type == JSON_OBJECT) {
                        lsp.hover_supported = true;
                    }
                }
                JsonValue *completionProvider = json_object_get(caps, "completionProvider");
                if (completionProvider) {
                    if (completionProvider->type == JSON_BOOL) {
                        lsp.completion_supported = json_get_bool(completionProvider);
                    } else if (completionProvider->type == JSON_OBJECT) {
                        lsp.completion_supported = true;
                    }
                }
                JsonValue *semTokens = json_object_get(caps, "semanticTokensProvider");
                if (semTokens) {
                    JsonValue *legend = json_object_get(semTokens, "legend");
                    if (legend) {
                        JsonValue *tokenTypes = json_object_get(legend, "tokenTypes");
                        if (tokenTypes) {
                            int count = json_array_length(tokenTypes);
                            lsp.token_types = calloc(count, sizeof(char*));
                            lsp.token_type_count = count;
                            for (int i = 0; i < count; i++) {
                                const char *t = json_get_string(json_array_get(tokenTypes, i));
                                if (t) lsp.token_types[i] = strdup(t);
                            }
                        }
                    }
                }
            }
        }

        PendingRequest req = {0};
        if (pop_pending_request(req_id, &req)) {
            if (req.type == REQ_SEMANTIC_TOKENS) {
                if (req.uri && result) {
                    handle_semantic_tokens_response(req.uri, result);
                }
            } else if (req.type == REQ_HOVER) {
                if (!result && error_msg && lsp.hover_cb) {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "Hover error: %s", error_msg);
                    lsp.hover_cb(req.uri, req.line, req.col, buf);
                } else {
                    handle_hover_response(&req, result);
                }
            } else if (req.type == REQ_COMPLETION) {
                if (!lsp.completion_cb) {
                    // Nothing to do
                } else if (!result && error_msg) {
                    lsp.completion_cb(req.uri, req.line, req.col, NULL, 0);
                } else if (result) {
                    JsonValue *items = NULL;
                    bool is_incomplete = false;
                    if (result->type == JSON_ARRAY) {
                        items = result;
                    } else if (result->type == JSON_OBJECT) {
                        JsonValue *list_items = json_object_get(result, "items");
                        if (list_items && list_items->type == JSON_ARRAY) {
                            items = list_items;
                        }
                        JsonValue *incomplete = json_object_get(result, "isIncomplete");
                        if (incomplete && incomplete->type == JSON_BOOL) {
                            is_incomplete = json_get_bool(incomplete);
                        }
                    }

                    if (!items) {
                        lsp.completion_cb(req.uri, req.line, req.col, NULL, 0);
                    } else {
                        int count = json_array_length(items);
                        LspCompletionItem *out = NULL;
                        if (count > 0) {
                            out = calloc(count, sizeof(LspCompletionItem));
                            if (!out) {
                                lsp.completion_cb(req.uri, req.line, req.col, NULL, 0);
                                if (req.uri) free(req.uri);
                                return;
                            }
                        }
                        int out_count = 0;
                        for (int i = 0; i < count; i++) {
                            JsonValue *item = json_array_get(items, i);
                            if (!item || item->type != JSON_OBJECT) continue;

                            JsonValue *label = json_object_get(item, "label");
                            const char *label_str = label ? json_get_string(label) : NULL;
                            if (!label_str || label_str[0] == '\0') continue;

                            JsonValue *detail = json_object_get(item, "detail");
                            const char *detail_str = detail ? json_get_string(detail) : NULL;

                            JsonValue *documentation = json_object_get(item, "documentation");
                            char *doc_text = completion_doc_to_text(documentation);

                            out[out_count].label = strdup(label_str);
                            out[out_count].detail = detail_str ? strdup(detail_str) : NULL;
                            out[out_count].documentation = doc_text;
                            out_count++;
                        }

                        if (is_incomplete) {
                            // We don't implement resolution; still return what we have.
                        }

                        lsp.completion_cb(req.uri, req.line, req.col, out, out_count);
                        free_completion_items(out, out_count);
                        free(out);
                    }
                }
            }
            if (req.uri) free(req.uri);
        }
    }
}

static bool buf_ensure_capacity(int needed) {
    if (lsp.read_buf_len + needed >= lsp.read_buf_capacity) {
        int new_cap = lsp.read_buf_capacity == 0 ? 4096 : lsp.read_buf_capacity * 2;
        while (new_cap < lsp.read_buf_len + needed) new_cap *= 2;
        char *new_buf = realloc(lsp.read_buf, new_cap);
        if (!new_buf) return false;
        lsp.read_buf = new_buf;
        lsp.read_buf_capacity = new_cap;
    }
    return true;
}

static int find_header_end(const char *buf, int len) {
    for (int i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i;
        }
    }
    return -1;
}

static bool parse_content_length(const char *buf, int len, int *out_len) {
    const char key[] = "Content-Length:";
    int key_len = (int)sizeof(key) - 1;
    for (int i = 0; i + key_len <= len; i++) {
        if (memcmp(buf + i, key, key_len) == 0) {
            int j = i + key_len;
            while (j < len && (buf[j] == ' ' || buf[j] == '\t')) j++;
            int value = 0;
            bool found = false;
            while (j < len && buf[j] >= '0' && buf[j] <= '9') {
                value = value * 10 + (buf[j] - '0');
                found = true;
                j++;
            }
            if (!found) return false;
            *out_len = value;
            return true;
        }
    }
    return false;
}

// Parse command string into argv array
static char **parse_command(const char *command, int *argc) {
    if (!command || !argc) return NULL;

    // Count words (rough estimate for allocation)
    int capacity = 8;
    char **argv = calloc(capacity, sizeof(char*));
    if (!argv) return NULL;

    char *cmd_copy = strdup(command);
    if (!cmd_copy) {
        free(argv);
        return NULL;
    }

    int count = 0;
    char *saveptr;
    char *token = strtok_r(cmd_copy, " \t", &saveptr);

    while (token) {
        if (count >= capacity - 1) {
            capacity *= 2;
            char **new_argv = realloc(argv, capacity * sizeof(char*));
            if (!new_argv) {
                for (int i = 0; i < count; i++) free(argv[i]);
                free(argv);
                free(cmd_copy);
                return NULL;
            }
            argv = new_argv;
        }
        argv[count++] = strdup(token);
        token = strtok_r(NULL, " \t", &saveptr);
    }
    argv[count] = NULL;

    free(cmd_copy);
    *argc = count;
    return argv;
}

bool lsp_init(const char *command) {
    if (!command) return false;
    if (lsp.running) {
        if (lsp.command && strcmp(lsp.command, command) == 0) {
            return true;
        }
        lsp_shutdown();
    }

    // Parse command into argv
    int argc;
    char **argv = parse_command(command, &argc);
    if (!argv || argc == 0) {
        return false;
    }

    // Create pipes for communication
    int stdin_pipe[2];
    int stdout_pipe[2];

    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
        for (int i = 0; i < argc; i++) free(argv[i]);
        free(argv);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        for (int i = 0; i < argc; i++) free(argv[i]);
        free(argv);
        return false;
    }

    if (pid == 0) {
        // Child process - exec LSP server
        close(stdin_pipe[1]);  // Close write end
        close(stdout_pipe[0]); // Close read end

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        // Redirect stderr to /dev/null to avoid pollution
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execvp(argv[0], argv);
        _exit(1);
    }

    // Parent: clean up argv
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);

    // Parent process
    close(stdin_pipe[0]);  // Close read end
    close(stdout_pipe[1]); // Close write end

    lsp.pid = pid;
    lsp.stdin_fd = stdin_pipe[1];
    lsp.stdout_fd = stdout_pipe[0];
    lsp.running = true;
    lsp.request_id = 0;
    lsp.command = strdup(command);

    // Set stdout_fd to non-blocking
    int flags = fcntl(lsp.stdout_fd, F_GETFL, 0);
    fcntl(lsp.stdout_fd, F_SETFL, flags | O_NONBLOCK);

    // Send initialize request
    JsonValue *params = json_object();
    json_object_set(params, "processId", json_number(getpid()));

    // Root URI (current directory)
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        char *root_uri = lsp_path_to_uri(cwd);
        if (root_uri) {
            json_object_set(params, "rootUri", json_string(root_uri));
            free(root_uri);
        }
    }

    // Capabilities
    JsonValue *capabilities = json_object();

    // Text document capabilities
    JsonValue *textDocCaps = json_object();
    JsonValue *syncCaps = json_object();
    json_object_set(syncCaps, "dynamicRegistration", json_bool(false));
    json_object_set(textDocCaps, "synchronization", syncCaps);

    JsonValue *diagCaps = json_object();
    json_object_set(diagCaps, "relatedInformation", json_bool(true));
    json_object_set(textDocCaps, "publishDiagnostics", diagCaps);

    JsonValue *hoverCaps = json_object();
    json_object_set(hoverCaps, "dynamicRegistration", json_bool(false));
    JsonValue *hoverFormats = json_array();
    json_array_push(hoverFormats, json_string("plaintext"));
    json_array_push(hoverFormats, json_string("markdown"));
    json_object_set(hoverCaps, "contentFormat", hoverFormats);
    json_object_set(textDocCaps, "hover", hoverCaps);

    JsonValue *completionCaps = json_object();
    json_object_set(completionCaps, "dynamicRegistration", json_bool(false));
    JsonValue *completionItem = json_object();
    JsonValue *completionFormats = json_array();
    json_array_push(completionFormats, json_string("plaintext"));
    json_array_push(completionFormats, json_string("markdown"));
    json_object_set(completionItem, "documentationFormat", completionFormats);
    json_object_set(completionCaps, "completionItem", completionItem);
    json_object_set(completionCaps, "contextSupport", json_bool(true));
    json_object_set(textDocCaps, "completion", completionCaps);

    // Semantic tokens capability
    JsonValue *semTokenCaps = json_object();
    json_object_set(semTokenCaps, "dynamicRegistration", json_bool(false));
    JsonValue *requests = json_object();
    json_object_set(requests, "full", json_bool(true));
    json_object_set(semTokenCaps, "requests", requests);
    JsonValue *tokenTypes = json_array();
    json_array_push(tokenTypes, json_string("variable"));
    json_array_push(tokenTypes, json_string("parameter"));
    json_array_push(tokenTypes, json_string("function"));
    json_array_push(tokenTypes, json_string("method"));
    json_array_push(tokenTypes, json_string("property"));
    json_array_push(tokenTypes, json_string("class"));
    json_array_push(tokenTypes, json_string("enum"));
    json_array_push(tokenTypes, json_string("enumMember"));
    json_array_push(tokenTypes, json_string("type"));
    json_array_push(tokenTypes, json_string("namespace"));
    json_array_push(tokenTypes, json_string("keyword"));
    json_array_push(tokenTypes, json_string("modifier"));
    json_array_push(tokenTypes, json_string("comment"));
    json_array_push(tokenTypes, json_string("string"));
    json_array_push(tokenTypes, json_string("number"));
    json_array_push(tokenTypes, json_string("operator"));
    json_array_push(tokenTypes, json_string("macro"));
    json_object_set(semTokenCaps, "tokenTypes", tokenTypes);
    JsonValue *tokenMods = json_array();
    json_object_set(semTokenCaps, "tokenModifiers", tokenMods);
    json_object_set(textDocCaps, "semanticTokens", semTokenCaps);

    json_object_set(capabilities, "textDocument", textDocCaps);
    json_object_set(params, "capabilities", capabilities);

    JsonValue *init_req = create_request("initialize", params);
    send_message(init_req);
    json_free(init_req);

    return true;
}

void lsp_shutdown(void) {
    if (!lsp.running) return;

    // Send shutdown request
    JsonValue *shutdown = create_request("shutdown", NULL);
    send_message(shutdown);
    json_free(shutdown);

    // Send exit notification
    JsonValue *exit_notif = create_notification("exit", NULL);
    send_message(exit_notif);
    json_free(exit_notif);

    // Close file descriptors
    close(lsp.stdin_fd);
    close(lsp.stdout_fd);

    // Wait for child process
    if (lsp.pid > 0) {
        int status;
        waitpid(lsp.pid, &status, WNOHANG);
        // If still running, kill it
        kill(lsp.pid, SIGTERM);
        waitpid(lsp.pid, &status, 0);
    }

    // Clean up buffer
    free(lsp.read_buf);

    // Clean up pending requests
    for (int i = 0; i < lsp.pending_count; i++) {
        free(lsp.pending[i].uri);
    }
    free(lsp.pending);

    // Clean up token types
    for (int i = 0; i < lsp.token_type_count; i++) {
        free(lsp.token_types[i]);
    }
    free(lsp.token_types);

    free(lsp.command);
    memset(&lsp, 0, sizeof(lsp));
}

bool lsp_is_running(void) {
    return lsp.running;
}

void lsp_did_open(const char *path, const char *content, const char *language_id) {
    if (!lsp.running || !path || !content) return;

    char *uri = lsp_path_to_uri(path);
    if (!uri) return;

    if (!language_id || language_id[0] == '\0') {
        language_id = "plaintext";
    }

    JsonValue *params = json_object();
    JsonValue *textDoc = json_object();

    json_object_set(textDoc, "uri", json_string(uri));
    json_object_set(textDoc, "languageId", json_string(language_id));
    json_object_set(textDoc, "version", json_number(1));
    json_object_set(textDoc, "text", json_string(content));

    json_object_set(params, "textDocument", textDoc);

    JsonValue *notif = create_notification("textDocument/didOpen", params);
    send_message(notif);
    json_free(notif);
    free(uri);

    // Mark as initialized after first didOpen
    if (!lsp.initialized) {
        // Send initialized notification
        JsonValue *init_notif = create_notification("initialized", json_object());
        send_message(init_notif);
        json_free(init_notif);
        lsp.initialized = true;
    }
}

void lsp_did_change(const char *path, const char *content, int version) {
    if (!lsp.running || !path || !content) return;

    char *uri = lsp_path_to_uri(path);
    if (!uri) return;

    JsonValue *params = json_object();
    JsonValue *textDoc = json_object();

    json_object_set(textDoc, "uri", json_string(uri));
    json_object_set(textDoc, "version", json_number(version));

    json_object_set(params, "textDocument", textDoc);

    // Full document sync - send entire content
    JsonValue *changes = json_array();
    JsonValue *change = json_object();
    json_object_set(change, "text", json_string(content));
    json_array_push(changes, change);

    json_object_set(params, "contentChanges", changes);

    JsonValue *notif = create_notification("textDocument/didChange", params);
    send_message(notif);
    json_free(notif);
    free(uri);
}

void lsp_did_close(const char *path) {
    if (!lsp.running || !path) return;

    char *uri = lsp_path_to_uri(path);
    if (!uri) return;

    JsonValue *params = json_object();
    JsonValue *textDoc = json_object();

    json_object_set(textDoc, "uri", json_string(uri));
    json_object_set(params, "textDocument", textDoc);

    JsonValue *notif = create_notification("textDocument/didClose", params);
    send_message(notif);
    json_free(notif);
    free(uri);
}

int lsp_get_fd(void) {
    return lsp.running ? lsp.stdout_fd : -1;
}

void lsp_process_incoming(void) {
    if (!lsp.running) return;

    // Read available data
    char tmp[4096];
    while (1) {
        ssize_t n = read(lsp.stdout_fd, tmp, sizeof(tmp));
        if (n <= 0) break;

        if (!buf_ensure_capacity(n)) return;
        memcpy(lsp.read_buf + lsp.read_buf_len, tmp, n);
        lsp.read_buf_len += n;
    }

    // Process complete messages
    while (lsp.read_buf_len > 0) {
        // Look for Content-Length header
        int header_pos = find_header_end(lsp.read_buf, lsp.read_buf_len);
        if (header_pos < 0) break;

        int header_len = header_pos + 4;

        // Parse Content-Length
        int content_len = 0;
        if (!parse_content_length(lsp.read_buf, header_pos, &content_len) || content_len <= 0) {
            // Invalid message, skip header
            memmove(lsp.read_buf, lsp.read_buf + header_len,
                    lsp.read_buf_len - header_len);
            lsp.read_buf_len -= header_len;
            continue;
        }

        // Check if we have the full message
        if (lsp.read_buf_len < header_len + content_len) break;

        // Extract and parse JSON content
        char *content = lsp.read_buf + header_len;
        char saved = content[content_len];
        content[content_len] = '\0';

        JsonValue *msg = json_parse(content);
        content[content_len] = saved;

        // Handle the message
        if (msg) {
            handle_message(msg);
            json_free(msg);
        }

        // Remove processed message from buffer
        int total_len = header_len + content_len;
        memmove(lsp.read_buf, lsp.read_buf + total_len,
                lsp.read_buf_len - total_len);
        lsp.read_buf_len -= total_len;
    }
}

void lsp_set_diagnostics_callback(lsp_diagnostics_callback cb) {
    lsp.diagnostics_cb = cb;
}

void lsp_set_semantic_tokens_callback(lsp_semantic_tokens_callback cb) {
    lsp.semantic_cb = cb;
}

void lsp_set_hover_callback(lsp_hover_callback cb) {
    lsp.hover_cb = cb;
}

void lsp_set_completion_callback(lsp_completion_callback cb) {
    lsp.completion_cb = cb;
}

void lsp_request_semantic_tokens(const char *path) {
    if (!lsp.running || !path) return;

    char *uri = lsp_path_to_uri(path);
    if (!uri) return;

    JsonValue *params = json_object();
    JsonValue *textDoc = json_object();

    json_object_set(textDoc, "uri", json_string(uri));
    json_object_set(params, "textDocument", textDoc);

    JsonValue *req = create_request("textDocument/semanticTokens/full", params);

    // Track this request so we can match the response
    add_pending_request(lsp.request_id, uri, REQ_SEMANTIC_TOKENS, -1, -1);

    send_message(req);
    json_free(req);
    free(uri);
}

void lsp_request_hover(const char *path, int line, int col) {
    if (!lsp.running || !path) return;

    char *uri = lsp_path_to_uri(path);
    if (!uri) return;

    JsonValue *params = json_object();
    JsonValue *textDoc = json_object();
    JsonValue *pos = json_object();

    json_object_set(textDoc, "uri", json_string(uri));
    json_object_set(pos, "line", json_number(line));
    json_object_set(pos, "character", json_number(col));
    json_object_set(params, "textDocument", textDoc);
    json_object_set(params, "position", pos);

    JsonValue *req = create_request("textDocument/hover", params);

    add_pending_request(lsp.request_id, uri, REQ_HOVER, line, col);

    send_message(req);
    json_free(req);
    free(uri);
}

void lsp_request_completion(const char *path, int line, int col, const char *trigger, int trigger_kind) {
    if (!lsp.running || !path) return;

    char *uri = lsp_path_to_uri(path);
    if (!uri) return;

    JsonValue *params = json_object();
    JsonValue *textDoc = json_object();
    JsonValue *pos = json_object();

    json_object_set(textDoc, "uri", json_string(uri));
    json_object_set(pos, "line", json_number(line));
    json_object_set(pos, "character", json_number(col));
    json_object_set(params, "textDocument", textDoc);
    json_object_set(params, "position", pos);

    JsonValue *context = json_object();
    if (trigger_kind <= 0) trigger_kind = 1;
    if (trigger && trigger[0] != '\0') {
        json_object_set(context, "triggerKind", json_number(trigger_kind));
        json_object_set(context, "triggerCharacter", json_string(trigger));
    } else {
        json_object_set(context, "triggerKind", json_number(trigger_kind));
    }
    json_object_set(params, "context", context);

    JsonValue *req = create_request("textDocument/completion", params);
    add_pending_request(lsp.request_id, uri, REQ_COMPLETION, line, col);
    send_message(req);
    json_free(req);
    free(uri);
}

bool lsp_hover_is_supported(void) {
    return lsp.hover_supported;
}

bool lsp_completion_is_supported(void) {
    return lsp.completion_supported;
}
