/* Diagnostic helpers: read-only service allow-list, trouble codes, bit extraction.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "canary/core.h"

#include <string.h>

/* CANary only ever reads. Anything that could change state in the vehicle
 * (clear codes, sessions, security access, actuator tests, writes, resets)
 * is refused here, below every other layer, so no profile or API call can
 * put it on the bus. */
bool cn_diag_sid_allowed(uint8_t sid)
{
    switch (sid) {
    case 0x01: /* current data */
    case 0x02: /* freeze frame */
    case 0x03: /* stored trouble codes */
    case 0x06: /* on-board monitor results */
    case 0x07: /* pending trouble codes */
    case 0x09: /* vehicle information */
    case 0x0A: /* permanent trouble codes */
    case 0x19: /* UDS read DTC information */
    case 0x22: /* UDS read data by identifier */
        return true;
    default:
        return false;
    }
}

void cn_dtc_format(uint8_t hi, uint8_t lo, char out[8])
{
    static const char letters[] = "PCBU";
    static const char hex[] = "0123456789ABCDEF";
    out[0] = letters[hi >> 6];
    out[1] = (char)('0' + ((hi >> 4) & 3));
    out[2] = hex[hi & 0x0F];
    out[3] = hex[lo >> 4];
    out[4] = hex[lo & 0x0F];
    out[5] = 0;
}

int cn_dtc_parse_obd(const uint8_t *resp, int len, uint8_t kind, uint32_t src, cn_dtc *out, int max)
{
    if (len < 2) return 0;
    int declared = resp[1];
    int avail = (len - 2) / 2;
    if (declared < avail) avail = declared;
    int n = 0;
    for (int i = 0; i < avail && n < max; i++) {
        uint8_t hi = resp[2 + i * 2], lo = resp[3 + i * 2];
        if (!hi && !lo) continue;
        memset(&out[n], 0, sizeof(out[n]));
        cn_dtc_format(hi, lo, out[n].code);
        out[n].kind = kind;
        out[n].ecu = 0xFF;
        out[n].src = src;
        n++;
    }
    return n;
}

int cn_dtc_parse_uds(const uint8_t *resp, int len, uint32_t src, uint8_t ecu, cn_dtc *out, int max)
{
    if (len < 3 || resp[0] != 0x59) return 0;
    int n = 0;
    for (int i = 3; i + 3 < len && n < max; i += 4) {
        uint8_t hi = resp[i], mid = resp[i + 1], ftb = resp[i + 2], st = resp[i + 3];
        if (!hi && !mid && !ftb) continue;
        memset(&out[n], 0, sizeof(out[n]));
        cn_dtc_format(hi, mid, out[n].code);
        out[n].ftb = ftb;
        out[n].status = st;
        /* bit 3 confirmed, bit 2 pending */
        out[n].kind = (st & 0x08) ? CN_DTC_STORED : CN_DTC_PENDING;
        out[n].ecu = ecu;
        out[n].src = src;
        n++;
    }
    return n;
}

bool cn_extract_bits(const uint8_t *data, int data_len, int bit, int nbits, bool sign, bool lsb, double *out)
{
    if (nbits < 1 || nbits > 64 || bit < 0) return false;
    if (bit + nbits > data_len * 8) return false;
    uint64_t v = 0;
    if (lsb && (bit % 8) == 0 && (nbits % 8) == 0) {
        int first = bit / 8, bytes = nbits / 8;
        for (int i = bytes - 1; i >= 0; i--) v = (v << 8) | data[first + i];
    } else {
        for (int i = 0; i < nbits; i++) {
            int b = bit + i;
            v = (v << 1) | ((data[b / 8] >> (7 - (b % 8))) & 1u);
        }
    }
    if (sign && nbits < 64 && (v & (1ull << (nbits - 1)))) {
        int64_t sv = (int64_t)(v | ~((1ull << nbits) - 1));
        *out = (double)sv;
    } else if (sign) {
        *out = (double)(int64_t)v;
    } else {
        *out = (double)v;
    }
    return true;
}
