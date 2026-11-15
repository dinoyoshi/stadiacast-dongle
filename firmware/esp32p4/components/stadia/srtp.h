/*
 * SRTP sender, AES128_CM_HMAC_SHA1_80 (RFC 3711), for one outgoing stream: the headset audio.
 * A port of rtc/srtp.py's protect_rtp (key derivation rate 0, as libsrtp does it).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbedtls/aes.h"

#define SRTP_TAG_LEN 10

typedef struct {
    bool ready;
    mbedtls_aes_context aes; /* the session key */
    uint8_t salt[14];        /* the session salt */
    uint8_t auth[20];        /* the session authentication key */
    uint32_t roc;            /* rollover counter */
    int32_t last_seq;        /* -1 before the first packet */
} srtp_tx_t;

/* From the DTLS-exported master key (16) and salt (14). */
void srtp_tx_init(srtp_tx_t *s, const uint8_t master_key[16], const uint8_t master_salt[14]);
void srtp_tx_free(srtp_tx_t *s);
/* Encrypt the payload of the RTP packet in pkt[0..len) in place and append the tag. Returns
 * the SRTP length (len + 10), or 0 if it does not fit in cap or is not an RTP packet. */
size_t srtp_protect(srtp_tx_t *s, uint8_t *pkt, size_t len, size_t cap);
