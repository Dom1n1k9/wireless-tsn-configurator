#ifndef WTSN_STR_UTIL_H
#define WTSN_STR_UTIL_H

#include "common/common.h"

#include <stddef.h>

size_t wtsn_strlcpy(char *dst, const char *src, size_t size);
int wtsn_str_valid_utf8(const char *s);
void wtsn_str_trim(char *s);
bool wtsn_str_starts_with(const char *s, const char *prefix);
char *wtsn_str_dup(const char *s);

#endif
