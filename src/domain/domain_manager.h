#ifndef WTSN_DOMAIN_MANAGER_H
#define WTSN_DOMAIN_MANAGER_H

#include "common/common.h"
#include "db/db.h"
#include "db/db_domains.h"
#include "mvc/event_bus.h"

#define WTSN_DOMAIN_MANAGER_MODEL "domain"

typedef struct wtsn_domain_manager wtsn_domain_manager;

wtsn_domain_manager *wtsn_domain_manager_create(wtsn_db *db, wtsn_event_bus *bus);
void wtsn_domain_manager_destroy(wtsn_domain_manager *m);
wtsn_error wtsn_domain_manager_save(wtsn_domain_manager *m, const wtsn_domain *d);
wtsn_error wtsn_domain_manager_delete(wtsn_domain_manager *m, const char *id);
void wtsn_domain_manager_for_each(wtsn_domain_manager *m, wtsn_db_domain_cb cb, void *ud);
wtsn_error wtsn_domain_manager_assign_device(wtsn_domain_manager *m, const char *device_id,
                                            const char *domain_id);

#endif
