#ifndef SIM_STR_H
#define SIM_STR_H

/* The simulator reuses the core string utilities (src/common/str_util.c).
 * sim_* names are kept as aliases so simulator sources stay unchanged. */
#include "common/str_util.h"

#define sim_strlcpy         wtsn_strlcpy
#define sim_str_trim        wtsn_str_trim
#define sim_str_starts_with wtsn_str_starts_with

/* No core counterpart; simulator-only helper. */
int sim_strsplit(char *line, char sep, char *out[], int max, char *storage, size_t storage_size);

#endif
