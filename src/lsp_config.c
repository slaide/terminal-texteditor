#define _GNU_SOURCE
#include "lsp_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

static LspServerConfig *configs = NULL;
static int config_count = 0;
static int config_capacity = 0;

// Add a new config entry
static void add_config(const char *extensions_str, const char *command, const char *name) {
    if (config_count >= config_capacity) {
        int new_cap = config_capacity == 0 ? 8 : config_capacity * 2;
        LspServerConfig *new_configs = realloc(configs, new_cap * sizeof(LspServerConfig));
        if (!new_configs) return;
        configs = new_configs;
        config_capacity = new_cap;
    }

    LspServerConfig *cfg = &configs[config_count];
    memset(cfg, 0, sizeof(LspServerConfig));

    cfg->command = strdup(command);
    cfg->name = name ? strdup(name) : strdup(command);

    // Parse comma-separated extensions
    char *ext_copy = strdup(extensions_str);
    char *saveptr;
    char *token = strtok_r(ext_copy, ",", &saveptr);

    while (token) {
        // Skip whitespace
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';

        if (*token) {
            if (cfg->extension_count >= 16) break; // Max 16 extensions per server

            // Allocate extension array if needed
            if (cfg->extension_count == 0) {
                cfg->extensions = calloc(16, sizeof(char*));
            }

            // Ensure extension starts with '.'
            if (token[0] == '.') {
                cfg->extensions[cfg->extension_count++] = strdup(token);
            } else {
                char *with_dot = malloc(strlen(token) + 2);
                sprintf(with_dot, ".%s", token);
                cfg->extensions[cfg->extension_count++] = with_dot;
            }
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    free(ext_copy);

    config_count++;
}

// Parse a single line from config file
static void parse_config_line(char *line) {
    // Skip leading whitespace
    while (*line == ' ' || *line == '\t') line++;

    // Skip empty lines and comments
    if (*line == '\0' || *line == '\n' || *line == '#') return;

    // Remove trailing newline
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    // Format: extensions:command
    // Example: .c,.h,.cpp:clangd --log=error
    // Or with name: .c,.h:clangd --log=error:C/C++ (clangd)

    char *colon = strchr(line, ':');
    if (!colon) return;

    *colon = '\0';
    char *extensions = line;
    char *rest = colon + 1;

    // Check for optional name after second colon
    char *second_colon = strchr(rest, ':');
    char *command = rest;
    char *name = NULL;

    if (second_colon) {
        *second_colon = '\0';
        name = second_colon + 1;
        // Trim whitespace from name
        while (*name == ' ') name++;
    }

    // Trim whitespace from command
    while (*command == ' ') command++;
    char *cmd_end = command + strlen(command) - 1;
    while (cmd_end > command && *cmd_end == ' ') *cmd_end-- = '\0';

    if (*extensions && *command) {
        add_config(extensions, command, name);
    }
}

// Try to load config from a specific path
static bool load_from_path(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        parse_config_line(line);
    }

    fclose(f);
    return true;
}

// Get user's home directory
static const char *get_home_dir(void) {
    const char *home = getenv("HOME");
    if (home) return home;

    struct passwd *pw = getpwuid(getuid());
    if (pw) return pw->pw_dir;

    return NULL;
}

bool lsp_config_load(void) {
    // Free any existing config
    lsp_config_free();

    // Try loading from current directory first
    if (load_from_path("lsp.conf")) {
        return config_count > 0;
    }

    // Try XDG config directory
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    char path[1024];

    if (xdg_config) {
        snprintf(path, sizeof(path), "%s/texteditor/lsp.conf", xdg_config);
        if (load_from_path(path)) {
            return config_count > 0;
        }
    }

    // Try ~/.config/texteditor/lsp.conf
    const char *home = get_home_dir();
    if (home) {
        snprintf(path, sizeof(path), "%s/.config/texteditor/lsp.conf", home);
        if (load_from_path(path)) {
            return config_count > 0;
        }
    }

    // No config file found - add default clangd configuration
    add_config(".c,.h,.cpp,.cc,.cxx,.hpp,.hxx,.C", "clangd --log=error", "clangd");

    return config_count > 0;
}

void lsp_config_free(void) {
    for (int i = 0; i < config_count; i++) {
        for (int j = 0; j < configs[i].extension_count; j++) {
            free(configs[i].extensions[j]);
        }
        free(configs[i].extensions);
        free(configs[i].command);
        free(configs[i].name);
    }
    free(configs);
    configs = NULL;
    config_count = 0;
    config_capacity = 0;
}

const char *lsp_config_get_command(const char *extension) {
    if (!extension) return NULL;

    for (int i = 0; i < config_count; i++) {
        for (int j = 0; j < configs[i].extension_count; j++) {
            if (strcasecmp(configs[i].extensions[j], extension) == 0) {
                return configs[i].command;
            }
        }
    }
    return NULL;
}

bool lsp_config_has_server(const char *filename) {
    if (!filename) return false;

    const char *ext = strrchr(filename, '.');
    if (!ext) return false;

    return lsp_config_get_command(ext) != NULL;
}

LspServerConfig *lsp_config_get_all(int *count) {
    if (count) *count = config_count;
    return configs;
}
