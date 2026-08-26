#include "wtsn_json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Skip whitespace */
static const char *skipws(const char *s) { while (s && *s && (*s==' '||*s=='\t'||*s=='\n'||*s=='\r')) s++; return s; }

/* iterate "key": value at root */
static const char *probe_value(const char *json, const char *key, int *cmp) {
    const char *p = json;
    size_t klen = strlen(key);
    while (p && *p) {
        p = skipws(p);
        if (*p != '"') { p++; continue; }
        const char *ks = p + 1;
        const char *ke = strchr(ks, '"');
        if (!ke) break;
        size_t n = (size_t)(ke - ks);
        p = ke + 1;
        p = skipws(p);
        if (*p != ':') { p++; continue; }
        const char *val = skipws(p + 1);
        if (n == klen && memcmp(ks, key, klen) == 0) { *cmp = 1; return val; }
        *cmp = 0;
        /* skip this value to next entry */
        if (*val == '"') { const char *qe = strchr(val + 1, '"'); p = qe ? qe + 1 : val + 1; }
        else if (*val == '{') { int d=1; p=val+1; while (p && *p && d){ if(*p=='{')d++; else if(*p=='}')d--; p++;} }
        else if (*val == '[') { int d=1; p=val+1; while (p && *p && d){ if(*p=='[')d++; else if(*p==']')d--; p++;} }
        else { const char *e = val; while (*e && *e!=',' && *e!='}' && *e!=' ') e++; p = e; }
    }
    return NULL;
}

bool wtsn_json_get_int(const char *json, const char *key, int *out) {
    int cmp = 0;
    const char *v = probe_value(json, key, &cmp);
    if (!v || !cmp) return false;
    if (*v == '"') return false;
    char tmp[32]; int i = 0;
    if (*v == '-') tmp[i++] = *v++;
    while (*v >= '0' && *v <= '9') { if (i < 31) tmp[i++] = *v; v++; }
    tmp[i] = '\0';
    *out = atoi(tmp);
    return i > 0;
}

bool wtsn_json_get_i64(const char *json, const char *key, int64_t *out) {
    int cmp = 0;
    const char *v = probe_value(json, key, &cmp);
    if (!v || !cmp) return false;
    if (*v == '"') return false;
    char tmp[32]; int i = 0;
    if (*v == '-') tmp[i++] = *v++;
    while (*v >= '0' && *v <= '9') { if (i < 31) tmp[i++] = *v; v++; }
    tmp[i] = '\0';
    *out = atoll(tmp);
    return i > 0;
}

bool wtsn_json_get_str(const char *json, const char *key, char *out, size_t sz) {
    int cmp = 0;
    const char *v = probe_value(json, key, &cmp);
    if (!v || !cmp || *v != '"') return false;
    const char *s = v + 1;
    const char *e = strchr(s, '"');
    if (!e) return false;
    size_t n = (size_t)(e - s);
    if (n >= sz) n = sz - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

/* Locate a top-level array value and return pointer to its '[' (or NULL).
   Only sets *out if the value is an array (starts with '['). */
bool wtsn_json_get_root_array(const char *json, const char *key, const char **out) {
    int cmp = 0;
    const char *p = (const char *)probe_value(json, key, &cmp);
    if (!p || !cmp || *p != '[') return false;
    if (out) *out = p;
    return true;
}

/* Parse a JSON array of plain strings ["a","b","c"] into out[][]. Returns count. */
int wtsn_json_parse_str_array(const char *arr, char out[][64], int max_entries) {
    if (!arr || !out || max_entries <= 0) return 0;
    const char *p = arr + 1; /* skip '[' */
    int n = 0;
    for (; p && *p && *p != ']' && n < max_entries; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (!*p || *p == ']') break;
        if (*p == '"') {
            const char *s = p + 1;
            const char *e = strchr(s, '"');
            if (!e) break;
            size_t len = (size_t)(e - s);
            if (len >= 64) len = 63;
            memcpy(out[n], s, len);
            out[n][len] = '\0';
            n++;
            p = e + 1;
        } else {
            break;
        }
    }
    return n;
}

/* Parse a JSON array of {"gate_state":N,"duration_ns":M} objects into
   parallel arrays. Returns number of entries parsed (max max_entries). */
int wtsn_json_parse_gcl(const char *arr, int *gates, int64_t *durations, int max_entries) {
    if (!arr || !gates || !durations || max_entries <= 0) return 0;
    const char *p = arr + 1; /* skip '[' */
    int n = 0;
    for (; p && *p && *p != ']' && n < max_entries; ) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',' || *p == '{') p++;
        if (!*p || *p == ']') break;
        int gs = 0;
        if (wtsn_json_get_int(p, "gate_state", &gs)) gates[n] = gs;
        int64_t dur = 0;
        if (wtsn_json_get_i64(p, "duration_ns", &dur)) durations[n] = dur;
        n++;
        /* advance past this object: find matching '}' */
        p = strchr(p, '}');
        if (p) p++;
    }
    return n;
}
