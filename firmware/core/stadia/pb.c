#include "stadia_pb.h"

#include <string.h>

void pbw_raw(pbw_t *w, const void *data, size_t len)
{
    if (w->overflow || len > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, data, len);
    w->len += len;
}

void pbw_varint(pbw_t *w, uint64_t v)
{
    uint8_t tmp[10];
    size_t n = 0;
    do {
        tmp[n] = v & 0x7F;
        v >>= 7;
        if (v) {
            tmp[n] |= 0x80;
        }
        n++;
    } while (v);
    pbw_raw(w, tmp, n);
}

static void tag(pbw_t *w, uint32_t field, int wt)
{
    pbw_varint(w, ((uint64_t)field << 3) | (uint32_t)wt);
}

void pbw_uint(pbw_t *w, uint32_t field, uint64_t v)
{
    tag(w, field, PB_VARINT);
    pbw_varint(w, v);
}

void pbw_sint64(pbw_t *w, uint32_t field, int64_t v)
{
    tag(w, field, PB_VARINT);
    pbw_varint(w, (uint64_t)v);
}

void pbw_bytes(pbw_t *w, uint32_t field, const void *data, size_t len)
{
    tag(w, field, PB_LEN);
    pbw_varint(w, len);
    pbw_raw(w, data, len);
}

void pbw_str(pbw_t *w, uint32_t field, const char *s)
{
    pbw_bytes(w, field, s, strlen(s));
}

static bool read_varint(pbr_t *r, uint64_t *out)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) {
            return false;
        }
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

bool pbr_next(pbr_t *r, pbf_t *f)
{
    if (r->error || r->p >= r->end) {
        return false;
    }
    f->start = r->p;
    uint64_t key;
    if (!read_varint(r, &key)) {
        goto bad;
    }
    f->field = (uint32_t)(key >> 3);
    f->wt = key & 7;
    f->data = NULL;
    f->len = 0;
    f->v = 0;
    switch (f->wt) {
    case PB_VARINT:
        if (!read_varint(r, &f->v)) {
            goto bad;
        }
        break;
    case PB_I64:
    case PB_I32: {
        size_t n = f->wt == PB_I64 ? 8 : 4;
        if ((size_t)(r->end - r->p) < n) {
            goto bad;
        }
        for (size_t i = 0; i < n; i++) {
            f->v |= (uint64_t)r->p[i] << (8 * i);
        }
        r->p += n;
        break;
    }
    case PB_LEN: {
        uint64_t n;
        if (!read_varint(r, &n) || n > (uint64_t)(r->end - r->p)) {
            goto bad;
        }
        f->data = r->p;
        f->len = (size_t)n;
        r->p += n;
        break;
    }
    default:
        goto bad;
    }
    f->total = (size_t)(r->p - f->start);
    return true;
bad:
    r->error = true;
    return false;
}

void pbf_str(const pbf_t *f, char *out, size_t out_len)
{
    size_t n = 0;
    if (f->wt == PB_LEN && out_len) {
        n = f->len < out_len - 1 ? f->len : out_len - 1;
        memcpy(out, f->data, n);
    }
    if (out_len) {
        out[n] = '\0';
    }
}

bool pb_find(const void *msg, size_t len, uint32_t field, pbf_t *out)
{
    pbr_t r = PBR(msg, len);
    while (pbr_next(&r, out)) {
        if (out->field == field) {
            return true;
        }
    }
    return false;
}
