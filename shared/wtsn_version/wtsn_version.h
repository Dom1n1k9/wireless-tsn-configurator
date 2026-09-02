#ifndef WTSN_VERSION_H
#define WTSN_VERSION_H

/* Single source of truth for the firmware version, reported by both
 * esp32-agent and esp32-cam in their tsn/discover and tsn/status payloads
 * ("fw" field). Bump on every firmware release; keep in sync with CHANGELOG.md. */
#define WTSN_FW_VERSION "1.1.0"

#endif
