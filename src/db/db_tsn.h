#ifndef WTSN_DB_TSN_H
#define WTSN_DB_TSN_H

#include "db/db.h"
#include "stream/stream.h"

/* load / save a single TSN stream (and its listener members) */
wtsn_error wtsn_db_tsn_save(wtsn_db *db, const wtsn_stream *s);
wtsn_error wtsn_db_tsn_load(wtsn_db *db, const char *stream_id, wtsn_stream *out);
wtsn_error wtsn_db_tsn_delete(wtsn_db *db, const char *stream_id);
wtsn_error wtsn_db_tsn_set_status(wtsn_db *db, const char *stream_id, wtsn_stream_status st);
void wtsn_db_tsn_for_each(wtsn_db *db, int (*cb)(const wtsn_stream *s, void *ud), void *ud);

#endif
