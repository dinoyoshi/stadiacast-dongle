/*
 * Our side of the display link: an ephemeral self-signed ECDSA P-256 certificate (WebRTC
 * style, like rtc/dtls.py's DtlsCertificate) and ICE credentials, made once per boot.
 */
#include "stadia_internal.h"
#include "stadia_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "rtc_id";
static rtc_identity_t s_id;

void rtc_ice_string(char *out, size_t n)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    uint8_t rnd[32];
    port_random(rnd, n);
    for (size_t i = 0; i < n; i++) {
        out[i] = alphabet[rnd[i] % 62];
    }
    out[n] = '\0';
}

bool rtc_identity_init(void)
{
    rtc_ice_string(s_id.ufrag, 8);
    rtc_ice_string(s_id.pwd, 24);

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    uint8_t *buf = malloc(1024);
    bool ok = false;
    int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"rtc", 3);
    if (rc == 0) rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc == 0) rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg);
    if (rc == 0) {
        uint8_t serial[8];
        port_random(serial, sizeof(serial));
        serial[0] &= 0x7F;
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        rc = mbedtls_x509write_crt_set_subject_name(&crt, "CN=" STADIA_SERVER_NAME);
        if (rc == 0) rc = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=" STADIA_SERVER_NAME);
        if (rc == 0) rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
        if (rc == 0) rc = mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20491231235959");
    }
    int len = rc == 0 ? mbedtls_x509write_crt_der(&crt, buf, 1024, mbedtls_ctr_drbg_random, &drbg) : rc;
    if (len > 0) {
        /* mbedTLS writes DER at the end of the buffer */
        s_id.cert_der = malloc((size_t)len);
        memcpy(s_id.cert_der, buf + 1024 - len, (size_t)len);
        s_id.cert_der_len = (size_t)len;
        int klen = mbedtls_pk_write_key_der(&key, buf, 1024);
        if (klen > 0) {
            s_id.key_der = malloc((size_t)klen);
            memcpy(s_id.key_der, buf + 1024 - klen, (size_t)klen);
            s_id.key_der_len = (size_t)klen;
            uint8_t hash[32];
            mbedtls_sha256(s_id.cert_der, s_id.cert_der_len, hash, 0);
            char *p = s_id.fingerprint + sprintf(s_id.fingerprint, "sha-256 ");
            for (int i = 0; i < 32; i++) {
                p += sprintf(p, i ? ":%02X" : "%02X", hash[i]);
            }
            ok = true;
        }
    } else {
        SLOGE(TAG, "certificate generation failed: -0x%04x", -len);
    }
    free(buf);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    if (ok) {
        SLOGI(TAG, "DTLS certificate %s, ICE ufrag %s", s_id.fingerprint, s_id.ufrag);
    }
    return ok;
}

const rtc_identity_t *rtc_identity(void)
{
    return &s_id;
}
