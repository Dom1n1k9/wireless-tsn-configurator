#ifndef WTSN_DB_DOMAINS_H
#define WTSN_DB_DOMAINS_H

#include "db/db.h"

typedef struct {
    char id[WTSN_MAX_STR];
    char name[WTSN_MAX_STR];
    char description[WTSN_MAX_STR];
} wtsn_domain;

wtsn_error wtsn_db_domain_save(wtsn_db *db, const wtsn_domain *d);
wtsn_error wtsn_db_domain_delete(wtsn_db *db, const char *id);
typedef void (*wtsn_db_domain_cb)(const wtsn_domain *d, void *userdata);
void wtsn_db_domain_for_each(wtsn_db *db, wtsn_db_domain_cb cb, void *userdata);

#endif
