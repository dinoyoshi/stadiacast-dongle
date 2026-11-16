/*
 * One WebRTC link to the controller on its own UDP socket: ICE (we are the controlled agent),
 * DTLS as server with use_srtp, and WireHeader2 records on top (port of rtc/endpoint.py and
 * rtc/wire.py). The display link and the game session each have one; rtc.c runs both from
 * one task, so nothing here needs a lock except the remote description.
 */
#pragma once

#include "srtp.h"
#include "stadia_internal.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbedtls/ssl.h"

#define WIRE_HEADER_LEN 8
#define WIRE_ID_RTP 0x66
#define WIRE_ID_RPC_CONTROLLER 0x74
#define WIRE_ID_RPC_DISPLAY 0x75 /* ours on the display link: the live preset's rpc_wire_id */
#define WIRE_ID_RPC 0xFFFA       /* the game session's own RPC channel */

typedef struct rtc_link rtc_link_t;

typedef struct {
    void (*on_connected)(rtc_link_t *L);
    void (*on_closed)(rtc_link_t *L);
    /* One WireHeader2 record (id, payload). */
    void (*on_wire)(rtc_link_t *L, uint16_t id, const uint8_t *p, size_t n);
    /* A DTLS record that is not WireHeader2 (the game session's "unframed" input). */
    void (*on_unframed)(rtc_link_t *L, const uint8_t *p, size_t n);
    /* One RTP packet the controller sent as SRTP on the selected pair, decrypted and checked
     * (its microphone). */
    void (*on_media)(rtc_link_t *L, const uint8_t *rtp, size_t n);
} rtc_link_ops_t;

struct rtc_link {
    const char *name;
    int slot; /* the player whose link this is */
    const rtc_link_ops_t *ops;
    int sock;
    uint16_t port;
    char ufrag[9];
    char pwd[25];
    /* remote description, set by rtc_link_set_remote() from another task */
    bool remote_new;
    bool close_new; /* rtc_link_close() asked for */
    remote_offer_t pending;
    remote_offer_t remote;
    /* ICE */
    bool have_remote;
    struct sockaddr_in cand[4];
    int ncand;
    int check_attempts[4];
    int64_t next_check[4];
    bool selected;
    struct sockaddr_in peer;
    bool verified_out;
    int64_t fast_until, next_keepalive;
    uint8_t tiebreaker[8];
    /* DTLS */
    bool dtls_on, dtls_up;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    const uint8_t *in_dgram;
    size_t in_len;
    int64_t timer_int, timer_fin, timer_start;
    uint8_t master[48], randbytes[64];
    mbedtls_tls_prf_types prf;
    uint8_t srtp_keys[60]; /* client key 16, server key 16, client salt 14, server salt 14 */
    bool srtp_ready;
    srtp_tx_t srtp_out; /* our media (the headset audio): the DTLS server's write key and salt */
    srtp_rx_t srtp_in;  /* the controller's media (its microphone): the client's write key and salt */
    uint32_t in_ssrc;   /* the controller's media stream we decrypt (0: none yet) */
    uint32_t media_in, rtcp_in, srtp_fail, stray_logged;
    int64_t dtls_at;
    int64_t last_rx; /* the last datagram from the selected peer */
    /* link quality: the round trip of our keepalive checks once DTLS is up (written by the
     * link's task, read as a snapshot by stadia_get_stats) */
    uint8_t ping_txid[12];
    int64_t ping_sent_us; /* 0: no check outstanding */
    uint32_t pings_sent, pings_answered;
    int32_t ping_us; /* the latest round trip, -1 before any on this session */
    /* our RTP stream (input acks, rumble) */
    uint16_t tx_seq;
    uint32_t ssrc;
};

/* Shared once per boot: the DTLS certificate and key of rtc_identity(). */
bool rtc_link_global_init(void);

bool rtc_link_init(rtc_link_t *L, const char *name, int slot, uint16_t port, const rtc_link_ops_t *ops);
/* Our ICE credentials for the next session (the display link keeps rtc_identity()'s). */
void rtc_link_set_credentials(rtc_link_t *L, const char *ufrag, const char *pwd);
/* A new offer: reset the link and start ICE towards its candidates (thread-safe). */
void rtc_link_set_remote(rtc_link_t *L, const remote_offer_t *remote);
/* Close the link and forget its remote description (thread-safe; done by the next poll or tick). */
void rtc_link_close(rtc_link_t *L);
/* Called by the task: apply a pending remote description, then one datagram (or none). */
void rtc_link_poll(rtc_link_t *L, const uint8_t *buf, size_t n, const struct sockaddr_in *from);
void rtc_link_tick(rtc_link_t *L);

bool rtc_link_up(const rtc_link_t *L);
bool rtc_link_send_wire(rtc_link_t *L, uint16_t id, const uint8_t *payload, size_t len);
/* `payload` as one RTP packet (our SSRC and sequence) inside WireHeader2 0x66. */
bool rtc_link_send_rtp_wire(rtc_link_t *L, uint8_t pt, const uint8_t *payload, size_t len);
/* `payload` as a bare DTLS record. */
bool rtc_link_send_record(rtc_link_t *L, const uint8_t *payload, size_t len);
/* A complete RTP packet as SRTP on the selected pair (not in 0x66): the media path the
 * headset audio takes. pkt needs SRTP_TAG_LEN bytes of room after len (cap). */
bool rtc_link_send_srtp(rtc_link_t *L, uint8_t *pkt, size_t len, size_t cap);

/* The payload of an RTP packet; false if it is not RTP (or is RTCP). */
bool rtp_payload(const uint8_t *p, size_t len, uint8_t *pt, const uint8_t **payload, size_t *plen);

/* RpcMessage{request|notification{1 ticket, 2 payload}} (kind 1/3) or
 * {response{1 ticket, 2 code, 3 message, 4 payload?}} (kind 2), into out. 0 if it does not fit. */
size_t rpc_encode(int kind, uint32_t ticket, int code, const char *message, const uint8_t *payload, size_t plen,
                  uint8_t *out, size_t cap);

/* RpcMessage decoded: kind 1 request, 2 response, 3 notification (0 = undecodable). */
typedef struct {
    int kind;
    bool has_ticket;
    uint32_t ticket;
    uint64_t code;
    const uint8_t *payload;
    size_t plen;
} rpc_msg_t;
bool rpc_decode(const uint8_t *data, size_t len, rpc_msg_t *m);
