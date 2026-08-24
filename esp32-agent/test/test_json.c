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

    /* GCL array parsing */
    const char *gcl = "[{\"gate_state\":1,\"duration_ns\":300000},"
                      "{\"gate_state\":3,\"duration_ns\":200000},"
                      "{\"gate_state\":0,\"duration_ns\":500000}]";
    int gates[32];
    int64_t durs[32];
    int n = wtsn_json_parse_gcl(gcl, gates, durs, 32);
    if (n != 3) { printf("FAIL gcl count %d\n", n); failures++; }
    else if (gates[0] != 1 || durs[0] != 300000LL) { printf("FAIL gcl[0]\n"); failures++; }
    else if (gates[1] != 3 || durs[1] != 200000LL) { printf("FAIL gcl[1]\n"); failures++; }
    else if (gates[2] != 0 || durs[2] != 500000LL) { printf("FAIL gcl[2]\n"); failures++; }
    else printf("ok   gcl parse %d entries\n", n);

    const char *snap2 = "{\"id\":\"x\",\"tas_cycle_ns\":1000000,\"gcl\":[{\"gate_state\":1,\"duration_ns\":400000},{\"gate_state\":0,\"duration_ns\":600000}]}";
    const char *arr2 = NULL;
    if (!wtsn_json_get_root_array(snap2, "gcl", &arr2)) { printf("FAIL array locate\n"); failures++; }
    else {
        int g2[4]; int64_t d2[4];
        int n2 = wtsn_json_parse_gcl(arr2, g2, d2, 4);
        if (n2 != 2) { printf("FAIL gcl2 count %d\n", n2); failures++; }
        else if (g2[0] != 1 || d2[0] != 400000LL || g2[1] != 0 || d2[1] != 600000LL)
        { printf("FAIL gcl2 values\n"); failures++; }
        else printf("ok   array locate + parse\n");
    }

    if (failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d FAILURES\n", failures);
    return 1;
}
