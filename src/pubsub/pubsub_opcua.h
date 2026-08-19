#ifndef WTSN_PUBSUB_OPCUA_H
#define WTSN_PUBSUB_OPCUA_H

#include "pubsub/pubsub.h"
#include <stdint.h>

wtsn_error wtsn_pubsub_opcua_backend(wtsn_pubsub_backend *out, void *server, uint16_t ns);
int wtsn_pubsub_opcua_available(void);

#endif
