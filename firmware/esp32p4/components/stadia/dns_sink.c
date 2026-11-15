/*
 * A DNS server that points every name at the board. Stadiacast's dns_sink forwards names it
 * does not redirect to the real resolver; the dongle has no internet, so it answers all of them
 * itself (connectivitycheck.gstatic.com and clients3.google.com included, which step 2 serves
 * over HTTPS with a 204 and "{}").
 */
#include "stadia_internal.h"
#include "stadia_port.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

static const char *TAG = "dns";
static struct in_addr s_ip;
#define TYPE_A 1
#define TYPE_AAAA 28

/* Copy the dotted name at q into out; return the offset just past it, or -1. */
static int read_name(const uint8_t *msg, int len, int off, char *out, size_t out_len)
{
    size_t n = 0;
    while (off < len) {
        uint8_t l = msg[off++];
        if (l == 0) {
            out[n] = '\0';
            return off;
        }
        if (l & 0xC0 || off + l > len || n + l + 2 > out_len) {
            return -1; /* questions are never compressed; refuse anything odd */
        }
        if (n) {
            out[n++] = '.';
        }
        memcpy(out + n, msg + off, l);
        n += l;
        off += l;
    }
    return -1;
}

static void dns_task(void *arg)
{
    int sock = (int)(intptr_t)arg;

    uint8_t buf[512];
    char name[256];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len < 12) {
            continue;
        }
        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if ((buf[2] & 0x80) || qdcount != 1) {
            continue; /* not a query, or more than one question */
        }
        int end = read_name(buf, len, 12, name, sizeof(name));
        if (end < 0 || end + 4 > len) {
            continue;
        }
        uint16_t qtype = (buf[end] << 8) | buf[end + 1];
        int qend = end + 4;

        /* Reply: same id and question; QR=1, AA=1, RD copied, RA=1, RCODE=0. */
        buf[2] = 0x84 | (buf[2] & 0x01);
        buf[3] = 0x80;
        buf[6] = buf[7] = 0;                       /* ANCOUNT */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;   /* NSCOUNT, ARCOUNT */
        int out = qend;

        if (qtype == TYPE_A && out + 16 <= (int)sizeof(buf)) {
            const uint8_t rr[] = {
                0xC0, 0x0C,             /* name: pointer to the question */
                0x00, TYPE_A, 0x00, 0x01,
                0x00, 0x00, 0x00, 0x3C, /* TTL 60 s */
                0x00, 0x04,
            };
            memcpy(buf + out, rr, sizeof(rr));
            out += sizeof(rr);
            memcpy(buf + out, &s_ip.s_addr, 4); /* already network order */
            out += 4;
            buf[7] = 1;
        }
        sendto(sock, buf, out, 0, (struct sockaddr *)&from, from_len);
        char from_ip[16];
        inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));
        SLOGI(TAG, "%s %s from %s -> %s", qtype == TYPE_A ? "A" : qtype == TYPE_AAAA ? "AAAA" : "?",
                 name, from_ip, qtype == TYPE_A ? "board" : "empty");
    }
}

bool dns_sink_start(uint16_t port, const char *answer_ip)
{
    if (inet_pton(AF_INET, answer_ip, &s_ip) != 1) {
        return false;
    }
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SLOGE(TAG, "cannot bind UDP %u", port);
        return false;
    }
    SLOGI(TAG, "answering every name with %s on UDP %u", answer_ip, port);
    return port_spawn(dns_task, (void *)(intptr_t)sock, "dns_sink", 4096);
}
