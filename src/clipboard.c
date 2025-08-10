#define _POSIX_C_SOURCE 200809L
#include "clipboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

char *clipboard_get(void) {
    FILE *fp;
    char *result = NULL;
    size_t size = 0;
    size_t capacity = 1024;
    
    if (access("/usr/bin/xclip", F_OK) == 0) {
        fp = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    } else if (access("/usr/bin/xsel", F_OK) == 0) {
        fp = popen("xsel --clipboard --output 2>/dev/null", "r");
    } else if (access("/usr/bin/pbpaste", F_OK) == 0) {
        fp = popen("pbpaste 2>/dev/null", "r");
    } else {
        return NULL;
    }
    
    if (!fp) return NULL;
    
    result = malloc(capacity);
    if (!result) {
        pclose(fp);
        return NULL;
    }
    
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), fp)) {
        size_t len = strlen(buffer);
        if (size + len >= capacity) {
            capacity *= 2;
            char *new_result = realloc(result, capacity);
            if (!new_result) {
                free(result);
                pclose(fp);
                return NULL;
            }
            result = new_result;
        }
        strcpy(result + size, buffer);
        size += len;
    }
    
    pclose(fp);
    
    if (size == 0) {
        free(result);
        return NULL;
    }
    
    result[size] = '\0';
    return result;
}

void clipboard_set(const char *text) {
    if (!text) return;
    
    FILE *fp;
    
    if (access("/usr/bin/xclip", F_OK) == 0) {
        fp = popen("xclip -selection clipboard 2>/dev/null", "w");
    } else if (access("/usr/bin/xsel", F_OK) == 0) {
        fp = popen("xsel --clipboard --input 2>/dev/null", "w");
    } else if (access("/usr/bin/pbcopy", F_OK) == 0) {
        fp = popen("pbcopy 2>/dev/null", "w");
    } else {
        return;
    }
    
    if (!fp) return;
    
    fprintf(fp, "%s", text);
    pclose(fp);
}