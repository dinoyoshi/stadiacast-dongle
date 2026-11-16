#include "srtp.h"

#include <string.h>

#include "mbedtls/md.h"

/* The AES-CM keystream for iv, XORed into buf (or written, if buf is all zero on entry). */
static void aes_cm(mbedtls_aes_context *aes, const uint8_t iv[16], uint8_t *buf, size_t n)
{
    uint8_t ctr[16], block[16];
    size_t off = 0;
    memcpy(ctr, iv, 16);
    mbedtls_aes_crypt_ctr(aes, n, &off, ctr, block, buf, buf);
}

/* RFC 3711 4.3.1 with key_derivation_rate 0: x = master_salt ^ (label << 48), IV = x << 16. */
static void kdf(mbedtls_aes_context *master, const uint8_t salt[14], uint8_t label, uint8_t *out, size_t n)
{
    uint8_t iv[16] = {0};
    memcpy(iv, salt, 14);
    iv[7] ^= label;
    memset(out, 0, n);
    aes_cm(master, iv, out, n);
}

void srtp_tx_init(srtp_tx_t *s, const uint8_t master_key[16], const uint8_t master_salt[14])
{
    if (s->ready) srtp_tx_free(s);
    mbedtls_aes_context master;
    mbedtls_aes_init(&master);
    mbedtls_aes_setkey_enc(&master, master_key, 128);
    uint8_t key[16];
    kdf(&master, master_salt, 0, key, sizeof(key));
    kdf(&master, master_salt, 1, s->auth, sizeof(s->auth));
    kdf(&master, master_salt, 2, s->salt, sizeof(s->salt));
    mbedtls_aes_free(&master);
    mbedtls_aes_init(&s->aes);
    mbedtls_aes_setkey_enc(&s->aes, key, 128);
    memset(key, 0, sizeof(key));
    s->roc = 0;
    s->last_seq = -1;
    s->ready = true;
}

void srtp_tx_free(srtp_tx_t *s)
{
    if (s->ready) mbedtls_aes_free(&s->aes);
    memset(s, 0, sizeof(*s));
}

/* The RTP header's length (CSRCs and the extension included), 0 if it does not fit in len. */
static size_t rtp_header_len(const uint8_t *pkt, size_t len)
{
    if (len < 12 || (pkt[0] >> 6) != 2) return 0;
    size_t hdr = 12 + 4 * (pkt[0] & 0x0F);
    if (pkt[0] & 0x10) {
        if (len < hdr + 4) return 0;
        hdr += 4 + 4 * ((pkt[hdr + 2] << 8) | pkt[hdr + 3]);
    }
    return hdr > len ? 0 : hdr;
}

/* IV = (session_salt << 16) ^ (ssrc << 64) ^ (index << 16), index = roc << 16 | seq */
static void cipher(srtp_tx_t *s, uint8_t *pkt, size_t hdr, size_t len, uint32_t roc, uint16_t seq)
{
    uint8_t iv[16] = {0};
    memcpy(iv, s->salt, 14);
    for (int i = 0; i < 4; i++) iv[4 + i] ^= pkt[8 + i];
    const uint8_t idx[6] = {roc >> 24, (roc >> 16) & 0xFF, (roc >> 8) & 0xFF, roc & 0xFF, seq >> 8, seq & 0xFF};
    for (int i = 0; i < 6; i++) iv[8 + i] ^= idx[i];
    aes_cm(&s->aes, iv, pkt + hdr, len - hdr);
}

/* tag = HMAC-SHA1(auth, packet || roc)[0..10) */
static void tag(srtp_tx_t *s, const uint8_t *pkt, size_t len, uint32_t roc, uint8_t mac[20])
{
    const uint8_t roc_be[4] = {roc >> 24, (roc >> 16) & 0xFF, (roc >> 8) & 0xFF, roc & 0xFF};
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
    mbedtls_md_hmac_starts(&md, s->auth, sizeof(s->auth));
    mbedtls_md_hmac_update(&md, pkt, len);
    mbedtls_md_hmac_update(&md, roc_be, 4);
    mbedtls_md_hmac_finish(&md, mac);
    mbedtls_md_free(&md);
}

size_t srtp_protect(srtp_tx_t *s, uint8_t *pkt, size_t len, size_t cap)
{
    size_t hdr = rtp_header_len(pkt, len);
    if (!s->ready || !hdr || len + SRTP_TAG_LEN > cap) return 0;
    uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if (s->last_seq >= 0 && seq < s->last_seq) s->roc++;
    s->last_seq = seq;
    cipher(s, pkt, hdr, len, s->roc, seq);
    uint8_t mac[20];
    tag(s, pkt, len, s->roc, mac);
    memcpy(pkt + len, mac, SRTP_TAG_LEN);
    return len + SRTP_TAG_LEN;
}

size_t srtp_unprotect(srtp_rx_t *s, uint8_t *pkt, size_t len)
{
    if (!s->ready || len < 12 + SRTP_TAG_LEN) return 0;
    len -= SRTP_TAG_LEN;
    size_t hdr = rtp_header_len(pkt, len);
    if (!hdr) return 0;
    uint16_t seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    /* RFC 3711 3.3.1: guess the index nearest the highest one seen */
    uint32_t roc = s->roc;
    if (s->last_seq >= 0) {
        int32_t d = (int32_t)seq - s->last_seq;
        if (d < -32768 && roc != 0xFFFFFFFFu) roc++;
        else if (d > 32768 && roc) roc--;
    }
    uint8_t mac[20];
    tag(s, pkt, len, roc, mac);
    uint8_t diff = 0;
    for (int i = 0; i < SRTP_TAG_LEN; i++) diff |= (uint8_t)(mac[i] ^ pkt[len + i]);
    if (diff) return 0;
    cipher(s, pkt, hdr, len, roc, seq);
    if (s->last_seq < 0 || roc > s->roc || (roc == s->roc && (int32_t)seq > s->last_seq)) {
        s->roc = roc;
        s->last_seq = seq;
    }
    return len;
}
