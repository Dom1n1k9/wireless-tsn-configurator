#include "simulator/common/sim_str.h"

#include <string.h>

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
