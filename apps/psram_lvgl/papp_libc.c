/*
 * papp_libc.c — minimal freestanding C runtime for PSRAM apps.
 *
 * PAPPs link with -nostdlib -nodefaultlibs, so there is no libc. LVGL itself
 * is configured with LV_USE_STDLIB_* = LV_STDLIB_BUILTIN and therefore brings
 * its own lv_memcpy/lv_strlen/lv_snprintf. These symbols exist for a different
 * reason: GCC *implicitly* emits calls to memset/memcpy/memmove when it sees
 * struct initialisation, array copies or loops it recognises as such — even if
 * the source never names them. Without these definitions the link fails with
 * "undefined reference to `memset'".
 *
 * IMPORTANT: this file must be compiled with -fno-tree-loop-distribute-patterns,
 * otherwise GCC "optimises" the byte loop inside memset() into a call to
 * memset() — i.e. infinite recursion. The build script passes that flag.
 */

#include <stddef.h>
#include <stdint.h>

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') { }
    return dst;
}
