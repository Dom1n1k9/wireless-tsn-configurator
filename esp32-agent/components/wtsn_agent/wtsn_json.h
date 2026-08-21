#ifndef WTSN_JSON_H
#define WTSN_JSON_H

#include <stdbool.h>
#include <stdint.h>

/* minimal single-pass JSON object getters (no allocation).
   Given a JSON object string, find top-level key and return int or copy string.
   Fields must be of the form  "key": value  at depth 0 (object root). */
bool wtsn_json_get_int(const char *json, const char *key, int *out);
bool wtsn_json_get_i64(const char *json, const char *key, int64_t *out);
bool wtsn_json_get_str(const char *json, const char *key, char *out, size_t sz);

#endif
