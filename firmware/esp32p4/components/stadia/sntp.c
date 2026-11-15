/*
 * SNTP for time.google.com (port of helpers/sntp_server.py): answer each client request with
 * our clock. The dongle's clock is only as good as port_wall_time(), which the controller
 * doesn't hold against us: it checks no certificate dates.
 */
#include "stadia_internal.h"
#include "stadia_port.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

static const char *TAG = "sntp";
#define NTP_UNIX_OFFSET 2208988800ULL

static void put_ts(uint8_t *p, double t)
{
    uint64_t secs = (uint64_t)t + NTP_UNIX_OFFSET;
    uint32_t frac = (uint32_t)((t - (uint64_t)t) * 4294967296.0);
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(secs >> (24 - 8 * i));
        p[4 + i] = (uint8_t)(frac >> (24 - 8 * i));
    }
}

static void sntp_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    uint8_t buf[68];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 48 || (buf[0] & 7) != 3) { /* mode 3 = client */
            continue;
        }
        double now = port_wall_time();
        uint8_t out[48] = {0};
        out[0] = (uint8_t)((buf[0] & 0x38) | 4); /* LI 0, the client's version, mode 4 = server */
        out[1] = 1;                               /* stratum 1 */
        out[2] = buf[2];
        out[3] = (uint8_t)-20;
        memcpy(out + 12, "LOCL", 4);
        put_ts(out + 16, now);        /* reference */
        memcpy(out + 24, buf + 40, 8); /* originate = the client's transmit */
        put_ts(out + 32, now);        /* receive */
        put_ts(out + 40, now);        /* transmit */
        sendto(sock, out, sizeof(out), 0, (struct sockaddr *)&from, flen);
    }
}

bool sntp_start(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SLOGE(TAG, "cannot bind UDP %u", port);
        return false;
    }
    SLOGI(TAG, "serving time on UDP %u", port);
    return port_spawn(sntp_task, (void *)(intptr_t)sock, "sntp", 3072);
}
