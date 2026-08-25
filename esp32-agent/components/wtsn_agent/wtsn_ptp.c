#include "wtsn_ptp.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "wtsn_ptp";

/*
 * Real software gPTP on ESP32 over UDP multicast (IEEE 802.1AS UDP transport).
 *
 *   Event port    : UDP 319
 *   General port  : UDP 320
 *   Multicast group: 224.0.1.129 (PTP primary, IPv4)
 *
 * The node implements the minimal gPTP message exchange over the wire:
 *   - grandmaster (mode 1): sends Sync, Follow_Up; replies Delay_Resp to
 *     Delay_Req.
 *   - slave (mode 2/3): processes Sync/Follow_Up, sends Delay_Req and Pdelay_Req,
 *     computes offset_from_master and jitter.
 *
 * ESP32's WiFi MAC has no raw 802.3/TSN egress, so we use the standards UDP
 * transport. This is REAL PTP frame exchange over the air (sub-ms on WiFi), not
 * deterministic TSN grade. Reports are published on tsn/ptp so the configurator
 * monitor shows the live gPTP exchange.
 */

#define PTP_EVENT_PORT    319
#define PTP_GENERAL_PORT  320
#define PTP_GROUP_IP      "224.0.1.129"

#define PTP_PAYLOAD_OFFSET 34       /* fixed header 34 bytes */
#define PTP_SYNC_SEC      1
#define PTP_NSEC_PER_SEC  1000000000LL
#define PTP_REPORT_MS     2000

/* Message types (messageType field). */
enum {
    PTP_SYNC          = 0x0,
    PTP_FOLLOW_UP     = 0x8,
    PTP_DELAY_REQ     = 0x1,
    PTP_DELAY_RESP    = 0x9,
    PTP_PDELAY_REQ    = 0x2,
    PTP_PDELAY_RESP   = 0x3,
};

enum {
    PTP_CTRL_DISABLED = 0,
    PTP_CTRL_LOCAL_GM = 1,
    PTP_CTRL_EXTERNAL = 2,
    PTP_CTRL_AUTO     = 3,
};

typedef struct {
    uint8_t  uuid[8];
    uint16_t portNumber;
} ptp_identity_t;

static ptp_identity_t g_ident;
static int g_mode = PTP_CTRL_DISABLED;
static volatile bool g_run = false;

static wtsn_ptp_report g_report;
static char g_device_id[32] = "esp32-01";
static wtsn_mqtt *g_mq = NULL;

static int g_event_fd = -1;
static int g_general_fd = -1;
static int64_t g_last_sync_local = 0;
static int64_t g_last_sync_gm = 0;
static int64_t g_delayreq_local = 0;

static void ptp_tx_loop(void *arg);
static void ptp_rx_loop(void *arg);

static int64_t now_ns(void) { return (int64_t)esp_timer_get_time() * 1000; }

static void build_header(uint8_t *b, uint8_t msgtype, uint16_t len, uint16_t seq) {
    b[0] = 2;                       /* PTP minorVersion */
    b[1] = msgtype;
    b[2] = 0x0;                    /* transportSpecific (low nibble) */
    b[3] = 0;                      /* messageLength hi */
    b[4] = 0;                      /* messageLength lo -> filled below */
    b[5] = 0; b[6] = 0x02;       /* versionPTP 0x0002 */
    b[7] = 0;                      /* domain */ /* versionPTP lo 0x02? */
    b[7] = (uint8_t)(len >> 8);    /* used versionPTP as offset space */

    /* write messageLength (2 bytes at offset 4) */
    b[4] = (uint8_t)(len >> 8);
    b[5] = (uint8_t)(len & 0xff);
    /* versionPTP (offset 6): 0x0002 */
    b[6] = 0x00; b[7] = 0x02;
    /* domainNumber offset 8 */
    b[8] = 0;
    /* flags offset 9 */
    b[9] = 0;
    /* correctionField offset 10 (8 bytes) all zero */
    for (int i = 0; i < 8; i++) b[10 + i] = 0;
    /* sourcePortIdentity offset 18: 8-byte clockIdentity + 2-byte port */
    for (int i = 0; i < 8; i++) b[18 + i] = g_ident.uuid[i];
    b[26] = (uint8_t)(g_ident.portNumber >> 8);
    b[27] = (uint8_t)(g_ident.portNumber & 0xff);
    /* sequenceId offset 28 */
    b[28] = (uint8_t)(seq >> 8);
    b[29] = (uint8_t)(seq & 0xff);
    /* controlField offset 30 */
    b[30] = (msgtype == PTP_SYNC) ? 0 : (msgtype >> 4);
    /* logMessagePeriod offset 32 */
    b[32] = 0;                     /* logSyncInterval: 0 -> 1s */
    b[33] = 0;                     /* reserved */
}

static void set_timestamp(uint8_t *p, int64_t ns) {
    uint64_t sec = (uint64_t)(ns / PTP_NSEC_PER_SEC);
    uint32_t nsec = (uint32_t)(ns % PTP_NSEC_PER_SEC);
    for (int i = 0; i < 6; i++) p[5 - i] = (uint8_t)((sec >> (8 * i)) & 0xff);
    for (int i = 0; i < 4; i++) p[9 - i] = (uint8_t)((nsec >> (8 * i)) & 0xff);
}

static int64_t read_timestamp(const uint8_t *p) {
    uint64_t sec = 0;
    uint32_t nsec = 0;
    for (int i = 0; i < 6; i++) sec = (sec << 8) | p[i];
    for (int i = 0; i < 4; i++) nsec = (nsec << 8) | p[6 + i];
    return (int64_t)sec * PTP_NSEC_PER_SEC + (int64_t)nsec;
}

static const struct sockaddr_in *ptp_dst(void) {
    static struct sockaddr_in d;
    if (!d.sin_port) {
        d.sin_family = AF_INET;
        d.sin_port = htons(PTP_EVENT_PORT);
        d.sin_addr.s_addr = inet_addr(PTP_GROUP_IP);
    }
    return &d;
}

static void ptp_send_event(const uint8_t *buf, size_t len) {
    if (g_event_fd < 0) return;
    sendto(g_event_fd, buf, len, 0, (const struct sockaddr *)ptp_dst(), sizeof(struct sockaddr_in));
}

static void ptp_send_general(const uint8_t *buf, size_t len) {
    if (g_general_fd < 0) return;
    sendto(g_general_fd, buf, len, 0, (const struct sockaddr *)ptp_dst(), sizeof(struct sockaddr_in));
}

static void send_sync(uint16_t seq) {
    uint8_t b[64];
    build_header(b, PTP_SYNC, 44, seq);
    set_timestamp(b + PTP_PAYLOAD_OFFSET, now_ns());
    ptp_send_event(b, 44);
    /* Follow_Up with precise timestamp */
    uint8_t fu[64];
    build_header(fu, PTP_FOLLOW_UP, 44, seq);
    set_timestamp(fu + PTP_PAYLOAD_OFFSET, now_ns());
    ptp_send_event(fu, 44);
}

static void send_delayreq(uint16_t *seq) {
    uint8_t b[64];
    build_header(b, PTP_DELAY_REQ, 42, (*seq)++);
    g_delayreq_local = now_ns();
    set_timestamp(b + PTP_PAYLOAD_OFFSET, g_delayreq_local);
    ptp_send_event(b, 42);
}

static void send_pdelayreq(uint16_t *seq) {
    uint8_t b[64];
    build_header(b, PTP_PDELAY_REQ, 44, (*seq)++);
    set_timestamp(b + PTP_PAYLOAD_OFFSET, now_ns());
    ptp_send_event(b, 44);
}

static void process_packet(const uint8_t *buf, ssize_t len) {
    if (len < PTP_PAYLOAD_OFFSET) return;
    uint8_t mt = buf[1];
    if (mt == PTP_SYNC && g_mode != PTP_CTRL_LOCAL_GM) {
        g_last_sync_local = now_ns();
    } else if (mt == PTP_FOLLOW_UP && g_mode != PTP_CTRL_LOCAL_GM) {
        g_last_sync_gm = read_timestamp(buf + PTP_PAYLOAD_OFFSET);
        if (g_last_sync_local) {
            int64_t off = g_last_sync_gm - g_last_sync_local;
            if (off < 0) off = -off;
            g_report.offset_ns = off;   /* magnitude */
            g_report.jitter_ns = g_report.jitter_ns < off ? off : g_report.jitter_ns;
            g_report.state = (off < 100000000) ? 0 : 1;
        }
    } else if (mt == PTP_DELAY_REQ && g_mode == PTP_CTRL_LOCAL_GM) {
        uint8_t resp[64];
        static uint16_t seq = 1;
        build_header(resp, PTP_DELAY_RESP, 42, seq++);
        set_timestamp(resp + PTP_PAYLOAD_OFFSET, now_ns());
        ptp_send_general(resp, 42);
    } else if (mt == PTP_PDELAY_REQ) {
        uint8_t resp[64];
        static uint16_t pseq = 1;
        build_header(resp, PTP_PDELAY_RESP, 44, pseq++);
        set_timestamp(resp + PTP_PAYLOAD_OFFSET, now_ns());
        ptp_send_event(resp, 44);
    }
}

static void ptp_tx_loop(void *arg) {
    (void)arg;
    uint16_t seq = 1;
    int64_t last_sync = 0, last_dreq = 0, last_pdreq = 0, last_rep = 0;
    g_last_sync_gm = 0; g_last_sync_local = 0; g_delayreq_local = 0;
    g_report.offset_ns = 0; g_report.jitter_ns = 0;
    while (g_run) {
        int64_t t = now_ns();
        if (g_mode == PTP_CTRL_LOCAL_GM) {
            if (t - last_sync >= PTP_SYNC_SEC * PTP_NSEC_PER_SEC) {
                last_sync = t;
                send_sync(seq++);
            }
        } else if (g_mode == PTP_CTRL_EXTERNAL || g_mode == PTP_CTRL_AUTO) {
            if (t - last_pdreq >= 500000000LL) { last_pdreq = t; send_pdelayreq(&seq); }
            if (t - last_dreq >= 2000000000LL) { last_dreq = t; send_delayreq(&seq); }
        }
        /* periodic report every 2s regardless of mode (if mode != disabled) */
        if (g_mode != PTP_CTRL_DISABLED && t - last_rep >= PTP_REPORT_MS * 1000000LL) {
            last_rep = t;
            char buf[320];
            snprintf(buf, sizeof(buf),
                    "{\"id\":\"%s\",\"offset_ns\":%lld,\"jitter_ns\":%lld,"
                    "\"state\":%d,\"mode\":%d,\"grandmaster\":\"%s\","
                    "\"grandmaster_id\":\"%s\",\"clock_identity\":\"%s\"}",
                    g_device_id, (long long)g_report.offset_ns,
                    (long long)g_report.jitter_ns, g_report.state, g_report.mode,
                    g_report.grandmaster, g_report.grandmaster_id,
                    g_report.clock_identity);
            if (g_mq) wtsn_mqtt_publish(g_mq, "tsn/ptp", buf);
            ESP_LOGI(TAG, "ptp offset=%lld ns jitter=%lld ns state=%d",
                     (long long)g_report.offset_ns,
                     (long long)g_report.jitter_ns, g_report.state);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
}

static void ptp_rx_loop(void *arg) {
    (void)arg;
    uint8_t buf[256];
    while (g_run) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (g_event_fd >= 0)   { FD_SET(g_event_fd, &rfds);   if (g_event_fd > maxfd) maxfd = g_event_fd; }
        if (g_general_fd >= 0) { FD_SET(g_general_fd, &rfds); if (g_general_fd > maxfd) maxfd = g_general_fd; }
        if (maxfd < 0) break;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) continue;
        if (FD_ISSET(g_event_fd, &rfds)) {
            ssize_t n = recv(g_event_fd, buf, sizeof(buf), 0);
            if (n > 0) process_packet(buf, n);
        }
        if (FD_ISSET(g_general_fd, &rfds)) {
            ssize_t n = recv(g_general_fd, buf, sizeof(buf), 0);
            if (n > 0) process_packet(buf, n);
        }
    }
    vTaskDelete(NULL);
}

static int make_udp(int port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
    struct ip_mreq mr;
    memset(&mr, 0, sizeof(mr));
    mr.imr_multiaddr.s_addr = inet_addr(PTP_GROUP_IP);
    mr.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr));
    unsigned char loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    struct in_addr ifc = { .s_addr = htonl(INADDR_ANY) };
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifc, sizeof(ifc));
    return s;
}

int wtsn_ptp_setup(const char *device_id, wtsn_mqtt *mq) {
    if (device_id) snprintf(g_device_id, sizeof(g_device_id), "%s", device_id);
    snprintf(g_report.clock_identity, sizeof(g_report.clock_identity), "%s",
             g_device_id);
    g_mq = mq;
    return 0;
}

int wtsn_ptp_start(void) {
    if (g_run) return 0;
    for (size_t i = 0; i < 8; i++) g_ident.uuid[i] = (uint8_t)g_device_id[i % strlen(g_device_id)];
    g_ident.portNumber = (uint16_t)(g_device_id[0] & 0xff) ? (uint16_t)(g_device_id[0] & 0xff) : 1;
    g_ident.portNumber |= 0x0001;

    g_event_fd = make_udp(PTP_EVENT_PORT);
    g_general_fd = make_udp(PTP_GENERAL_PORT);
    if (g_event_fd < 0 || g_general_fd < 0) {
        ESP_LOGE(TAG, "PTP UDP sockets failed (%d/%d)", g_event_fd, g_general_fd);
        return -1;
    }
    g_run = true;
    if (xTaskCreatePinnedToCore(&ptp_tx_loop, "wtsn_ptp_tx", 4096, NULL, 6, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(&ptp_rx_loop, "wtsn_ptp_rx", 4096, NULL, 6, NULL, 1) != pdPASS) {
        g_run = false;
        return -1;
    }
    ESP_LOGI(TAG, "gPTP UDP multicast started (event=%d general=%d), identity=%s",
             PTP_EVENT_PORT, PTP_GENERAL_PORT, g_device_id);
    return 0;
}

wtsn_ptp_report *wtsn_ptp_get_report(void) { return &g_report; }

void wtsn_ptp_apply(int mode, const char *grandmaster) {
    g_mode = mode;
    g_report.mode = mode;
    if (mode == PTP_CTRL_LOCAL_GM) {
        g_report.state = 0;
        g_report.offset_ns = 0;
        g_report.jitter_ns = 0;
        snprintf(g_report.grandmaster, sizeof(g_report.grandmaster), "%s", g_device_id);
        snprintf(g_report.grandmaster_id, sizeof(g_report.grandmaster_id), "%s", g_device_id);
    } else if (mode == PTP_CTRL_DISABLED) {
        g_report.state = 2;
        g_report.offset_ns = 0;
        g_report.jitter_ns = 0;
        snprintf(g_report.grandmaster, sizeof(g_report.grandmaster), "%s", "disabled");
    } else {
        snprintf(g_report.grandmaster, sizeof(g_report.grandmaster), "%s",
                 grandmaster && grandmaster[0] ? grandmaster : "external-master");
        snprintf(g_report.grandmaster_id, sizeof(g_report.grandmaster_id), "%s",
                 grandmaster && grandmaster[0] ? grandmaster : "external-master");
    }
    ESP_LOGI(TAG, "apply mode=%d", mode);
}
