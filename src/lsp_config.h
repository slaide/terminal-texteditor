#ifndef LSP_CONFIG_H
#define LSP_CONFIG_H

#include <stdbool.h>

// Single LSP server configuration
typedef struct {
    char **extensions;      // Array of extensions (e.g., ".c", ".h")
    int extension_count;
    char *command;          // Command to run (e.g., "clangd --log=error")
    char *name;             // Display name (e.g., "clangd")
} LspServerConfig;

// Load LSP configurations from config file
// Searches: ./lsp.conf, ~/.config/texteditor/lsp.conf
bool lsp_config_load(void);

// Free all loaded configurations
void lsp_config_free(void);

// Find LSP server command for a given file extension
// Returns NULL if no LSP configured for this extension
const char *lsp_config_get_command(const char *extension);

// Check if any LSP is configured for a file
bool lsp_config_has_server(const char *filename);

// Get all loaded configs (for debugging/display)
LspServerConfig *lsp_config_get_all(int *count);

#endif
