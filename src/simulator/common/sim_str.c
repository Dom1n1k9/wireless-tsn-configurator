#include "simulator/common/sim_str.h"

#include <string.h>

size_t sim_strlcpy(char *dst, const char *src, size_t size) {
    if (!dst || size == 0) return 0;
    size_t n = src ? strlen(src) : 0;
    if (n >= size) {
        memcpy(dst, src, size - 1);
        dst[size - 1] = '\0';
    } else if (src) {
        memcpy(dst, src, n + 1);
    } else {
        dst[0] = '\0';
    }
    return n;
}

void sim_str_trim(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = '\0';
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

bool sim_str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int sim_strsplit(char *line, char sep, char *out[], int max, char *storage, size_t storage_size) {
    if (!line || !out) return 0;
    int count = 0;
    char *st = storage;
    size_t rem = storage_size;
    char *p = line;
    while (*p && count < max) {
        while (*p == sep) p++;
        if (!*p) break;
        out[count] = st;
        char *start = p;
        while (*p && *p != sep) p++;
        size_t len = (size_t)(p - start);
        if (len >= rem) len = rem - 1;
        memcpy(st, start, len);
        st[len] = '\0';
        st += len + 1;
        rem -= len + 1;
        count++;
        if (*p) p++;
    }
    return count;
}
