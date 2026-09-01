#ifndef WTSN_DB_TRACE_LOG_H
#define WTSN_DB_TRACE_LOG_H

#include "db/db.h"
#include "trace/trace.h"

wtsn_error wtsn_db_trace_log_insert(wtsn_db *db, const wtsn_trace_entry *e);
typedef void (*wtsn_db_trace_cb)(const wtsn_trace_entry *e, void *userdata);
void wtsn_db_trace_paged(wtsn_db *db, int offset, int limit, wtsn_db_trace_cb cb, void *userdata);
int wtsn_db_trace_count(wtsn_db *db);
wtsn_error wtsn_db_trace_prune(wtsn_db *db, int keep);

#endif
