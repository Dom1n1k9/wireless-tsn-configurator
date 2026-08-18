#ifndef WTSN_TAS_MANAGER_H
#define WTSN_TAS_MANAGER_H

#include "common/common.h"
#include "db/db.h"
#include "db/db_tas.h"
#include "mvc/event_bus.h"
#include "tas/tas.h"

#define WTSN_TAS_MANAGER_MODEL "tas"

typedef struct wtsn_tas_manager wtsn_tas_manager;

wtsn_tas_manager *wtsn_tas_manager_create(wtsn_db *db, wtsn_event_bus *bus);
void wtsn_tas_manager_destroy(wtsn_tas_manager *m);

wtsn_error wtsn_tas_manager_save(wtsn_tas_manager *m, const wtsn_tas_schedule_model *s);
wtsn_error wtsn_tas_manager_load(wtsn_tas_manager *m, const char *id, wtsn_tas_schedule_model *out);
wtsn_error wtsn_tas_manager_delete(wtsn_tas_manager *m, const char *id);
wtsn_error wtsn_tas_manager_deploy(wtsn_tas_manager *m, const char *id);

#endif
