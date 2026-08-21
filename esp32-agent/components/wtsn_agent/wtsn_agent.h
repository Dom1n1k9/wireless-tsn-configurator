#ifndef WTSN_AGENT_H
#define WTSN_AGENT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Command topic parser: tsn/cmd/<device_id>/<command> -> device_id + cmd */
bool wtsn_parse_cmd(const char *topic, char *device_id, size_t device_id_sz,
                    char *cmd, size_t cmd_sz);

#endif

