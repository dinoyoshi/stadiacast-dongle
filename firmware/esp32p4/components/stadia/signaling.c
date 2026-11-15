/*
 * WebRTC signaling in the controller's protobuf dialect (port of signaling.py).
 * The controller offers; we answer with our ICE credentials, DTLS fingerprint and one host
 * candidate. Field numbers are from src/stadiacast/schemas/webrtc_sdp.proto.
 */
#include "stadia_internal.h"
#include "stadia_pb.h"
#include "stadia_port.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "signaling";

enum { SDP_OFFER = 1, SDP_ANSWER = 2, ICE_CANDIDATE = 3 };
enum { VIDEO_CODEC_H264 = 1, VIDEO_CODEC_VP9 = 3 };
enum { NETWORK_TYPE_ETHERNET = 1 };
#define HOST_CANDIDATE_PRIORITY 2130706431u /* (126 << 24) | (65535 << 8) | 255 */

/* A WebRtcSdp{ICE_CANDIDATE}: remember its IPv4 address and port. */
static void take_candidate(const uint8_t *msg, size_t len, remote_offer_t *remote)
{
    pbf_t type, cand, f;
    if (!pb_find(msg, len, 1, &type) || type.v != ICE_CANDIDATE || !pb_find(msg, len, 5, &cand)) {
        return;
    }
    char ip[40] = "";
    uint32_t port = 0;
    pbr_t r = PBR(cand.data, cand.len);
    while (pbr_next(&r, &f)) {
        if (f.field == 5) pbf_str(&f, ip, sizeof(ip));
        if (f.field == 6) port = (uint32_t)f.v;
    }
    unsigned a, b, c, d;
    if (strchr(ip, ':') || sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || port == 0 || port > 65535) {
        SLOGI(TAG, "skipping candidate %s:%u (only IPv4 host candidates are used)", ip, (unsigned)port);
        return;
    }
    if (remote->candidates < 4) {
        snprintf(remote->cand_ip[remote->candidates], sizeof(remote->cand_ip[0]), "%.15s", ip);
        remote->cand_port[remote->candidates++] = (uint16_t)port;
    }
}

bool signaling_parse_offer(const uint8_t *offer, size_t len, remote_offer_t *remote, offer_echo_t *echo)
{
    memset(echo, 0, sizeof(*echo));
    pbf_t f, first_video = {0};
    pbr_t r = PBR(offer, len);
    while (pbr_next(&r, &f)) {
        switch (f.field) {
        case 1: pbf_str(&f, remote->session_id, sizeof(remote->session_id)); break;
        case 3: pbf_str(&f, remote->ufrag, sizeof(remote->ufrag)); break;
        case 4: pbf_str(&f, remote->pwd, sizeof(remote->pwd)); break;
        case 5: pbf_str(&f, remote->fingerprint, sizeof(remote->fingerprint)); break;
        case 16: { /* video_format: the first H264 or VP9 one, else the first */
            pbf_t codec;
            if (!first_video.data) first_video = f;
            if (!echo->video.data && pb_find(f.data, f.len, 1, &codec) &&
                (codec.v == VIDEO_CODEC_H264 || codec.v == VIDEO_CODEC_VP9)) {
                echo->video = f;
            }
            break;
        }
        case 17: if (!echo->audio.data) echo->audio = f; break;
        case 21: echo->raw_dtls = f.v != 0; break;
        case 23: echo->delayed_acks = f.v != 0; break;
        case 25: echo->rtp_ts = f.v != 0; break;
        }
    }
    if (!echo->video.data) echo->video = first_video;
    if (!remote->ufrag[0] || !remote->pwd[0]) {
        SLOGW(TAG, "the offer has no ICE credentials");
        return false;
    }
    return true;
}

size_t signaling_build_answer(const offer_echo_t *echo, const char *ufrag, const char *pwd, uint8_t *out, size_t cap)
{
    /* RtcSdpAnswer, fields in number order as the Python receiver serializes it.
     * --boreal off (the live preset): no use_boreal. */
    pbw_t a = PBW(out, cap);
    pbw_str(&a, 1, ufrag);
    pbw_str(&a, 2, pwd);
    pbw_str(&a, 3, rtc_identity()->fingerprint);
    if (echo->video.data) pbw_bytes(&a, 8, echo->video.data, echo->video.len);
    if (echo->audio.data) pbw_bytes(&a, 9, echo->audio.data, echo->audio.len);
    pbw_uint(&a, 10, echo->raw_dtls);
    pbw_uint(&a, 11, echo->delayed_acks);
    pbw_uint(&a, 12, 1); /* disable_game_stats */
    pbw_uint(&a, 13, 1); /* disable_frame_tokens */
    pbw_str(&a, 14, STADIA_SERVER_NAME "/" STADIA_VERSION);
    pbw_uint(&a, 15, NETWORK_TYPE_ETHERNET);
    pbw_uint(&a, 17, echo->rtp_ts);
    return a.overflow ? 0 : a.len;
}

size_t signaling_build_candidate(uint16_t port, uint8_t *out, size_t cap)
{
    pbw_t c = PBW(out, cap); /* RtcIceCandidate: our one host candidate */
    pbw_str(&c, 1, "0");
    pbw_uint(&c, 2, 0);
    pbw_str(&c, 3, "1");
    pbw_uint(&c, 4, HOST_CANDIDATE_PRIORITY);
    pbw_str(&c, 5, g_cfg.advertise_ip);
    pbw_uint(&c, 6, port);
    return c.overflow ? 0 : c.len;
}

size_t signaling_answer(const uint8_t *bundle, size_t len, remote_offer_t *remote, uint8_t *out, size_t out_cap)
{
    memset(remote, 0, sizeof(*remote));
    pbf_t f, sdp = {0}, offer = {0};
    bool have_sdp = false;
    pbr_t r = PBR(bundle, len);
    while (pbr_next(&r, &f)) {
        if (f.field == 1 && f.wt == PB_LEN) {
            sdp = f;
            have_sdp = true;
        } else if (f.field == 2 && f.wt == PB_LEN) {
            take_candidate(f.data, f.len, remote);
        }
    }
    pbf_t type;
    if (!have_sdp || !pb_find(sdp.data, sdp.len, 1, &type) || type.v != SDP_OFFER ||
        !pb_find(sdp.data, sdp.len, 3, &offer)) {
        SLOGW(TAG, "the bundle carries no SDP offer");
        return 0;
    }
    offer_echo_t echo;
    if (!signaling_parse_offer(offer.data, offer.len, remote, &echo)) return 0;

    const rtc_identity_t *id = rtc_identity();
    uint8_t ans_buf[1024], sdp_buf[1100], cand_buf[96], cmsg_buf[128];
    size_t alen = signaling_build_answer(&echo, id->ufrag, id->pwd, ans_buf, sizeof(ans_buf));
    size_t clen = signaling_build_candidate(g_cfg.rtc_port, cand_buf, sizeof(cand_buf));

    pbw_t s = PBW(sdp_buf, sizeof(sdp_buf)); /* WebRtcSdp{SDP_ANSWER} */
    pbw_uint(&s, 1, SDP_ANSWER);
    pbw_bytes(&s, 4, ans_buf, alen);

    pbw_t cm = PBW(cmsg_buf, sizeof(cmsg_buf)); /* WebRtcSdp{ICE_CANDIDATE} */
    pbw_uint(&cm, 1, ICE_CANDIDATE);
    pbw_bytes(&cm, 5, cand_buf, clen);

    pbw_t o = PBW(out, out_cap); /* WebRtcSdpAndIceCandidates */
    pbw_bytes(&o, 1, s.buf, s.len);
    pbw_bytes(&o, 2, cm.buf, cm.len);
    if (!alen || !clen || s.overflow || cm.overflow || o.overflow) {
        SLOGE(TAG, "answer does not fit");
        return 0;
    }
    SLOGI(TAG, "offer %s: raw_dtls=%d delayed_acks=%d, %d IPv4 candidate(s); answering with %s:%u", remote->session_id,
          echo.raw_dtls, echo.delayed_acks, remote->candidates, g_cfg.advertise_ip, (unsigned)g_cfg.rtc_port);
    return o.len;
}
