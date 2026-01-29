#define _GNU_SOURCE
#include "editor_config.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

static LanguageConfig *configs = NULL;
static int config_count = 0;
static int config_capacity = 0;

// Parse fold style string
static ConfigFoldStyle parse_fold_style(const char *str) {
    if (!str) return FOLD_STYLE_NONE;
    if (strcmp(str, "braces") == 0) return FOLD_STYLE_BRACES;
    if (strcmp(str, "indent") == 0) return FOLD_STYLE_INDENT;
    if (strcmp(str, "headings") == 0) return FOLD_STYLE_HEADINGS;
    return FOLD_STYLE_NONE;
}

// Add a language config
static void add_config(const char *name, JsonValue *lang_obj) {
    if (!lang_obj || lang_obj->type != JSON_OBJECT) return;

    // Get extensions array
    JsonValue *extensions = json_object_get(lang_obj, "extensions");
    if (!extensions || extensions->type != JSON_ARRAY) return;

    int ext_count = json_array_length(extensions);
    if (ext_count == 0) return;

    // Grow configs array if needed
    if (config_count >= config_capacity) {
        int new_cap = config_capacity == 0 ? 8 : config_capacity * 2;
        LanguageConfig *new_configs = realloc(configs, new_cap * sizeof(LanguageConfig));
        if (!new_configs) return;
        configs = new_configs;
        config_capacity = new_cap;
    }

    LanguageConfig *cfg = &configs[config_count];
    memset(cfg, 0, sizeof(LanguageConfig));

    cfg->name = strdup(name);

    // Parse extensions
    cfg->extensions = calloc(ext_count, sizeof(char*));
    cfg->extension_count = 0;

    for (int i = 0; i < ext_count; i++) {
        JsonValue *ext = json_array_get(extensions, i);
        const char *ext_str = json_get_string(ext);
        if (ext_str) {
            // Ensure extension starts with '.'
            if (ext_str[0] == '.') {
                cfg->extensions[cfg->extension_count++] = strdup(ext_str);
            } else {
                char *with_dot = malloc(strlen(ext_str) + 2);
                sprintf(with_dot, ".%s", ext_str);
                cfg->extensions[cfg->extension_count++] = with_dot;
            }
        }
    }

    // Get LSP command (optional)
    JsonValue *lsp = json_object_get(lang_obj, "lsp");
    const char *lsp_str = json_get_string(lsp);
    if (lsp_str) {
        cfg->lsp_command = strdup(lsp_str);
    }

    // Get fold style (optional)
    JsonValue *fold = json_object_get(lang_obj, "fold");
    const char *fold_str = json_get_string(fold);
    if (fold_str) {
        cfg->fold_style = parse_fold_style(fold_str);
    }

    config_count++;
}

// Read entire file into string
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = malloc(size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);

    return content;
}

// Try to load config from a specific path
static bool load_from_path(const char *path) {
    char *content = read_file(path);
    if (!content) return false;

    JsonValue *root = json_parse(content);
    free(content);

    if (!root || root->type != JSON_OBJECT) {
        if (root) json_free(root);
        return false;
    }

    // Get languages object
    JsonValue *languages = json_object_get(root, "languages");
    if (!languages || languages->type != JSON_OBJECT) {
        json_free(root);
        return false;
    }

    // Iterate over language entries
    for (int i = 0; i < languages->data.object.count; i++) {
        add_config(languages->data.object.pairs[i].key,
                   languages->data.object.pairs[i].value);
    }

    json_free(root);
    return config_count > 0;
}

// Get user's home directory
static const char *get_home_dir(void) {
    const char *home = getenv("HOME");
    if (home) return home;

    struct passwd *pw = getpwuid(getuid());
    if (pw) return pw->pw_dir;

    return NULL;
}

// Add default configurations
static void add_defaults(void) {
    // Create default JSON structure and parse it
    const char *default_json =
        "{"
        "  \"languages\": {"
        "    \"c\": {"
        "      \"extensions\": [\".c\", \".h\", \".cpp\", \".hpp\", \".cc\", \".cxx\"],"
        "      \"lsp\": \"clangd --log=error\","
        "      \"fold\": \"braces\""
        "    },"
        "    \"python\": {"
        "      \"extensions\": [\".py\", \".pyw\"],"
        "      \"lsp\": \"pylsp\","
        "      \"fold\": \"indent\""
        "    },"
        "    \"javascript\": {"
        "      \"extensions\": [\".js\", \".jsx\", \".ts\", \".tsx\"],"
        "      \"fold\": \"braces\""
        "    },"
        "    \"java\": {"
        "      \"extensions\": [\".java\"],"
        "      \"fold\": \"braces\""
        "    },"
        "    \"go\": {"
        "      \"extensions\": [\".go\"],"
        "      \"lsp\": \"gopls\","
        "      \"fold\": \"braces\""
        "    },"
        "    \"rust\": {"
        "      \"extensions\": [\".rs\"],"
        "      \"lsp\": \"rust-analyzer\","
        "      \"fold\": \"braces\""
        "    },"
        "    \"json\": {"
        "      \"extensions\": [\".json\"],"
        "      \"fold\": \"braces\""
        "    },"
        "    \"markdown\": {"
        "      \"extensions\": [\".md\", \".markdown\"],"
        "      \"lsp\": \"./md-lsp\","
        "      \"fold\": \"headings\""
        "    }"
        "  }"
        "}";

    JsonValue *root = json_parse(default_json);
    if (!root) return;

    JsonValue *languages = json_object_get(root, "languages");
    if (languages && languages->type == JSON_OBJECT) {
        for (int i = 0; i < languages->data.object.count; i++) {
            add_config(languages->data.object.pairs[i].key,
                       languages->data.object.pairs[i].value);
        }
    }

    json_free(root);
}

bool editor_config_load(void) {
    // Free any existing config
    editor_config_free();

    // Try loading from current directory first
    if (load_from_path("editor.json")) {
        return true;
    }

    // Try XDG config directory
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    char path[1024];

    if (xdg_config) {
        snprintf(path, sizeof(path), "%s/texteditor/editor.json", xdg_config);
        if (load_from_path(path)) {
            return true;
        }
    }

    // Try ~/.config/texteditor/editor.json
    const char *home = get_home_dir();
    if (home) {
        snprintf(path, sizeof(path), "%s/.config/texteditor/editor.json", home);
        if (load_from_path(path)) {
            return true;
        }
    }

    // No config file found - add defaults
    add_defaults();
    return config_count > 0;
}

void editor_config_free(void) {
    for (int i = 0; i < config_count; i++) {
        free(configs[i].name);
        for (int j = 0; j < configs[i].extension_count; j++) {
            free(configs[i].extensions[j]);
        }
        free(configs[i].extensions);
        free(configs[i].lsp_command);
    }
    free(configs);
    configs = NULL;
    config_count = 0;
    config_capacity = 0;
}

LanguageConfig *editor_config_get_for_extension(const char *extension) {
    if (!extension) return NULL;

    for (int i = 0; i < config_count; i++) {
        for (int j = 0; j < configs[i].extension_count; j++) {
            if (strcasecmp(configs[i].extensions[j], extension) == 0) {
                return &configs[i];
            }
        }
    }
    return NULL;
}

const char *editor_config_get_lsp_command(const char *filename) {
    if (!filename) return NULL;

    const char *ext = strrchr(filename, '.');
    if (!ext) return NULL;

    LanguageConfig *cfg = editor_config_get_for_extension(ext);
    return cfg ? cfg->lsp_command : NULL;
}

bool editor_config_has_lsp(const char *filename) {
    return editor_config_get_lsp_command(filename) != NULL;
}

ConfigFoldStyle editor_config_get_fold_style(const char *filename) {
    if (!filename) return FOLD_STYLE_NONE;

    const char *ext = strrchr(filename, '.');
    if (!ext) return FOLD_STYLE_NONE;

    LanguageConfig *cfg = editor_config_get_for_extension(ext);
    return cfg ? cfg->fold_style : FOLD_STYLE_NONE;
}

LanguageConfig *editor_config_get_all(int *count) {
    if (count) *count = config_count;
    return configs;
}
