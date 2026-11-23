/* ISO 15765-2 (ISO-TP) request/response over a raw CAN link.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Addressing covers the three cases seen in practice:
 *   - OBD-II functional, 11-bit:  7DF → 7E8..7EF, flow control to (responder - 8)
 *   - OBD-II functional, 29-bit:  18DB33F1 → 18DAF1xx, flow control to 18DAxxF1
 *   - physical (UDS):             tx → rx, flow control on tx
 */
#include "canary/core.h"

#include <string.h>

#define SLOTS 6
#define PAD 0x55

typedef struct {
    bool active, done;
    uint32_t id;
    uint16_t total, got;
    uint8_t next_seq;
    uint8_t buf[CN_ISOTP_MAX];
} slot_t;

/* Only the vehicle task talks on the bus, so one static reassembly area is enough. */
static slot_t s_slots[SLOTS];

static bool accept_id(const cn_addr *a, uint32_t id, bool ext)
{
    if (ext != a->ext) return false;
    if (a->rx) return id == a->rx;
    if (a->ext) return (id & 0xFFFFFF00u) == 0x18DAF100u;
    return id >= 0x7E8 && id <= 0x7EF;
}

static bool is_functional(const cn_addr *a) { return a->ext ? a->tx == 0x18DB33F1u : a->tx == 0x7DF; }

static uint32_t flow_control_id(const cn_addr *a, uint32_t responder)
{
    if (!is_functional(a)) return a->tx;
    if (a->ext) return 0x18DA00F1u | ((responder & 0xFF) << 8); /* 18DAF1xx → 18DAxxF1 */
    return responder - 8;
}

static int send_frame(const cn_platform *pf, uint32_t id, bool ext, const uint8_t *bytes, int n)
{
    cn_can_frame f = {.id = id, .ext = ext, .dlc = 8};
    memset(f.data, PAD, 8);
    memcpy(f.data, bytes, (size_t)n);
    return pf->can_send(pf->ctx, &f) == 0 ? 0 : CN_ERR_BUS;
}

static slot_t *slot_for(uint32_t id, bool create)
{
    for (int i = 0; i < SLOTS; i++)
        if (s_slots[i].active && s_slots[i].id == id) return &s_slots[i];
    if (!create) return NULL;
    for (int i = 0; i < SLOTS; i++)
        if (!s_slots[i].active) {
            memset(&s_slots[i], 0, offsetof(slot_t, buf));
            s_slots[i].active = true;
            s_slots[i].id = id;
            return &s_slots[i];
        }
    return NULL;
}

/* Send the request (single frame, or first frame + consecutive frames). */
static int send_request(const cn_platform *pf, const cn_addr *a, const uint8_t *req, int len)
{
    uint8_t d[8];
    if (len <= 7) {
        d[0] = (uint8_t)len;
        memcpy(d + 1, req, (size_t)len);
        return send_frame(pf, a->tx, a->ext, d, len + 1);
    }
    if (len > 4095 || is_functional(a) || !a->rx) return CN_ERR_PROTO; /* needs a physical peer */
    d[0] = (uint8_t)(0x10 | (len >> 8));
    d[1] = (uint8_t)len;
    memcpy(d + 2, req, 6);
    int rc = send_frame(pf, a->tx, a->ext, d, 8);
    if (rc) return rc;
    /* wait for flow control */
    int64_t deadline = pf->mono_ms(pf->ctx) + 1000;
    uint8_t stmin = 0;
    for (;;) {
        int left = (int)(deadline - pf->mono_ms(pf->ctx));
        if (left <= 0) return CN_ERR_TIMEOUT;
        cn_can_frame f;
        int r = pf->can_recv(pf->ctx, &f, left);
        if (r < 0) return CN_ERR_BUS;
        if (r == 0) return CN_ERR_TIMEOUT;
        if (f.id != a->rx || (f.data[0] >> 4) != 3) continue;
        uint8_t fs = f.data[0] & 0x0F;
        if (fs == 1) { /* wait */
            deadline = pf->mono_ms(pf->ctx) + 1000;
            continue;
        }
        if (fs != 0) return CN_ERR_PROTO; /* overflow */
        stmin = f.data[2];
        break;
    }
    int gap = stmin <= 0x7F ? stmin : 1;
    int sent = 6;
    uint8_t seq = 1;
    while (sent < len) {
        int n = len - sent > 7 ? 7 : len - sent;
        d[0] = (uint8_t)(0x20 | (seq & 0x0F));
        memcpy(d + 1, req + sent, (size_t)n);
        if (gap) pf->delay_ms(pf->ctx, gap);
        rc = send_frame(pf, a->tx, a->ext, d, n + 1);
        if (rc) return rc;
        sent += n;
        seq++;
    }
    return 0;
}

/* Feed one received frame into reassembly. Returns the completed slot, or NULL. */
static slot_t *feed(const cn_platform *pf, const cn_addr *a, const cn_can_frame *f, int *err)
{
    if (f->dlc < 2) return NULL;
    uint8_t pci = f->data[0] >> 4;
    if (pci == 0) {
        int len = f->data[0] & 0x0F;
        if (len == 0 || len > 7 || len > f->dlc - 1) return NULL;
        slot_t *s = slot_for(f->id, true);
        if (!s) return NULL;
        s->total = s->got = (uint16_t)len;
        memcpy(s->buf, f->data + 1, (size_t)len);
        s->done = true;
        return s;
    }
    if (pci == 1) {
        int total = ((f->data[0] & 0x0F) << 8) | f->data[1];
        if (total < 8) return NULL;
        slot_t *s = slot_for(f->id, true);
        if (!s) return NULL;
        if (total > CN_ISOTP_MAX) {
            s->active = false;
            *err = CN_ERR_OVERFLOW;
            return NULL;
        }
        s->total = (uint16_t)total;
        s->got = 6;
        s->next_seq = 1;
        s->done = false;
        memcpy(s->buf, f->data + 2, 6);
        uint8_t fc[3] = {0x30, 0x00, 0x00}; /* continue, no block limit, no separation time */
        if (send_frame(pf, flow_control_id(a, f->id), f->ext, fc, 3) != 0) *err = CN_ERR_BUS;
        return NULL;
    }
    if (pci == 2) {
        slot_t *s = slot_for(f->id, false);
        if (!s || s->done || !s->total) return NULL;
        if ((f->data[0] & 0x0F) != (s->next_seq & 0x0F)) {
            s->active = false;
            *err = CN_ERR_PROTO;
            return NULL;
        }
        int n = s->total - s->got;
        if (n > 7) n = 7;
        if (n > f->dlc - 1) n = f->dlc - 1;
        memcpy(s->buf + s->got, f->data + 1, (size_t)n);
        s->got = (uint16_t)(s->got + n);
        s->next_seq++;
        if (s->got >= s->total) {
            s->done = true;
            return s;
        }
    }
    return NULL;
}

static void drain(const cn_platform *pf)
{
    cn_can_frame f;
    for (int i = 0; i < 64 && pf->can_recv(pf->ctx, &f, 0) == 1; i++) {}
}

int cn_isotp_request(const cn_platform *pf, const cn_addr *a, const uint8_t *req, int req_len, uint8_t *resp,
                     int resp_cap, int timeout_ms, uint32_t *resp_id)
{
    if (req_len < 1 || !cn_diag_sid_allowed(req[0])) return CN_ERR_DENIED;
    for (int i = 0; i < SLOTS; i++) s_slots[i].active = false;
    drain(pf);
    int rc = send_request(pf, a, req, req_len);
    if (rc) return rc;

    int64_t deadline = pf->mono_ms(pf->ctx) + timeout_ms;
    int pending_nrc = 0, err = 0;
    for (;;) {
        int left = (int)(deadline - pf->mono_ms(pf->ctx));
        if (left <= 0) break;
        cn_can_frame f;
        int r = pf->can_recv(pf->ctx, &f, left);
        if (r < 0) return CN_ERR_BUS;
        if (r == 0) break;
        if (!accept_id(a, f.id, f.ext)) continue;
        uint8_t pci = f.data[0] >> 4;
        slot_t *s = feed(pf, a, &f, &err);
        if (pci == 1 || pci == 2) {
            /* a multi-frame transfer is in progress: allow N_Cr between frames */
            int64_t ext = pf->mono_ms(pf->ctx) + 1000;
            if (ext > deadline) deadline = ext;
        }
        if (!s) continue;
        s->active = false;
        if (s->total >= 3 && s->buf[0] == 0x7F && s->buf[1] == req[0]) {
            if (s->buf[2] == 0x78) { /* response pending */
                deadline = pf->mono_ms(pf->ctx) + 5000;
                continue;
            }
            pending_nrc = s->buf[2];
            if (!is_functional(a)) return CN_ERR_NRC(pending_nrc);
            continue; /* functional: another ECU may still answer positively */
        }
        if (s->buf[0] != (uint8_t)(req[0] + 0x40)) continue; /* not ours */
        if (s->total > resp_cap) return CN_ERR_OVERFLOW;
        memcpy(resp, s->buf, s->total);
        if (resp_id) *resp_id = s->id;
        return s->total;
    }
    if (pending_nrc) return CN_ERR_NRC(pending_nrc);
    return err ? err : CN_ERR_TIMEOUT;
}

int cn_isotp_request_multi(const cn_platform *pf, const cn_addr *a, const uint8_t *req, int req_len,
                           cn_isotp_msg *out, int max, int timeout_ms)
{
    if (req_len < 1 || !cn_diag_sid_allowed(req[0])) return CN_ERR_DENIED;
    for (int i = 0; i < SLOTS; i++) s_slots[i].active = false;
    drain(pf);
    int rc = send_request(pf, a, req, req_len);
    if (rc) return rc;
    int count = 0, err = 0;
    int64_t deadline = pf->mono_ms(pf->ctx) + timeout_ms;
    while (count < max) {
        int left = (int)(deadline - pf->mono_ms(pf->ctx));
        if (left <= 0) break;
        cn_can_frame f;
        int r = pf->can_recv(pf->ctx, &f, left);
        if (r < 0) return count ? count : CN_ERR_BUS;
        if (r == 0) break;
        if (!accept_id(a, f.id, f.ext)) continue;
        uint8_t pci = f.data[0] >> 4;
        slot_t *s = feed(pf, a, &f, &err);
        if (pci == 1 || pci == 2) {
            int64_t ext = pf->mono_ms(pf->ctx) + 1000;
            if (ext > deadline) deadline = ext;
        }
        if (!s) continue;
        s->active = false;
        if (s->total >= 3 && s->buf[0] == 0x7F) {
            if (s->buf[2] == 0x78) deadline = pf->mono_ms(pf->ctx) + 5000;
            continue;
        }
        if (s->buf[0] != (uint8_t)(req[0] + 0x40)) continue;
        out[count].id = s->id;
        out[count].len = s->total;
        memcpy(out[count].data, s->buf, s->total);
        count++;
    }
    return count;
}
