/* Modelled car: a 2.0 turbo petrol engine with an 8-speed automatic, answering
 * OBD-II and (optionally) Volkswagen-style UDS requests over ISO-TP.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "simcar.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define QCAP 96
#define DT 0.1
#define BARO 98.0

typedef struct {
    bool active;
    uint32_t resp_id, fc_id;
    bool ext;
    uint8_t data[512];
    int len, sent;
    uint8_t seq;
} pending_t;

struct simcar {
    simcar_cfg cfg;
    simcar_seg *segs;
    int nsegs;
    bool loop;
    double script_len;
    double phase; /* script time at t = 0 */
    double t;
    uint32_t rng;
    /* state */
    bool ign, running;
    double speed, rpm, pedal, load, boost_target, boost_actual, coolant, oil, iat, vbat, off_for;
    double rail_target, rail_actual, lp_fuel, stft, ltft, advance, knock[4], misfire[4], misfire_acc;
    double cam_in_t, cam_in_a, cam_ex_t, cam_ex_a, odo, fuel_level, run_time, wastegate, fuel_rate;
    int gear;
    /* bus */
    cn_can_frame q[QCAP];
    int qh, qn;
    pending_t pend[4];
    unsigned frames_seen;
};

static double frand(simcar *c)
{
    c->rng = c->rng * 1664525u + 1013904223u;
    return ((c->rng >> 8) & 0xFFFF) / 65535.0;
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Faults that get worse as the simulated days go by, so trends have something to show. */
static double worn(simcar *c, double fault) { return fault * (1 + c->cfg.wear_per_day * c->t / 86400.0); }

simcar *simcar_create(const simcar_cfg *cfg)
{
    simcar *c = calloc(1, sizeof(*c));
    if (cfg) c->cfg = *cfg;
    if (!c->cfg.vin) c->cfg.vin = "3VV2B7AX5JM012345";
    if (c->cfg.ambient_c == 0) c->cfg.ambient_c = 12;
    if (c->cfg.odometer_km == 0) c->cfg.odometer_km = 165766;
    c->rng = 12345;
    c->odo = c->cfg.odometer_km;
    c->coolant = c->oil = c->iat = c->cfg.ambient_c;
    c->vbat = 12.5;
    c->off_for = 3600;
    c->fuel_level = 63;
    c->boost_actual = BARO;
    c->boost_target = BARO;
    c->lp_fuel = 520;
    c->gear = 1;
    simcar_script_commute(c, 12, true);
    return c;
}

void simcar_free(simcar *c)
{
    if (!c) return;
    free(c->segs);
    free(c);
}

void simcar_script(simcar *c, const simcar_seg *segs, int n, bool loop)
{
    free(c->segs);
    c->segs = malloc(sizeof(simcar_seg) * (size_t)n);
    memcpy(c->segs, segs, sizeof(simcar_seg) * (size_t)n);
    c->nsegs = n;
    c->loop = loop;
    c->script_len = 0;
    for (int i = 0; i < n; i++) c->script_len += segs[i].seconds;
}

/* One drive, key-on to key-off. Returns the number of segments written. */
static int drive_segments(simcar_seg *out, int minutes)
{
    double cruise = minutes * 60.0 - 430;
    if (cruise < 60) cruise = 60;
    const simcar_seg s[] = {
        {4, 0, 0, true},               /* key on */
        {35, 0, 0, false},             /* idle */
        {45, 48, 0, false},            /* pull away */
        {18, 0, 0, false},             /* lights */
        {70, 62, 0, false},
        {14, 0, 0, false},
        {9, 120, 100, false},          /* on-ramp, full throttle */
        {cruise * 0.45, 108, 0, false},
        {16, 0, 0, false},             /* roadworks, stopped */
        {26, 0, 0, false},
        {9, 118, 90, false},
        {7, 135, 100, false},          /* overtake */
        {cruise * 0.55, 110, 0, false},
        {9, 132, 78, false},           /* pass a truck */
        {25, 100, 0, false},
        {30, 55, 0, false},
        {10, 95, 85, false},           /* brisk pull */
        {60, 45, 0, false},
        {50, 0, 0, false},             /* arrive, idle */
    };
    int n = (int)(sizeof(s) / sizeof(s[0]));
    memcpy(out, s, sizeof(s));
    return n;
}

static double segments_len(const simcar_seg *s, int n)
{
    double t = 0;
    for (int i = 0; i < n; i++) t += s[i].seconds;
    return t;
}

void simcar_script_daily(simcar *c, double phase_s)
{
    simcar_seg s[64];
    int n = 0;
    const double morning = 7 * 3600 + 38 * 60, evening = 17 * 3600 + 12 * 60;
    s[n++] = (simcar_seg){morning, -1, 0, false};
    int a = drive_segments(s + n, 12);
    double used = morning + segments_len(s + n, a);
    n += a;
    s[n++] = (simcar_seg){evening - used, -1, 0, false};
    int b = drive_segments(s + n, 15);
    used = evening + segments_len(s + n, b);
    n += b;
    s[n++] = (simcar_seg){86400 - used, -1, 0, false};
    simcar_script(c, s, n, true);
    c->phase = fmod(phase_s, 86400);
    if (c->phase < 0) c->phase += 86400;
}

void simcar_script_commute(simcar *c, int minutes, bool loop)
{
    double cruise = minutes * 60.0 - 430;
    if (cruise < 60) cruise = 60;
    c->phase = 0;
    simcar_seg s[] = {
        {15, -1, 0, false},            /* parked */
        {4, 0, 0, true},               /* key on */
        {35, 0, 0, false},             /* idle */
        {45, 48, 0, false},            /* pull away */
        {18, 0, 0, false},             /* lights */
        {70, 62, 0, false},
        {14, 0, 0, false},
        {9, 120, 100, false},          /* on-ramp, full throttle */
        {cruise * 0.45, 108, 0, false},
        {16, 0, 0, false},             /* roadworks, stopped */
        {26, 0, 0, false},
        {9, 118, 90, false},
        {7, 135, 100, false},          /* overtake */
        {cruise * 0.55, 110, 0, false},
        {9, 132, 78, false},           /* pass a truck */
        {25, 100, 0, false},
        {30, 55, 0, false},
        {10, 95, 85, false},           /* brisk pull */
        {60, 45, 0, false},
        {50, 0, 0, false},             /* arrive, idle */
        {420, -1, 0, false},           /* parked */
    };
    simcar_script(c, s, (int)(sizeof(s) / sizeof(s[0])), loop);
}

static const simcar_seg *segment_at(simcar *c, double t)
{
    static const simcar_seg off = {1e9, -1, 0, false};
    if (!c->nsegs) return &off;
    t += c->phase;
    if (c->loop)
        t = fmod(t, c->script_len);
    else if (t >= c->script_len)
        return &off;
    for (int i = 0; i < c->nsegs; i++) {
        if (t < c->segs[i].seconds) return &c->segs[i];
        t -= c->segs[i].seconds;
    }
    return &off;
}

static void step(simcar *c)
{
    const simcar_seg *sg = segment_at(c, c->t);
    bool was_running = c->running;
    c->ign = sg->speed >= 0;
    c->running = c->ign && !sg->engine_off_ign_on;
    double amb = c->cfg.ambient_c;
    if (c->running && !was_running) {
        c->run_time = 0;
        memset(c->misfire, 0, sizeof(c->misfire));
    }
    if (!c->running) {
        c->speed = fmax(0, c->speed - 8 * DT);
        c->rpm = 0;
        c->pedal = 0;
        c->load = 0;
        c->boost_actual += (BARO - c->boost_actual) * DT / 0.5;
        c->boost_target = BARO;
        c->coolant += (amb - c->coolant) * DT / 3000;
        c->oil += (amb - c->oil) * DT / 3600;
        c->iat += (amb - c->iat) * DT / 600;
        c->off_for += DT;
        double rest = 12.45 + 0.45 * exp(-c->off_for / 90.0);
        c->vbat += ((c->ign ? rest - 0.25 : rest) - c->vbat) * DT / 2.0;
        c->fuel_rate = 0;
        return;
    }
    c->off_for = 0;
    c->run_time += DT;
    /* longitudinal */
    double target = sg->speed, acc;
    if (sg->pedal > 0) {
        c->pedal = c->speed < target ? sg->pedal : 20;
        acc = c->speed < target ? 3.0 * sg->pedal / 100.0 : 0;
    } else {
        acc = clampd((target - c->speed) * 0.25, -2.6, 1.9);
        c->pedal = acc > 0.05 ? clampd(10 + acc * 24 + c->speed * 0.13, 0, 70) : (c->speed > 5 && acc > -0.3 ? 6 + c->speed * 0.12 : 0);
    }
    c->speed = fmax(0, c->speed + acc * 3.6 * DT);
    /* drivetrain */
    static const double kmh_per_1000[8] = {8, 13, 20, 28, 37, 47, 58, 70};
    double idle = 750 + fmax(0, 55 - c->coolant) * 7;
    if (c->speed < 4) {
        c->gear = 1;
        c->rpm += (idle + c->pedal * 20 - c->rpm) * DT / 0.3;
    } else {
        double shift = 1900 + c->pedal * 36;
        int g = 0;
        while (g < 7 && c->speed / kmh_per_1000[g] * 1000 > shift) g++;
        while (g > 0 && c->speed / kmh_per_1000[g] * 1000 < 1150) g--;
        c->gear = g + 1;
        double rpm = fmax(idle, c->speed / kmh_per_1000[g] * 1000);
        c->rpm += (rpm - c->rpm) * DT / 0.25;
    }
    c->load = c->speed < 4 && c->pedal < 2 ? 24 + frand(c) * 2 : clampd(20 + c->pedal * 0.82, 15, 100);
    /* boost */
    double gauge = 0, target_abs;
    if (c->pedal > 35 && c->rpm > 1500) {
        gauge = fmin(132, (c->pedal - 35) / 65.0 * 136) * fmin(1, (c->rpm - 1300) / 1400.0);
        target_abs = BARO + gauge;
    } else {
        target_abs = c->speed < 4 && c->pedal < 2 ? 34 : 36 + c->pedal * 1.7;
    }
    double reach = target_abs <= BARO ? target_abs : BARO + (target_abs - BARO) * (1 - clampd(worn(c, c->cfg.boost_leak), 0, 0.6));
    c->boost_target = target_abs;
    c->boost_actual += (reach - c->boost_actual) * DT / 0.45;
    c->wastegate = clampd(gauge / 136 * 82 + worn(c, c->cfg.boost_leak) * 140 * (gauge > 5), 0, 100);
    /* ignition */
    bool loaded = c->pedal > 45 && c->rpm > 1800;
    for (int i = 0; i < 4; i++) {
        double k = loaded ? frand(c) * 1.6 : frand(c) * 0.3;
        if (i == 2 && loaded) k += c->cfg.knock_cyl3 * (0.75 + frand(c) * 0.5);
        c->knock[i] += (-k - c->knock[i]) * 0.5;
    }
    double worst = 0;
    for (int i = 0; i < 4; i++) worst = fmin(worst, c->knock[i]);
    c->advance = 30 - c->load * 0.24 + worst;
    if (c->cfg.misfire_rate > 0) {
        c->misfire_acc += c->cfg.misfire_rate / 60.0 * DT;
        if (c->misfire_acc >= 1) {
            c->misfire[1] += 1;
            c->misfire_acc -= 1;
        }
    }
    /* fuelling */
    bool at_idle = c->speed < 3 && c->rpm < 1100;
    c->ltft = 1.8 + c->cfg.idle_lean * (at_idle ? 0.7 : 0.12);
    c->stft += ((frand(c) - 0.5) * 4 + c->cfg.idle_lean * (at_idle ? 0.3 : 0.03) - c->stft) * 0.3;
    c->rail_target = at_idle ? 5200 : 6500 + c->load * 150;
    c->rail_actual = c->rail_target * (0.985 + frand(c) * 0.03);
    c->lp_fuel = 505 + frand(c) * 30;
    c->fuel_rate = 0.7 + c->load / 100.0 * c->rpm / 1000.0 * 4.2;
    c->fuel_level = fmax(2, c->fuel_level - c->fuel_rate * DT / 3600.0 / 58.0 * 100.0);
    /* valve timing */
    c->cam_in_t = clampd(c->load * 0.32, 0, 30);
    c->cam_in_a += (c->cam_in_t - c->cam_in_a) * DT / 0.3;
    c->cam_ex_t = clampd(c->load * 0.18, 0, 18);
    c->cam_ex_a += (c->cam_ex_t - c->cam_ex_a) * DT / 0.3;
    /* heat */
    double warm = c->cfg.thermostat_open ? amb + 46 + c->load * 0.1 - c->speed * 0.08 : 91 + (c->load - 30) * 0.09;
    c->coolant += (warm - c->coolant) * DT / (c->cfg.thermostat_open ? 420 : 210);
    c->oil += (c->coolant + 5 + c->load * 0.1 - c->oil) * DT / 380;
    double iat = amb + 5 + fmax(0, c->boost_actual - BARO) * 0.13 + (c->speed < 20 ? 11 : 0);
    c->iat += (iat - c->iat) * DT / 35;
    /* electrical */
    double v = c->cfg.charge_voltage > 0 ? c->cfg.charge_voltage : 14.25;
    c->vbat += (v - (at_idle ? 0.12 : 0) + (frand(c) - 0.5) * 0.06 - c->vbat) * DT / 1.5;
    c->odo += c->speed * DT / 3600.0;
}

void simcar_advance(simcar *c, double t)
{
    while (c->t + DT <= t) {
        step(c);
        c->t += DT;
    }
}

double simcar_voltage(simcar *c) { return c->vbat; }
double simcar_odometer(simcar *c) { return c->odo; }
bool simcar_ignition(simcar *c) { return c->ign; }
unsigned simcar_frames_seen(simcar *c) { return c->frames_seen; }

/* ───────────── bus ───────────── */

static void push(simcar *c, uint32_t id, bool ext, const uint8_t *d, int n)
{
    if (c->qn >= QCAP) return;
    cn_can_frame *f = &c->q[(c->qh + c->qn) % QCAP];
    f->id = id;
    f->ext = ext;
    f->dlc = 8;
    memset(f->data, 0xAA, 8);
    memcpy(f->data, d, (size_t)n);
    c->qn++;
}

bool simcar_tx(simcar *c, cn_can_frame *f)
{
    if (!c->qn) return false;
    *f = c->q[c->qh];
    c->qh = (c->qh + 1) % QCAP;
    c->qn--;
    return true;
}

static void respond(simcar *c, uint32_t resp_id, uint32_t fc_id, bool ext, const uint8_t *data, int len)
{
    uint8_t d[8];
    if (len <= 7) {
        d[0] = (uint8_t)len;
        memcpy(d + 1, data, (size_t)len);
        push(c, resp_id, ext, d, len + 1);
        return;
    }
    pending_t *p = NULL;
    for (int i = 0; i < 4; i++)
        if (!c->pend[i].active || c->pend[i].resp_id == resp_id) {
            p = &c->pend[i];
            break;
        }
    if (!p) return;
    p->active = true;
    p->resp_id = resp_id;
    p->fc_id = fc_id;
    p->ext = ext;
    p->len = len;
    memcpy(p->data, data, (size_t)len);
    p->sent = 6;
    p->seq = 1;
    d[0] = (uint8_t)(0x10 | (len >> 8));
    d[1] = (uint8_t)len;
    memcpy(d + 2, data, 6);
    push(c, resp_id, ext, d, 8);
}

static void flow_control(simcar *c, uint32_t id)
{
    for (int i = 0; i < 4; i++) {
        pending_t *p = &c->pend[i];
        if (!p->active || p->fc_id != id) continue;
        while (p->sent < p->len) {
            uint8_t d[8];
            int n = p->len - p->sent > 7 ? 7 : p->len - p->sent;
            d[0] = (uint8_t)(0x20 | (p->seq++ & 0x0F));
            memcpy(d + 1, p->data + p->sent, (size_t)n);
            push(c, p->resp_id, p->ext, d, n + 1);
            p->sent += n;
        }
        p->active = false;
    }
}

static int dtc_bytes(const char *list, uint8_t *out, int cap, bool uds, uint8_t status)
{
    int n = 0;
    if (!list) return 0;
    while (*list) {
        while (*list == ' ') list++;
        if (strlen(list) < 5) break;
        const char *letters = "PCBU", *l = strchr(letters, list[0]);
        if (!l) break;
        char mid[3] = {list[3], list[4], 0}, one[2] = {list[2], 0};
        unsigned d1 = (unsigned)(list[1] - '0') & 3;
        unsigned n2 = (unsigned)strtoul(one, NULL, 16);
        unsigned lo = (unsigned)strtoul(mid, NULL, 16);
        if (n + (uds ? 4 : 2) > cap) break;
        out[n++] = (uint8_t)(((unsigned)(l - letters) << 6) | (d1 << 4) | n2);
        out[n++] = (uint8_t)lo;
        if (uds) {
            out[n++] = 0x00;
            out[n++] = status;
        }
        list += 5;
    }
    return n;
}

static const uint8_t k_pids[] = {0x01, 0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x11, 0x1F, 0x21,
                                 0x2F, 0x30, 0x31, 0x33, 0x42, 0x44, 0x46, 0x49, 0x5C, 0x5E};

static bool pid_supported(simcar *c, uint8_t pid)
{
    for (size_t i = 0; i < sizeof(k_pids); i++)
        if (k_pids[i] == pid) return true;
    if (c->cfg.no_uds && (pid == 0x70 || pid == 0xA6)) return true;
    return false;
}

static int u16(uint8_t *o, double v)
{
    long x = lround(v);
    if (x < 0) x += 65536;
    o[0] = (uint8_t)(x >> 8);
    o[1] = (uint8_t)x;
    return 2;
}

static int obd_pid(simcar *c, uint8_t pid, uint8_t *o)
{
    if (pid % 0x20 == 0 && pid <= 0xA0) { /* support bitmap */
        uint32_t bits = 0;
        for (int i = 1; i <= 0x20; i++)
            if (pid_supported(c, (uint8_t)(pid + i))) bits |= 1u << (32 - i);
        bool more = false;
        for (int p = pid + 0x21; p <= 0xC0; p++)
            if (pid_supported(c, (uint8_t)p)) more = true;
        if (more) bits |= 1;
        o[0] = (uint8_t)(bits >> 24);
        o[1] = (uint8_t)(bits >> 16);
        o[2] = (uint8_t)(bits >> 8);
        o[3] = (uint8_t)bits;
        return 4;
    }
    if (!pid_supported(c, pid)) return -1;
    switch (pid) {
    case 0x01: {
        uint8_t tmp[32];
        int n = dtc_bytes(c->cfg.stored_dtcs, tmp, sizeof(tmp), false, 0) / 2;
        o[0] = (uint8_t)((c->cfg.mil ? 0x80 : 0) | n);
        o[1] = 0x07;
        o[2] = 0xE5;
        o[3] = 0x00;
        return 4;
    }
    case 0x04: o[0] = (uint8_t)lround(c->load * 2.55); return 1;
    case 0x05: o[0] = (uint8_t)lround(c->coolant + 40); return 1;
    case 0x06: o[0] = (uint8_t)lround((c->stft + 100) * 1.28); return 1;
    case 0x07: o[0] = (uint8_t)lround((c->ltft + 100) * 1.28); return 1;
    case 0x0B: o[0] = (uint8_t)clampd(lround(c->boost_actual), 0, 255); return 1;
    case 0x0C: return u16(o, c->rpm * 4);
    case 0x0D: o[0] = (uint8_t)lround(c->speed); return 1;
    case 0x0E: o[0] = (uint8_t)lround((c->advance + 64) * 2); return 1;
    case 0x0F: o[0] = (uint8_t)lround(c->iat + 40); return 1;
    case 0x11: o[0] = (uint8_t)lround(clampd(12 + c->pedal * 0.8, 0, 100) * 2.55); return 1;
    case 0x1F: return u16(o, c->run_time);
    case 0x21: return u16(o, c->cfg.mil ? 212 : 0);
    case 0x2F: o[0] = (uint8_t)lround(c->fuel_level * 2.55); return 1;
    case 0x30: o[0] = 141; return 1;
    case 0x31: return u16(o, 8420);
    case 0x33: o[0] = (uint8_t)BARO; return 1;
    case 0x42: return u16(o, c->vbat * 1000);
    case 0x44: return u16(o, 32768);
    case 0x46: o[0] = (uint8_t)lround(c->cfg.ambient_c + 40); return 1;
    case 0x49: o[0] = (uint8_t)lround(c->pedal * 2.55); return 1;
    case 0x5C: o[0] = (uint8_t)lround(c->oil + 40); return 1;
    case 0x5E: return u16(o, c->fuel_rate * 20);
    case 0x70:
        o[0] = 0x03;
        u16(o + 1, c->boost_target * 32);
        u16(o + 3, c->boost_actual * 32);
        memset(o + 5, 0, 5);
        return 10;
    case 0xA6: {
        uint32_t v = (uint32_t)(c->odo * 10);
        o[0] = (uint8_t)(v >> 24);
        o[1] = (uint8_t)(v >> 16);
        o[2] = (uint8_t)(v >> 8);
        o[3] = (uint8_t)v;
        return 4;
    }
    }
    return -1;
}

static int knock_raw(double deg) { return (int)lround((deg + 48) / 0.375); }

/* Engine DIDs this particular car answers. Others get "request out of range". */
static int engine_did(simcar *c, uint16_t did, uint8_t *o)
{
    switch (did) {
    case 0x2029: return u16(o, c->boost_target / 0.008);
    case 0x202A: return u16(o, c->boost_actual / 0.008);
    case 0x16EE: return u16(o, c->wastegate / 0.0122068184);
    case 0x149E: o[0] = (uint8_t)lround((c->iat + 48) / 0.75); return 1;
    case 0x202F: return u16(o, (c->oil + 273.1) * 10);
    case 0x200A: o[0] = (uint8_t)knock_raw(c->knock[0]); return 1;
    case 0x200B: o[0] = (uint8_t)knock_raw(c->knock[1]); return 1;
    case 0x200C: o[0] = (uint8_t)knock_raw(c->knock[2]); return 1;
    case 0x200D: o[0] = (uint8_t)knock_raw(c->knock[3]); return 1;
    case 0x291D: return u16(o, c->misfire[0]);
    case 0x2920: return u16(o, c->misfire[1]);
    case 0x291E: return u16(o, c->misfire[2]);
    case 0x291F: return u16(o, c->misfire[3]);
    case 0x2904: return u16(o, c->misfire[0] + c->misfire[1] + c->misfire[2] + c->misfire[3]);
    case 0x201E: return u16(o, c->cam_in_a * 10);
    case 0x201D: return u16(o, c->cam_in_t * 10);
    case 0x201A: return u16(o, c->cam_ex_a * 10);
    case 0x2019: return u16(o, c->cam_ex_t * 10);
    case 0x2021: return u16(o, -worn(c, c->cfg.chain_stretch) * 10);
    case 0x2023: return u16(o, -worn(c, c->cfg.chain_stretch) * 0.6 * 10);
    case 0x2027: return u16(o, c->rail_actual / 10);
    case 0x116A: return u16(o, c->rail_target / 10);
    case 0x2025: return u16(o, c->lp_fuel * 10);
    case 0x1047: return u16(o, c->load * 3.5 * 10);
    case 0x105E: return u16(o, c->vbat * 50);
    case 0x10E0: {
        uint32_t v = (uint32_t)c->odo;
        o[0] = (uint8_t)(v >> 24);
        o[1] = (uint8_t)(v >> 16);
        o[2] = (uint8_t)(v >> 8);
        o[3] = (uint8_t)v;
        return 4;
    }
    }
    return -1;
}

static void negative(simcar *c, uint32_t resp_id, bool ext, uint8_t sid, uint8_t nrc)
{
    uint8_t d[3] = {0x7F, sid, nrc};
    respond(c, resp_id, 0, ext, d, 3);
}

/* req = service bytes, functional = sent to the broadcast address */
static void engine_request(simcar *c, const uint8_t *req, int len, bool functional, uint32_t resp_id, uint32_t fc_id, bool ext)
{
    uint8_t out[128];
    int n;
    switch (req[0]) {
    case 0x01:
        if (len < 2) return;
        n = obd_pid(c, req[1], out + 2);
        if (n < 0) {
            if (!functional) negative(c, resp_id, ext, 0x01, 0x12);
            return; /* silent on functional requests, as real ECUs are */
        }
        out[0] = 0x41;
        out[1] = req[1];
        respond(c, resp_id, fc_id, ext, out, n + 2);
        return;
    case 0x03:
    case 0x07:
    case 0x0A: {
        const char *list = req[0] == 0x03 ? c->cfg.stored_dtcs : req[0] == 0x07 ? c->cfg.pending_dtcs : (c->cfg.mil ? c->cfg.stored_dtcs : NULL);
        n = dtc_bytes(list, out + 2, 60, false, 0);
        out[0] = (uint8_t)(req[0] + 0x40);
        out[1] = (uint8_t)(n / 2);
        respond(c, resp_id, fc_id, ext, out, n + 2);
        return;
    }
    case 0x09:
        if (len >= 2 && req[1] == 0x02) {
            out[0] = 0x49;
            out[1] = 0x02;
            out[2] = 0x01;
            memcpy(out + 3, c->cfg.vin, 17);
            respond(c, resp_id, fc_id, ext, out, 20);
        }
        return;
    case 0x22: {
        if (functional || c->cfg.no_uds || len < 3) {
            if (!functional) negative(c, resp_id, ext, 0x22, 0x11);
            return;
        }
        uint16_t did = (uint16_t)((req[1] << 8) | req[2]);
        n = engine_did(c, did, out + 3);
        if (n < 0) {
            negative(c, resp_id, ext, 0x22, 0x31);
            return;
        }
        out[0] = 0x62;
        out[1] = req[1];
        out[2] = req[2];
        respond(c, resp_id, fc_id, ext, out, n + 3);
        return;
    }
    case 0x19:
        if (functional || c->cfg.no_uds || len < 3 || req[1] != 0x02) return;
        out[0] = 0x59;
        out[1] = 0x02;
        out[2] = 0xFF;
        n = dtc_bytes(c->cfg.stored_dtcs, out + 3, 60, true, 0x09);
        n += dtc_bytes(c->cfg.pending_dtcs, out + 3 + n, 60, true, 0x04);
        respond(c, resp_id, fc_id, ext, out, n + 3);
        return;
    default:
        if (!functional) negative(c, resp_id, ext, req[0], 0x11);
    }
}

static void module_request(simcar *c, uint32_t tx, const uint8_t *req, int len)
{
    uint32_t rx = tx + 0x6A;
    uint8_t out[64];
    if (req[0] == 0x19 && len >= 3 && req[1] == 0x02) {
        if (tx == 0x70E || tx == 0x746) return; /* modules that stay silent */
        out[0] = 0x59;
        out[1] = 0x02;
        out[2] = 0xFF;
        int n = tx == 0x713 ? dtc_bytes(c->cfg.abs_dtcs, out + 3, 40, true, 0x09) : 0;
        respond(c, rx, tx, false, out, n + 3);
        return;
    }
    if (req[0] == 0x22 && len >= 3 && tx == 0x714) {
        uint16_t did = (uint16_t)((req[1] << 8) | req[2]);
        out[0] = 0x62;
        out[1] = req[1];
        out[2] = req[2];
        uint32_t km = (uint32_t)c->odo;
        if (did == 0x2203) {
            out[3] = (uint8_t)(km >> 16);
            out[4] = (uint8_t)(km >> 8);
            out[5] = (uint8_t)km;
            respond(c, rx, tx, false, out, 6);
            return;
        }
        if (did == 0x22B8) {
            uint32_t m = (uint32_t)(c->odo * 1000);
            out[3] = (uint8_t)(m >> 24);
            out[4] = (uint8_t)(m >> 16);
            out[5] = (uint8_t)(m >> 8);
            out[6] = (uint8_t)m;
            respond(c, rx, tx, false, out, 7);
            return;
        }
    }
    if (tx != 0x70E && tx != 0x746) negative(c, rx, false, req[0], req[0] == 0x22 ? 0x31 : 0x11);
}

void simcar_rx(simcar *c, const cn_can_frame *f)
{
    c->frames_seen++;
    if (!c->ign) return; /* asleep: nobody answers */
    uint8_t pci = f->data[0] >> 4;
    if (pci == 3) {
        flow_control(c, f->id);
        return;
    }
    if (pci != 0) return; /* the tester only ever sends single frames */
    int len = f->data[0] & 0x0F;
    if (len < 1 || len > 7) return;
    const uint8_t *req = f->data + 1;
    if (c->cfg.ext_ids) {
        if (!f->ext) return;
        if (f->id == 0x18DB33F1u)
            engine_request(c, req, len, true, 0x18DAF110u, 0x18DA10F1u, true);
        else if (f->id == 0x18DA10F1u)
            engine_request(c, req, len, false, 0x18DAF110u, 0x18DA10F1u, true);
        return;
    }
    if (f->ext) return;
    if (f->id == 0x7DF) {
        engine_request(c, req, len, true, 0x7E8, 0x7E0, false);
        /* the transmission also answers a couple of functional requests, a little later */
        if (!c->cfg.no_uds && req[0] == 0x01 && len >= 2 && (req[1] == 0x00 || req[1] == 0x0D)) {
            uint8_t out[6] = {0x41, req[1], 0, 0, 0, 0};
            if (req[1] == 0x00) {
                out[2] = 0x00;
                out[3] = 0x18; /* PIDs 0C, 0D */
                respond(c, 0x7E9, 0x7E1, false, out, 6);
            } else {
                out[2] = (uint8_t)lround(c->speed);
                respond(c, 0x7E9, 0x7E1, false, out, 3);
            }
        }
    } else if (f->id == 0x7E0) {
        engine_request(c, req, len, false, 0x7E8, 0x7E0, false);
    } else if (!c->cfg.no_uds && (f->id == 0x7E1 || f->id == 0x710 || f->id == 0x712 || f->id == 0x713 || f->id == 0x714 ||
                                  f->id == 0x715 || f->id == 0x70E || f->id == 0x746)) {
        if (f->id == 0x7E1) {
            uint8_t out[3] = {0x59, 0x02, 0xFF};
            if (req[0] == 0x19)
                respond(c, 0x7E9, 0x7E1, false, out, 3);
            else
                negative(c, 0x7E9, false, req[0], 0x31);
        } else {
            module_request(c, f->id, req, len);
        }
    }
}
