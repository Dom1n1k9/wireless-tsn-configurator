#ifndef WTSN_COMMON_H
#define WTSN_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define WTSN_MAX_STR 256
#define WTSN_MAX_DEVICES 512
#define WTSN_MAX_TOPICS 128

typedef enum {
    WTSN_OK = 0,
    WTSN_ERR_INVALID_ARG,
    WTSN_ERR_NO_MEMORY,
    WTSN_ERR_NOT_FOUND,
    WTSN_ERR_ALREADY_EXISTS,
    WTSN_ERR_DB,
    WTSN_ERR_IO,
    WTSN_ERR_NET,
    WTSN_ERR_NOT_IMPLEMENTED,
    WTSN_ERR_BUSY,
    WTSN_ERR_NOT_READY,
    WTSN_ERR_LAST
} wtsn_error;

static inline const char *wtsn_error_str(wtsn_error e) {
    switch (e) {
    case WTSN_OK: return "ok";
    case WTSN_ERR_INVALID_ARG: return "invalid argument";
    case WTSN_ERR_NO_MEMORY: return "out of memory";
    case WTSN_ERR_NOT_FOUND: return "not found";
    case WTSN_ERR_ALREADY_EXISTS: return "already exists";
    case WTSN_ERR_DB: return "database error";
    case WTSN_ERR_IO: return "i/o error";
    case WTSN_ERR_NET: return "network error";
    case WTSN_ERR_NOT_IMPLEMENTED: return "not implemented";
    case WTSN_ERR_BUSY: return "busy";
    case WTSN_ERR_NOT_READY: return "not ready";
    default: return "unknown error";
    }
}

#endif
