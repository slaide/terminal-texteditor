CC = gcc
CFLAGS_BASE = -std=c2x -Wall -Wextra
TARGET = texteditor

# Build mode: debug (default) or release
BUILD ?= debug

ifeq ($(BUILD),release)
    CFLAGS = $(CFLAGS_BASE) -O2
    BUILD_DIR = build/release
else
    CFLAGS = $(CFLAGS_BASE) -O0 -g
    BUILD_DIR = build/debug
endif

SOURCES = src/main.c src/editor_app.c src/editor_tabs.c src/editor_files.c src/editor_search.c src/editor_selection.c src/editor_cursor.c src/editor_folds.c src/editor_mouse.c src/editor_hover.c src/render.c src/file_manager.c src/terminal.c src/buffer.c src/clipboard.c src/json.c src/lsp.c src/editor_config.c src/lsp_integration.c
OBJECTS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all clean install

all: $(TARGET) md-lsp

# Copy executable from build dir to project root
$(TARGET): $(BUILD_DIR)/$(TARGET)
	cp $< $@

# Link object files into executable in build dir
$(BUILD_DIR)/$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

# Compile source files into object files in build dir
$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf build $(TARGET) md-lsp

install: $(TARGET) md-lsp
	cp $(TARGET) /usr/local/bin/
	cp md-lsp /usr/local/bin/

# Markdown LSP server
md-lsp: tools/md-lsp.c
	$(CC) $(CFLAGS) -o $@ $<
