#include "agent/agent.h"
#include "common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *device_id = "node-01";
    const char *platform = "linux";
    const char *mqtt_host = "localhost";
    int mqtt_port = 1883;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) device_id = argv[++i];
        else if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) platform = argv[++i];
        else if (strcmp(argv[i], "--mqtt-host") == 0 && i + 1 < argc) mqtt_host = argv[++i];
        else if (strcmp(argv[i], "--mqtt-port") == 0 && i + 1 < argc) mqtt_port = atoi(argv[++i]);
    }

    wtsn_log_init(WTSN_LOG_INFO, NULL);

    wtsn_agent *a = wtsn_agent_create(device_id, platform, mqtt_host, mqtt_port);
    if (!a) {
        fprintf(stderr, "agent create failed\n");
        return EXIT_FAILURE;
    }
    wtsn_error e = wtsn_agent_start(a);
    if (e != WTSN_OK && e != WTSN_ERR_NOT_IMPLEMENTED) {
        wtsn_log(WTSN_LOG_ERROR, "agent start failed: %s", wtsn_error_str(e));
    }

    for (;;) sleep(1);
    wtsn_agent_destroy(a);
    return 0;
}
