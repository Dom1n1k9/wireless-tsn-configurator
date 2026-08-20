#include "simulator/protocol/sim_protocol.h"
#include "simulator/common/sim_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_PROFILES 32

int main(int argc, char **argv) {
    char *profiles[MAX_PROFILES];
    int profile_count = 0;

    const char *mqtt_host = "localhost";
    int mqtt_port = 1883;
    bool run_loop = true;

    static const char *all[] =
        { "profiles/esp32.ini", "profiles/rpi.ini", "profiles/stm32.ini",
          "profiles/nxp.ini", "profiles/linux.ini" };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            if (profile_count < MAX_PROFILES) profiles[profile_count++] = argv[++i];
        } else if (strcmp(argv[i], "--all") == 0) {
            for (int j = 0; j < 5 && profile_count < MAX_PROFILES; j++)
                profiles[profile_count++] = (char *)all[j];
        } else if (strcmp(argv[i], "--mqtt-host") == 0 && i + 1 < argc) {
            mqtt_host = argv[++i];
        } else if (strcmp(argv[i], "--mqtt-port") == 0 && i + 1 < argc) {
            mqtt_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--once") == 0) {
            run_loop = false;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: tsn-node-simulator [--profile <file>]... [--all]\n"
                   "       [--mqtt-host <h>] [--mqtt-port <p>] [--once]\n");
            return 0;
        }
    }

    if (profile_count == 0) {
        for (int j = 0; j < 5 && profile_count < MAX_PROFILES; j++)
            profiles[profile_count++] = (char *)all[j];
    }

    sim_log_init(SIM_LOG_INFO, NULL);
    sim_simulator *sim = sim_simulator_create();
    if (!sim) return EXIT_FAILURE;

    sim_discovery *disc = sim_discovery_create(sim);

    int added = 0;
    for (int i = 0; i < profile_count; i++) {
        const char *pf = profiles[i];
        int idx = sim_simulator_add_device(sim, pf);
        if (idx < 0) {
            sim_log(SIM_LOG_WARN, "failed to load profile %s", pf);
        } else {
            sim_log(SIM_LOG_INFO, "loaded %s", pf);
            added++;
        }
    }
    if (added == 0) {
        sim_log(SIM_LOG_ERROR, "no device profiles loaded");
        return EXIT_FAILURE;
    }

    sim_mqtt *mqtt = sim_mqtt_create(sim);
    if (mqtt) sim_mqtt_connect(mqtt, mqtt_host, mqtt_port);

    sim_discovery_start(disc);

    long tick = 0;
    do {
        sim_simulator_tick(sim, tick * 1000000LL);
        if (mqtt) sim_mqtt_publish_all(mqtt);
        if ((tick % 5) == 0) sim_discovery_announce(disc);
        tick++;
        usleep(100000);
    } while (run_loop);

    sim_discovery_destroy(disc);
    if (mqtt) sim_mqtt_destroy(mqtt);
    sim_simulator_destroy(sim);
    return 0;
}
