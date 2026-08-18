#ifndef SIM_STR_H
#define SIM_STR_H

#include "simulator/common/sim_common.h"

#include <stddef.h>

size_t sim_strlcpy(char *dst, const char *src, size_t size);
void sim_str_trim(char *s);
bool sim_str_starts_with(const char *s, const char *prefix);
int sim_strsplit(char *line, char sep, char *out[], int max, char *storage, size_t storage_size);

#endif
