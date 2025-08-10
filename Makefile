CC = gcc
CFLAGS = -std=c23 -Wall -Wextra -O2 -g
TARGET = texteditor
SOURCES = src/main.c src/terminal.c src/buffer.c src/clipboard.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/

.SUFFIXES: .c .o
