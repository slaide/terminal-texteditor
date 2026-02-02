#ifndef LSP_H
#define LSP_H

#include <stdbool.h>

// Diagnostic severity levels (LSP spec)
typedef enum {
    DIAG_ERROR = 1,
    DIAG_WARNING = 2,
    DIAG_INFO = 3,
    DIAG_HINT = 4
} DiagnosticSeverity;

// Single diagnostic entry
typedef struct {
    int line;           // 0-based line number
    int col;            // 0-based column number
    int end_line;       // End position
    int end_col;
    DiagnosticSeverity severity;
    char *message;
    char *source;       // e.g., "clang"
} Diagnostic;

// Diagnostics for a file
typedef struct {
    char *uri;
    Diagnostic *items;
    int count;
    int capacity;
} FileDiagnostics;

// Callback for when diagnostics are received
typedef void (*lsp_diagnostics_callback)(const char *uri, Diagnostic *diags, int count);

// Semantic token types (indices into legend from server)
typedef enum {
    TOKEN_VARIABLE = 0,
    TOKEN_PARAMETER,
    TOKEN_FUNCTION,
    TOKEN_METHOD,
    TOKEN_PROPERTY,
    TOKEN_CLASS,
    TOKEN_ENUM,
    TOKEN_ENUM_MEMBER,
    TOKEN_TYPE,
    TOKEN_NAMESPACE,
    TOKEN_KEYWORD,
    TOKEN_MODIFIER,
    TOKEN_COMMENT,
    TOKEN_STRING,
    TOKEN_NUMBER,
    TOKEN_OPERATOR,
    TOKEN_MACRO,
    TOKEN_UNKNOWN
} SemanticTokenType;

// Single semantic token
typedef struct {
    int line;       // 0-based
    int col;        // 0-based
    int length;
    SemanticTokenType type;
} SemanticToken;

// Callback for semantic tokens
typedef void (*lsp_semantic_tokens_callback)(const char *uri, SemanticToken *tokens, int count);

// Callback for hover info
typedef void (*lsp_hover_callback)(const char *uri, int line, int col, const char *text);

// Completion item
typedef struct {
    char *label;
    char *detail;
    char *documentation;
} LspCompletionItem;

// Callback for completion results
typedef void (*lsp_completion_callback)(const char *uri, int line, int col,
                                        LspCompletionItem *items, int count);

// Lifecycle
bool lsp_init(const char *command);  // Spawn LSP server with given command
void lsp_shutdown(void);
bool lsp_is_running(void);

// Document sync
void lsp_did_open(const char *path, const char *content, const char *language_id);
void lsp_did_change(const char *path, const char *content, int version);
void lsp_did_close(const char *path);

// Polling (call from event loop)
int lsp_get_fd(void);
void lsp_process_incoming(void);

// Diagnostics callback
void lsp_set_diagnostics_callback(lsp_diagnostics_callback cb);

// Semantic tokens
void lsp_set_semantic_tokens_callback(lsp_semantic_tokens_callback cb);
void lsp_request_semantic_tokens(const char *path);

// Hover
void lsp_set_hover_callback(lsp_hover_callback cb);
void lsp_request_hover(const char *path, int line, int col);
bool lsp_hover_is_supported(void);

// Completion
void lsp_set_completion_callback(lsp_completion_callback cb);
void lsp_request_completion(const char *path, int line, int col, const char *trigger, int trigger_kind);
bool lsp_completion_is_supported(void);

// Helper to convert file path to URI
char *lsp_path_to_uri(const char *path);
char *lsp_uri_to_path(const char *uri);

#endif
