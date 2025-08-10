#ifndef CLIPBOARD_H
#define CLIPBOARD_H

char *clipboard_get(void);
void clipboard_set(const char *text);

#endif