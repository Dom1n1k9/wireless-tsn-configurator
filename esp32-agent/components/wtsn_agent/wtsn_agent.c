#include "wtsn_agent.h"
#include "wtsn_cfg.h"

static const char *TAG = "wtsn";

bool wtsn_parse_cmd(const char *topic, char *device_id, size_t device_id_sz,
                    char *cmd, size_t cmd_sz) {
    const char prefix[] = "tsn/cmd/";
    if (!topic || strncmp(topic, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *rest = topic + (sizeof(prefix) - 1);
    const char *slash = strchr(rest, '/');
    if (!slash) return false;
    size_t idlen = (size_t)(slash - rest);
    if (idlen >= device_id_sz) return false;
    memcpy(device_id, rest, idlen);
    device_id[idlen] = '\0';
    wtsn_strlcpy(cmd, slash + 1, cmd_sz);
    return true;
}

