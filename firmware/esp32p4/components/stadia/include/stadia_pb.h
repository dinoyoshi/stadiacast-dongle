/*
 * Just enough protobuf for the controller's messages: a writer into a fixed buffer and a
 * field-by-field reader. Sub-messages are written into their own buffer first and then added
 * with pbw_bytes(), which keeps every length a minimal varint as the firmware's nanopb expects.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PB_VARINT = 0, PB_I64 = 1, PB_LEN = 2, PB_I32 = 5 };

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} pbw_t;

#define PBW(b, n) ((pbw_t){.buf = (b), .cap = (n)})

void pbw_varint(pbw_t *w, uint64_t v);
void pbw_uint(pbw_t *w, uint32_t field, uint64_t v);  /* also bool and enum */
void pbw_sint64(pbw_t *w, uint32_t field, int64_t v); /* int32/int64: two's complement varint */
void pbw_bytes(pbw_t *w, uint32_t field, const void *data, size_t len);
void pbw_str(pbw_t *w, uint32_t field, const char *s);
void pbw_raw(pbw_t *w, const void *data, size_t len);

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool error;
} pbr_t;

typedef struct {
    uint32_t field;
    int wt;
    uint64_t v;          /* PB_VARINT, PB_I64, PB_I32 */
    const uint8_t *data; /* PB_LEN */
    size_t len;
    const uint8_t *start; /* the whole field, tag included, for copying it verbatim */
    size_t total;
} pbf_t;

#define PBR(d, n) ((pbr_t){.p = (const uint8_t *)(d), .end = (const uint8_t *)(d) + (n)})

/* Next field; false at the end or on malformed input (then r->error is set). */
bool pbr_next(pbr_t *r, pbf_t *f);

/* Copy a PB_LEN field into a NUL-terminated string, truncated to fit. */
void pbf_str(const pbf_t *f, char *out, size_t out_len);

/* Find the first field `field` of a message; false if absent. */
bool pb_find(const void *msg, size_t len, uint32_t field, pbf_t *out);
