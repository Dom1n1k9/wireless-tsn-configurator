#ifndef SIM_SIMULATOR_H
#define SIM_SIMULATOR_H

#include "simulator/core/sim_device.h"
#include "simulator/profiles/sim_profile.h"

#include <stdint.h>

typedef struct sim_simulator sim_simulator;

sim_simulator *sim_simulator_create(void);
void sim_simulator_destroy(sim_simulator *s);

int sim_simulator_add_device(sim_simulator *s, const char *profile_path);
sim_device *sim_simulator_device(sim_simulator *s, int index);
int sim_simulator_device_count(sim_simulator *s);
sim_device *sim_simulator_find(sim_simulator *s, const char *id);

void sim_simulator_tick(sim_simulator *s, int64_t now_ns);

typedef void (*sim_dev_cb)(sim_device *d, void *ud);
void sim_simulator_for_each(sim_simulator *s, sim_dev_cb cb, void *ud);
void sim_simulator_mark_offline(sim_simulator *s, int64_t threshold_us);

#endif
