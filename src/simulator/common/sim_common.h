#ifndef SIM_COMMON_H
#define SIM_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SIM_MAX_STR 256
#define SIM_MAX_SENSORS 32
#define SIM_MAX_TSN_FEATURES 16
#define SIM_MAX_GCL_ENTRIES 128
#define SIM_MAX_DEVICES 64

typedef enum {
    SIM_OK = 0,
    SIM_ERR_INVALID_ARG,
    SIM_ERR_NO_MEMORY,
    SIM_ERR_NOT_FOUND,
    SIM_ERR_ALREADY_EXISTS,
    SIM_ERR_IO,
    SIM_ERR_NET,
    SIM_ERR_CONFIG,
    SIM_ERR_NOT_IMPLEMENTED,
    SIM_ERR_BUSY
} sim_error;

static inline const char *sim_error_str(sim_error e) {
    switch (e) {
    case SIM_OK: return "ok";
    case SIM_ERR_INVALID_ARG: return "invalid argument";
    case SIM_ERR_NO_MEMORY: return "out of memory";
    case SIM_ERR_NOT_FOUND: return "not found";
    case SIM_ERR_ALREADY_EXISTS: return "already exists";
    case SIM_ERR_IO: return "i/o error";
    case SIM_ERR_NET: return "network error";
    case SIM_ERR_CONFIG: return "configuration error";
    case SIM_ERR_NOT_IMPLEMENTED: return "not implemented";
    case SIM_ERR_BUSY: return "busy";
    default: return "unknown error";
    }
}

#endif
