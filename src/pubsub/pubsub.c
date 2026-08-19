#include "pubsub/pubsub.h"

#include "common/str_util.h"
#include <string.h>

void wtsn_pubsub_init(wtsn_pubsub *ps, const wtsn_pubsub_backend *backend, void *state) {
    if (!ps || !backend) return;
    memset(ps, 0, sizeof(*ps));
    ps->backend = *backend;
    ps->backend.state = state;
}

const char *wtsn_pubsub_name(wtsn_pubsub *ps) {
    if (!ps || !ps->backend.name) return "pubsub";
    return ps->backend.name(ps);
}

wtsn_error wtsn_pubsub_start(wtsn_pubsub *ps) {
    if (!ps || !ps->backend.start) return WTSN_ERR_NOT_IMPLEMENTED;
    return ps->backend.start(ps);
}

wtsn_error wtsn_pubsub_stop(wtsn_pubsub *ps) {
    if (!ps || !ps->backend.stop) return WTSN_ERR_NOT_IMPLEMENTED;
    return ps->backend.stop(ps);
}

wtsn_error wtsn_pubsub_publish(wtsn_pubsub *ps, const wtsn_pubsub_dataset *ds) {
    if (!ps || !ps->backend.publish) return WTSN_ERR_NOT_IMPLEMENTED;
    return ps->backend.publish(ps, ds);
}

int wtsn_pubsub_process(wtsn_pubsub *ps, int timeout_ms) {
    if (!ps || !ps->backend.process) return 0;
    return ps->backend.process(ps, timeout_ms);
}

void wtsn_pubsub_field_set_double(wtsn_pubsub_field *f, const char *name, double v) {
    if (!f || !name) return;
    wtsn_strlcpy(f->name, name, sizeof(f->name));
    f->type = WTSN_FIELD_DOUBLE;
    f->value.d = v;
}

void wtsn_pubsub_field_set_int32(wtsn_pubsub_field *f, const char *name, int32_t v) {
    if (!f || !name) return;
    wtsn_strlcpy(f->name, name, sizeof(f->name));
    f->type = WTSN_FIELD_INT32;
    f->value.i = v;
}

void wtsn_pubsub_field_set_uint16(wtsn_pubsub_field *f, const char *name, uint16_t v) {
    if (!f || !name) return;
    wtsn_strlcpy(f->name, name, sizeof(f->name));
    f->type = WTSN_FIELD_UINT16;
    f->value.u = v;
}

void wtsn_pubsub_field_set_bool(wtsn_pubsub_field *f, const char *name, bool v) {
    if (!f || !name) return;
    wtsn_strlcpy(f->name, name, sizeof(f->name));
    f->type = WTSN_FIELD_BOOL;
    f->value.b = v;
}
