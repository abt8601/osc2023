#ifndef OSCOS_LIBC_STRING_H
#define OSCOS_LIBC_STRING_H

#include <stddef.h>

int memcmp(const void *s1, const void *s2, size_t n);
void *memset(void *s, int c, size_t n);
void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memmove(void *dest, const void *src, size_t n);

int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);

size_t strlen(const char *s);

#endif