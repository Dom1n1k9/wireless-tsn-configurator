#ifndef SIM_PROFILE_H
#define SIM_PROFILE_H

#include "simulator/core/sim_device.h"

sim_error sim_profile_load(const char *path, sim_device *out);
sim_error sim_profile_default(sim_device *out);

#endif
