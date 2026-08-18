#include "simulator/core/sim_simulator.h"

#include "simulator/common/sim_log.h"

#include <stdlib.h>
#include <string.h>

struct sim_simulator {
    sim_device devices[SIM_MAX_DEVICES];
    int count;
};

sim_simulator *sim_simulator_create(void) {
    return calloc(1, sizeof(sim_simulator));
}

void sim_simulator_destroy(sim_simulator *s) {
    free(s);
}

int sim_simulator_add_device(sim_simulator *s, const char *profile_path) {
    if (!s || s->count >= SIM_MAX_DEVICES) return -1;
    sim_device d;
    if (sim_profile_load(profile_path, &d) != SIM_OK) return -1;
    /* prevent duplicate id */
    if (sim_simulator_find(s, d.id)) return -1;
    s->devices[s->count++] = d;
    return s->count - 1;
}

sim_device *sim_simulator_device(sim_simulator *s, int index) {
    if (!s || index < 0 || index >= s->count) return NULL;
    return &s->devices[index];
}

int sim_simulator_device_count(sim_simulator *s) {
    return s ? s->count : 0;
}

sim_device *sim_simulator_find(sim_simulator *s, const char *id) {
    if (!s || !id) return NULL;
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->devices[i].id, id) == 0) return &s->devices[i];
    }
    return NULL;
}

static void tick_sensors(sim_device *d) {
    for (int i = 0; i < d->sensor_count; i++) {
        sim_sensor *sn = &d->sensors[i];
        double range = (sn->max - sn->min);
        if (range > 0) {
            double delta = ((double)rand() / RAND_MAX - 0.5) * sn->step * 2;
            sn->value += delta;
            if (sn->value < sn->min) sn->value = sn->min;
            if (sn->value > sn->max) sn->value = sn->max;
        }
    }
}

static void tick_gcl(sim_device *d, int64_t now_ns) {
    if (!d->services_tas || d->gcl_count == 0 || d->cycle_time_ns <= 0) return;
    int64_t t = now_ns % d->cycle_time_ns;
    int64_t acc = 0;
    for (int i = 0; i < d->gcl_count; i++) {
        acc += d->gcl[i].duration_ns;
        if (t < acc) {
            d->gcl_state = (d->gcl[i].gate_state == 0x01) ? 1 : 0;
            return;
        }
    }
    d->gcl_state = 0;
}

void sim_simulator_tick(sim_simulator *s, int64_t now_ns) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) {
        sim_device *d = &s->devices[i];
        if (d->status != SIM_DEVICE_ONLINE) continue;
        tick_sensors(d);
        tick_gcl(d, now_ns);
    }
}

void sim_simulator_for_each(sim_simulator *s, sim_dev_cb cb, void *ud) {
    if (!s || !cb) return;
    for (int i = 0; i < s->count; i++) cb(&s->devices[i], ud);
}

void sim_simulator_mark_offline(sim_simulator *s, int64_t threshold_us) {
    (void)s;
    (void)threshold_us;
    /* future: liveness tracking */
}
