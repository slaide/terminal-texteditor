#ifndef EDITOR_CONFIG_H
#define EDITOR_CONFIG_H

#include <stdbool.h>

// Fold styles
typedef enum {
    FOLD_STYLE_NONE,
    FOLD_STYLE_BRACES,      // C/C++/Java/JS - fold on { }
    FOLD_STYLE_INDENT,      // Python - fold on indentation
    FOLD_STYLE_HEADINGS,    // Markdown - fold on headings
} ConfigFoldStyle;

// Language configuration
typedef struct {
    char *name;             // Language name (e.g., "c", "python")
    char **extensions;      // Array of extensions (e.g., ".c", ".h")
    int extension_count;
    char *lsp_command;      // LSP command or NULL if none
    ConfigFoldStyle fold_style;
} LanguageConfig;

// Load editor configuration from JSON file
// Searches: ./editor.json, ~/.config/texteditor/editor.json
bool editor_config_load(void);

// Free all loaded configurations
void editor_config_free(void);

// Find language config for a given file extension
LanguageConfig *editor_config_get_for_extension(const char *extension);

// Get LSP command for a file (convenience function)
const char *editor_config_get_lsp_command(const char *filename);

// Check if LSP is configured for a file
bool editor_config_has_lsp(const char *filename);

// Get fold style for a file
ConfigFoldStyle editor_config_get_fold_style(const char *filename);

// Get all loaded configs
LanguageConfig *editor_config_get_all(int *count);

#endif
