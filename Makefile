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

SOURCES = src/main.c src/terminal.c src/buffer.c src/clipboard.c src/json.c src/lsp.c src/lsp_config.c
OBJECTS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all clean install

all: $(TARGET)

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
	rm -rf build $(TARGET)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/
