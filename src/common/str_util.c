#include "common/str_util.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

size_t wtsn_strlcpy(char *dst, const char *src, size_t size) {
    if (!dst || size == 0) return 0;
    size_t src_len = src ? strlen(src) : 0;
    if (src_len >= size) {
        memcpy(dst, src, size - 1);
        dst[size - 1] = '\0';
    } else if (src) {
        memcpy(dst, src, src_len + 1);
    } else {
        dst[0] = '\0';
    }
    return src_len;
}

void wtsn_str_trim(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\n' || s[len-1] == '\r')) {
        s[--len] = '\0';
    }
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

bool wtsn_str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

char *wtsn_str_dup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len + 1);
    return d;
}

int wtsn_str_valid_utf8(const char *s) {
    if (!s) return 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) {
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            p++; if ((*p & 0xC0) != 0x80) return 0; p++;
        } else if ((*p & 0xF0) == 0xE0) {
            p++; if ((*p & 0xC0) != 0x80) return 0;
            p++; if ((*p & 0xC0) != 0x80) return 0;
            p++;
        } else if ((*p & 0xF8) == 0xF0) {
            p++; if ((*p & 0xC0) != 0x80) return 0;
            p++; if ((*p & 0xC0) != 0x80) return 0;
            p++; if ((*p & 0xC0) != 0x80) return 0;
            p++;
        } else {
            return 0;
        }
    }
    return 1;
}
