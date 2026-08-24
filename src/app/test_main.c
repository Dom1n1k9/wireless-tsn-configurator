#include "common/common.h"
#include "common/str_util.h"
#include "db/db.h"
#include "db/db_tsn.h"
#include "device/device_manager.h"
#include "qos/qos.h"
#include "sensors/sensor.h"
#include "stream/stream.h"
#include "tas/tas.h"
#include "timesync/timesync.h"
#include "vlan/vlan.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond) do { tests_run++; if (!(cond)) { tests_failed++; \
    fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_device(void) {
    wtsn_device d;
    memset(&d, 0, sizeof(d));
    wtsn_strlcpy(d.id, "esp-1", sizeof(d.id));
    wtsn_strlcpy(d.name, "hall sensor node", sizeof(d.name));
    wtsn_strlcpy(d.ip, "192.168.1.50", sizeof(d.ip));
    d.kind = WTSN_DEVICE_KIND_ESP32;
    d.status = WTSN_DEVICE_ONLINE;
    snprintf(d.firmware, sizeof(d.firmware), "1.4.2");
    wtsn_device_add_tsn_feature(&d, "802.1Qbv");
    wtsn_device_add_tsn_feature(&d, "802.1AS");

    CHECK(strcmp(wtsn_device_status_str(d.status), "online") == 0);
    CHECK(d.tsn_features_count == 2);
    CHECK(strcmp(d.tsn_features[0], "802.1Qbv") == 0);
}

static void test_qos_validation(void) {
    wtsn_qos_config_model q;
    memset(&q, 0, sizeof(q));
    wtsn_strlcpy(q.device_id, "esp-1", sizeof(q.device_id));
    q.priority = 5;
    q.traffic_class = WTSN_QOS_TC_CRITICAL;
    q.bandwidth_kbps = 1000;
    q.latency_ms = 10;

    CHECK(wtsn_qos_validate(&q) == WTSN_OK);

    wtsn_qos_config_model bad = q;
    bad.priority = 8;
    CHECK(wtsn_qos_validate(&bad) == WTSN_ERR_INVALID_ARG);
}

static void test_vlan(void) {
    wtsn_vlan_group_model g;
    memset(&g, 0, sizeof(g));
    wtsn_strlcpy(g.name, "zone-A", sizeof(g.name));
    g.vlan_id = 100;
    CHECK(wtsn_vlan_validate_group(&g) == WTSN_OK);
    wtsn_vlan_group_id(&g);
    CHECK(strcmp(g.id, "vlan-100") == 0);
}

static void test_gcl(void) {
    wtsn_gcl gcl;
    CHECK(wtsn_gcl_init(&gcl, 100000) == WTSN_OK);
    CHECK(wtsn_gcl_add_entry(&gcl, WTSN_GATE_OPEN, 40000) == WTSN_OK);
    CHECK(wtsn_gcl_add_entry(&gcl, 0, 60000) == WTSN_OK);
    CHECK(wtsn_gcl_is_valid(&gcl) == true);

    wtsn_gcl gcl2;
    CHECK(wtsn_gcl_init(&gcl2, 100000) == WTSN_OK);
    CHECK(wtsn_gcl_add_entry(&gcl2, WTSN_GATE_OPEN, 10000) == WTSN_OK);
    CHECK(wtsn_gcl_is_valid(&gcl2) == false);

    char buf[256];
    wtsn_gcl_render_ascii(&gcl, buf, sizeof(buf));
    CHECK(strlen(buf) > 0);
}

static void test_timesync(void) {
    CHECK(strcmp(wtsn_timesync_mode_str(WTSN_TIMESYNC_LOCAL_GRANDMASTER),
                "local_grandmaster") == 0);
    CHECK(wtsn_timesync_mode_parse("external_grandmaster") ==
          WTSN_TIMESYNC_EXTERNAL_GRANDMASTER);
}

static void test_sensor_type(void) {
    CHECK(wtsn_sensor_type_parse("imu") == WTSN_SENSOR_IMU);
    CHECK(strcmp(wtsn_sensor_type_str(WTSN_SENSOR_PRESSURE), "pressure") == 0);
}

static void test_db_roundtrip(void) {
    wtsn_db db;
    CHECK(wtsn_db_open(&db, "test_wtsn.db") == WTSN_OK);

    wtsn_device d;
    memset(&d, 0, sizeof(d));
    wtsn_strlcpy(d.id, "stm-1", sizeof(d.id));
    d.kind = WTSN_DEVICE_KIND_STM32;
    d.status = WTSN_DEVICE_ONLINE;
    wtsn_device_add_tsn_feature(&d, "802.1Qav");

    CHECK(wtsn_db_device_upsert(&db, &d) == WTSN_OK);

    wtsn_device loaded;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(wtsn_db_device_get(&db, "stm-1", &loaded) == WTSN_OK);
    CHECK(loaded.kind == WTSN_DEVICE_KIND_STM32);
    CHECK(loaded.tsn_features_count == 1);
    CHECK(strcmp(loaded.tsn_features[0], "802.1Qav") == 0);

    wtsn_db_close(&db);
    remove("test_wtsn.db");
}

static void test_stream_validate(void) {
    wtsn_stream s;
    memset(&s, 0, sizeof(s));
    wtsn_strlcpy(s.stream_id, "stream-1", sizeof(s.stream_id));
    wtsn_strlcpy(s.name, "Control", sizeof(s.name));
    wtsn_strlcpy(s.talker, "esp32-01", sizeof(s.talker));
    s.vlan_id = 100;
    s.max_latency_ns = 1000000;
    s.max_interval_ns = 100000;
    s.priority = 5;
    s.data_frame_prio = 5;
    wtsn_strlcpy(s.listeners[0], "rpi-1", sizeof(s.listeners[0]));
    s.listener_count = 1;

    CHECK(wtsn_stream_validate(&s) == WTSN_OK);

    wtsn_stream bad = s;
    bad.priority = 8;
    CHECK(wtsn_stream_validate(&bad) == WTSN_ERR_INVALID_ARG);

    wtsn_stream nono = s;
    nono.listener_count = 0;
    nono.listener_all = 0;
    CHECK(wtsn_stream_validate(&nono) == WTSN_ERR_INVALID_ARG);

    CHECK(strcmp(wtsn_stream_status_str(WTSN_STREAM_READY), "ready") == 0);
    CHECK(wtsn_stream_status_parse("failed") == WTSN_STREAM_FAILED);
    CHECK(strcmp(wtsn_stream_role_str(WTSN_STREAM_ROLE_LISTENER), "listener") == 0);
}

static void test_stream_db_roundtrip(void) {
    wtsn_db db;
    CHECK(wtsn_db_open(&db, "test_stream.db") == WTSN_OK);

    wtsn_stream s;
    memset(&s, 0, sizeof(s));
    wtsn_strlcpy(s.stream_id, "s1", sizeof(s.stream_id));
    wtsn_strlcpy(s.name, "Control", sizeof(s.name));
    wtsn_strlcpy(s.talker, "esp32-01", sizeof(s.talker));
    s.vlan_id = 100;
    s.max_latency_ns = 1000000;
    s.max_interval_ns = 100000;
    s.priority = 5;
    s.data_frame_prio = 5;
    s.status = WTSN_STREAM_CONFIGURED;
    wtsn_strlcpy(s.listeners[0], "rpi-1", sizeof(s.listeners[0]));
    s.listener_count = 1;

    CHECK(wtsn_db_tsn_save(&db, &s) == WTSN_OK);

    wtsn_stream loaded;
    memset(&loaded, 0, sizeof(loaded));
    CHECK(wtsn_db_tsn_load(&db, "s1", &loaded) == WTSN_OK);
    CHECK(strcmp(loaded.talker, "esp32-01") == 0);
    CHECK(loaded.listener_count == 1);
    CHECK(strcmp(loaded.listeners[0], "rpi-1") == 0);
    CHECK(loaded.priority == 5);

    wtsn_db_tsn_set_status(&db, "s1", WTSN_STREAM_READY);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(wtsn_db_tsn_load(&db, "s1", &loaded) == WTSN_OK);
    CHECK(loaded.status == WTSN_STREAM_READY);

    CHECK(wtsn_db_tsn_delete(&db, "s1") == WTSN_OK);
    CHECK(wtsn_db_tsn_load(&db, "s1", &loaded) == WTSN_ERR_NOT_FOUND);

    wtsn_db_close(&db);
    remove("test_stream.db");
}

int main(void) {
    test_device();
    test_qos_validation();
    test_vlan();
    test_gcl();
    test_timesync();
    test_sensor_type();
    test_db_roundtrip();
    test_stream_validate();
    test_stream_db_roundtrip();

    printf("%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
