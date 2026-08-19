#include "agent/platform/agent_platform.h"

#include <string.h>

agent_platform agt_platform_from_string(const char *s) {
    if (!s) return AGENT_PLATFORM_LINUX;
    if (strcmp(s, "esp32") == 0) return AGENT_PLATFORM_ESP32;
    if (strcmp(s, "raspberry_pi") == 0 || strcmp(s, "rpi") == 0) return AGENT_PLATFORM_RASPBERRY_PI;
    if (strcmp(s, "stm32") == 0) return AGENT_PLATFORM_STM32;
    if (strcmp(s, "nxp") == 0) return AGENT_PLATFORM_NXP;
    if (strcmp(s, "linux") == 0) return AGENT_PLATFORM_LINUX;
    return AGENT_PLATFORM_LINUX;
}

void agt_platform_default_ops(agent_platform_ops *ops, agent_platform p) {
    if (!ops) return;
    memset(ops, 0, sizeof(*ops));
    (void)p;
    /* all callbacks default to not-implemented; the Linux adapter provides
       real behaviour, embedded adapters provide compiled-in logging. */
    ops->init = NULL;
    ops->destroy = NULL;
    ops->name = NULL;
    (void)ops;
}
