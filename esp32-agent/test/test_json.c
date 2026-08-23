#include "wtsn_json.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check_int(const char *json, const char *key, int expect, int present) {
    int v = -1;
    int ok = wtsn_json_get_int(json, key, &v);
    if (present) {
        if (!ok || v != expect) { printf("FAIL get_int(%s): got ok=%d v=%d (exp %d)\n", key, ok, v, expect); failures++; }
        else printf("ok   get_int(%s)=%d\n", key, v);
    } else {
        if (ok) { printf("FAIL get_int(%s) should be absent (got %d)\n", key, v); failures++; }
        else printf("ok   get_int(%s) absent\n", key);
    }
}

int main(void) {
    const char *snap = "{\"id\":\"esp32-01\",\"priority\":5,\"traffic_class\":5,\"preemption\":1,"
                       "\"vlan_id\":100,\"group\":\"Control\",\"timesync_mode\":2,"
                       "\"grandmaster\":\"esp32-01\",\"tas_cycle_ns\":1000000}";
    check_int(snap, "priority", 5, 1);
    check_int(snap, "traffic_class", 5, 1);
    check_int(snap, "preemption", 1, 1);
    check_int(snap, "vlan_id", 100, 1);
    check_int(snap, "timesync_mode", 2, 1);
    check_int(snap, "missing_key", 0, 0);
    check_int(snap, "priority", 5, 1); /* duplicate should still parse */

    int64_t l = 0;
    if (!wtsn_json_get_i64(snap, "tas_cycle_ns", &l)) { printf("FAIL i64\n"); failures++; }
    else if (l != 1000000LL) { printf("FAIL i64 got %lld\n", (long long)l); failures++; }
    else printf("ok   i64 tas_cycle_ns=%lld\n", (long long)l);

    char buf[32];
    if (!wtsn_json_get_str(snap, "group", buf, sizeof(buf))) { printf("FAIL str group\n"); failures++; }
    else if (strcmp(buf, "Control") != 0) { printf("FAIL str got %s\n", buf); failures++; }
    else printf("ok   str group=%s\n", buf);

    if (!wtsn_json_get_str(snap, "grandmaster", buf, sizeof(buf))) { printf("FAIL str gm\n"); failures++; }
    else printf("ok   str grandmaster=%s\n", buf);

    /* edge: empty JSON */
    int e = 0;
    if (wtsn_json_get_int("{}", "x", &e)) { printf("FAIL empty\n"); failures++; }
    else printf("ok   empty json handled\n");

    if (failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d FAILURES\n", failures);
    return 1;
}
