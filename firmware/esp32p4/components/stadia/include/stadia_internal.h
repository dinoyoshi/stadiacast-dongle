#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stadia.h"

#define STADIA_SERVER_NAME "stadia-local-receiver"
#define STADIA_VERSION "1.2.0-esp32p4"

extern stadia_config_t g_cfg;

/* ---- HTTP (https.c) ---------------------------------------------------------------- */

typedef struct {
    char method[8];
    char path[192];
    char query[192];
    char host[96];
    char device_id[64];       /* controller-device-id header */
    char cloud_device_id[64]; /* controller-cloud-device-id header */
    char peer[16];
    bool wants_close;
    uint8_t *body;
    size_t body_len;
} http_req_t;

typedef struct {
    int status;
    const char *content_type; /* NULL: no Content-Type */
    const uint8_t *body;
    size_t body_len;
    bool close;
    const char *handler; /* for the log */
} http_resp_t;

bool https_start(uint16_t port);

/* ---- the controller session (receiver.c) ------------------------------------------- */

/* Answer one request. The response body must stay valid until the next call on the same
 * connection, so it points into `scratch` (scratch_len bytes) or at static data. */
void receiver_handle(const http_req_t *req, http_resp_t *resp, uint8_t *scratch, size_t scratch_len);

/* ---- signaling (signaling.c) ------------------------------------------------------- */

typedef struct {
    char ufrag[64];
    char pwd[64];
    char fingerprint[128];
    char session_id[32];
    int candidates;
    char cand_ip[4][16];
    uint16_t cand_port[4];
} remote_offer_t;

/* Parse a WebRtcSdpAndIceCandidates offer bundle and build our answer bundle into `out`.
 * Returns the answer's length, 0 on failure. */
size_t signaling_answer(const uint8_t *offer_bundle, size_t len, remote_offer_t *remote, uint8_t *out, size_t out_cap);

/* ---- our DTLS identity and ICE credentials (rtc_identity.c) ------------------------ */

typedef struct {
    char ufrag[9];
    char pwd[25];
    char fingerprint[110]; /* "sha-256 AA:BB:..." */
    uint8_t *cert_der;
    size_t cert_der_len;
    uint8_t *key_der;
    size_t key_der_len;
} rtc_identity_t;

bool rtc_identity_init(void);
const rtc_identity_t *rtc_identity(void);

/* ---- small UDP servers ------------------------------------------------------------- */

bool dns_sink_start(uint16_t port, const char *answer_ip);
bool sntp_start(uint16_t port);
