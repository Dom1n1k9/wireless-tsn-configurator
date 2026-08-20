#include "app/app.h"
#include "common/log.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    wtsn_app_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.db_path, sizeof(cfg.db_path), "wtsn.db");
    snprintf(cfg.mqtt_host, sizeof(cfg.mqtt_host), "localhost");
    cfg.mqtt_port = 1883;
    cfg.headless = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc)
            snprintf(cfg.db_path, sizeof(cfg.db_path), "%s", argv[++i]);
        else if (strcmp(argv[i], "--mqtt-host") == 0 && i + 1 < argc)
            snprintf(cfg.mqtt_host, sizeof(cfg.mqtt_host), "%s", argv[++i]);
        else if (strcmp(argv[i], "--mqtt-port") == 0 && i + 1 < argc)
            cfg.mqtt_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--plugin-dir") == 0 && i + 1 < argc)
            snprintf(cfg.plugin_dir, sizeof(cfg.plugin_dir), "%s", argv[++i]);
    }

    wtsn_app app;
    if (wtsn_app_init(&app, &cfg) != WTSN_OK) {
        fprintf(stderr, "application init failed\n");
        return EXIT_FAILURE;
    }

    int rc = wtsn_ui_run(&app);
    wtsn_app_shutdown(&app);
    return rc;
}
