/* The vehicle side of the engine: protocol detection, identification, the
 * polling scheduler, trip detection and trouble-code sweeps.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bus etiquette, in order of importance:
 *   1. Read-only services only (enforced in isotp.c / diag.c).
 *   2. When the car stops answering, stop transmitting. A sleeping car is
 *      only probed again when the supply voltage says the engine is running.
 *   3. With the ignition on but the engine off, polling winds down to a slow
 *      heartbeat and then stops, so CANary never keeps control units awake.
 */
#include "canary/core.h"

#include "cJSON.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define REQ_TIMEOUT_MS 160
#define PROBE_TIMEOUT_MS 240
#define TICK_MS 500
#define KOEO_FULL_MS (3 * 60 * 1000)   /* full polling with the engine off */
#define KOEO_MAX_MS (15 * 60 * 1000)   /* then heartbeat only, then silence */
#define STOP_GRACE_MS (5 * 60 * 1000)  /* engine stopped but ECU alive (start/stop systems) */
#define DTC_INTERVAL_MS (10 * 60 * 1000)

static int64_t now_ms(cn_engine *e) { return e->pf.mono_ms(e->pf.ctx); }
static void lock(cn_engine *e) { if (e->pf.lock) e->pf.lock(e->pf.ctx); }
static void unlock(cn_engine *e) { if (e->pf.unlock) e->pf.unlock(e->pf.ctx); }

const char *cn_vs_name(int vs)
{
    static const char *const n[] = {"starting", "detecting", "identifying", "running", "ignition_on", "off"};
    return vs >= 0 && vs <= CN_VS_OFF ? n[vs] : "?";
}

void cn_log(cn_engine *e, char level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lock(e);
    cn_event *ev = &e->events[e->ev_head];
    e->ev_head = (e->ev_head + 1) % CN_EVENTS;
    if (e->ev_count < CN_EVENTS) e->ev_count++;
    ev->t_wall = e->pf.wall_s(e->pf.ctx);
    ev->t_mono = now_ms(e);
    ev->level = level;
    vsnprintf(ev->text, sizeof(ev->text), fmt, ap);
    cn_event copy = *ev;
    unlock(e);
    va_end(ap);
    fprintf(stderr, "[canary %c] %s\n", level, copy.text);
    const char *dir = e->pf.data_dir ? e->pf.data_dir(e->pf.ctx) : NULL;
    if (dir) {
        char path[180];
        snprintf(path, sizeof(path), "%s/device.log", dir);
        FILE *f = fopen(path, "a");
        if (f) {
            char ts[24] = "-";
            if (copy.t_wall > 0) cn_iso8601(copy.t_wall, ts);
            fprintf(f, "%s +%llds %c %s\n", ts, (long long)(copy.t_mono / 1000), level, copy.text);
            long size = ftell(f);
            fclose(f);
            if (size > 512 * 1024) { /* keep one generation */
                char old[190];
                snprintf(old, sizeof(old), "%s/device.log.1", dir);
                remove(old);
                rename(path, old);
            }
        }
    }
}

/* ───────────── persisted state ───────────── */

static void load_state(cn_engine *e)
{
    char *text = e->pf.kv_load ? e->pf.kv_load(e->pf.ctx, "state") : NULL;
    cJSON *d = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!d) return;
    const cJSON *v;
#define NUM(key, lhs, type) \
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(d, key))) lhs = (type)v->valuedouble
    NUM("odo_km", e->last_odo_km, double);
    NUM("odo_wall", e->last_odo_wall, int64_t);
    NUM("odo_sent_km", e->sync.last_odo_sent_km, double);
    NUM("odo_sent_wall", e->sync.last_odo_sent_wall, int64_t);
    NUM("can_bitrate", e->can_bitrate, int);
    NUM("boot_count", e->boot_count, uint32_t);
#undef NUM
    e->can_ext = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "can_ext"));
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(d, "vin"))) cn_strlcpy(e->vin, v->valuestring, sizeof(e->vin));
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(d, "profile")))
        cn_strlcpy(e->state_profile, v->valuestring, sizeof(e->state_profile));
    cJSON_Delete(d);
}

static void save_state(cn_engine *e)
{
    if (!e->pf.kv_save) return;
    cn_buf b;
    cn_buf_init(&b);
    cn_jw w;
    cn_jw_init(&w, &b);
    cn_jw_obj(&w);
    cn_jw_knum(&w, "odo_km", e->last_odo_km, 1);
    cn_jw_kint(&w, "odo_wall", e->last_odo_wall);
    cn_jw_knum(&w, "odo_sent_km", e->sync.last_odo_sent_km, 1);
    cn_jw_kint(&w, "odo_sent_wall", e->sync.last_odo_sent_wall);
    cn_jw_kint(&w, "can_bitrate", e->can_bitrate);
    cn_jw_kbool(&w, "can_ext", e->can_ext);
    cn_jw_kint(&w, "boot_count", e->boot_count);
    cn_jw_kstr(&w, "vin", e->vin);
    cn_jw_kstr(&w, "profile", e->profile ? e->profile->id : "");
    cn_jw_end_obj(&w);
    char *s = cn_buf_take(&b);
    if (s) e->pf.kv_save(e->pf.ctx, "state", s);
    free(s);
    e->last_state_save_ms = now_ms(e);
}

void cn_engine_save_config(cn_engine *e)
{
    char *s = cn_config_to_json(&e->cfg, false);
    if (s && e->pf.kv_save) e->pf.kv_save(e->pf.ctx, "config", s);
    free(s);
}

/* ───────────── profile handling ───────────── */

typedef struct {
    cn_engine *e;
    int found;
    int depth;
} dead_ctx;

static bool signal_dead(cn_engine *e, int idx, int depth);

static void dead_visit(void *ctx, int var)
{
    dead_ctx *dc = ctx;
    if (dc->found < 0 && signal_dead(dc->e, var, dc->depth + 1)) dc->found = var;
}

/* True when a signal can never produce a value on this car: every source was tried and refused. */
static bool signal_dead(cn_engine *e, int idx, int depth)
{
    cn_signal *s = &e->profile->signals[idx];
    if (depth > 4) return false;
    if (s->active >= 0) {
        cn_source *src = &s->src[s->active];
        if (src->kind != CN_SRC_CALC) return false;
        dead_ctx dc = {e, -1, depth};
        cn_expr_vars(src->calc, dead_visit, &dc);
        return dc.found >= 0;
    }
    for (int k = 0; k < s->nsrc; k++) {
        cn_source *src = &s->src[k];
        if (src->req >= 0 && e->profile->requests[src->req].sup == CN_SUP_UNKNOWN) return false; /* still to be tried */
    }
    return true;
}

bool cn_signal_available(cn_engine *e, int idx)
{
    if (idx < 0 || idx >= e->profile->nsignals) return false;
    return e->profile->signals[idx].active >= 0 && !signal_dead(e, idx, 0);
}

static void recompute_needed(cn_engine *e)
{
    cn_profile *p = e->profile;
    int pending = 0;
    for (int i = 0; i < p->nrequests; i++) p->requests[i].needed = false;
    for (int i = 0; i < p->nsignals; i++) {
        cn_signal *s = &p->signals[i];
        s->active = -1;
        bool probing = false;
        for (int k = 0; k < s->nsrc && s->active < 0; k++) {
            cn_source *src = &s->src[k];
            if (src->kind == CN_SRC_CALC) {
                s->active = (int8_t)k;
            } else if (src->kind == CN_SRC_DEVICE) {
                if (src->code != CN_DEV_VBAT || e->has_vbat) s->active = (int8_t)k;
            } else {
                cn_request *r = &p->requests[src->req];
                if (r->sup == CN_SUP_OK) {
                    s->active = (int8_t)k;
                    r->needed = true;
                } else if (r->sup == CN_SUP_UNKNOWN && !probing) {
                    /* worth probing; a later source can stand in meanwhile */
                    r->needed = true;
                    probing = true;
                }
            }
        }
    }
    for (int i = 0; i < p->nrequests; i++)
        if (p->requests[i].needed && p->requests[i].sup == CN_SUP_UNKNOWN) pending++;
    e->probe_pending = pending;
    /* monitors whose inputs this car will never provide say so instead of waiting forever */
    for (int i = 0; i < p->nmonitors; i++) {
        cn_monitor *m = &p->monitors[i];
        if (!m->enabled) continue;
        dead_ctx dc = {e, -1, 0};
        if (m->when) cn_expr_vars(m->when, dead_visit, &dc);
        if (m->value) cn_expr_vars(m->value, dead_visit, &dc);
        m->unavailable = dc.found >= 0;
        cn_strlcpy(m->missing, dc.found >= 0 ? p->signals[dc.found].name : "", sizeof(m->missing));
    }
}

static void apply_pid_support(cn_engine *e)
{
    cn_profile *p = e->profile;
    if (!e->pid_support_known) return;
    for (int i = 0; i < p->nrequests; i++) {
        cn_request *r = &p->requests[i];
        if (r->ecu != 0 || r->mode != 0x01 || r->code == 0 || r->code > 0xE0) continue;
        int idx = (r->code - 1) / 32, bit = 31 - ((r->code - 1) % 32);
        r->sup = (e->pid_support[idx] >> bit) & 1u ? CN_SUP_OK : CN_SUP_NO;
    }
}

static char *support_path(cn_engine *e)
{
    const char *dir = e->pf.data_dir ? e->pf.data_dir(e->pf.ctx) : NULL;
    if (!dir || !e->vin[0] || !e->profile) return NULL;
    cn_buf b;
    cn_buf_init(&b);
    cn_buf_printf(&b, "%s/support-%s-%s.json", dir, e->vin, e->profile->hash);
    return cn_buf_take(&b);
}

/* Which manufacturer requests this car answers is remembered per VIN and profile version. */
static void load_support(cn_engine *e)
{
    char *path = support_path(e);
    if (!path) return;
    char *text = cn_read_file(path, NULL);
    free(path);
    cJSON *d = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!d) return;
    cn_profile *p = e->profile;
    const cJSON *it;
    int n = 0;
    cJSON_ArrayForEach(it, d)
    {
        if (!cJSON_IsNumber(it)) continue;
        uint32_t key = (uint32_t)it->valuedouble; /* ecu<<24 | mode<<16 | code, bit 31 = supported */
        for (int i = 0; i < p->nrequests; i++) {
            cn_request *r = &p->requests[i];
            if ((((uint32_t)r->ecu << 24) | ((uint32_t)r->mode << 16) | r->code) == (key & 0x7FFFFFFFu) && r->mode != 0x01) {
                r->sup = (key & 0x80000000u) ? CN_SUP_OK : CN_SUP_NO;
                n++;
            }
        }
    }
    cJSON_Delete(d);
    if (n) cn_log(e, 'i', "Loaded what this car supports from the last drive (%d requests)", n);
}

static void save_support(cn_engine *e)
{
    char *path = support_path(e);
    if (!path) return;
    cn_profile *p = e->profile;
    cn_buf b;
    cn_buf_init(&b);
    cn_buf_puts(&b, "[");
    bool first = true;
    for (int i = 0; i < p->nrequests; i++) {
        cn_request *r = &p->requests[i];
        if (r->sup == CN_SUP_UNKNOWN || r->mode == 0x01) continue;
        uint32_t key = ((uint32_t)r->ecu << 24) | ((uint32_t)r->mode << 16) | r->code | (r->sup == CN_SUP_OK ? 0x80000000u : 0);
        cn_buf_printf(&b, "%s%u", first ? "" : ",", (unsigned)key);
        first = false;
    }
    cn_buf_puts(&b, "]");
    if (b.p) cn_write_file(path, b.p, b.len);
    cn_buf_free(&b);
    free(path);
}

static bool load_profile(cn_engine *e, const char *id)
{
    char err[160] = "";
    cn_profile *p = cn_profile_load(id, e->loader, e->loader_ctx, err, sizeof(err));
    if (!p) {
        cn_log(e, 'e', "Profile %s failed to load: %s", id, err);
        return false;
    }
    double *vars = calloc((size_t)(p->nsignals ? p->nsignals : 1), sizeof(double));
    if (!vars) {
        cn_profile_free(p);
        return false;
    }
    lock(e);
    cn_profile *old = e->profile;
    free(e->vars);
    e->vars = vars;
    e->profile = p;
    e->rpm_sig = cn_profile_signal_index(p, "rpm");
    e->speed_sig = cn_profile_signal_index(p, "speed");
    e->odo_sig = cn_profile_signal_index(p, "odometer");
    e->fuel_rate_sig = cn_profile_signal_index(p, "fuel_rate");
    e->mil_sig = cn_profile_signal_index(p, "mil");
    e->rr_cursor = 0;
    for (int i = 0; i < p->nsignals; i++) vars[i] = NAN;
    cn_monitors_reset(p);
    apply_pid_support(e);
    load_support(e);
    recompute_needed(e);
    unlock(e);
    cn_profile_free(old);
    int disabled = 0;
    for (int i = 0; i < p->nmonitors; i++)
        if (!p->monitors[i].enabled) disabled++;
    cn_log(e, 'i', "Profile %s (%s): %d signals, %d requests, %d monitors%s", p->id, p->hash, p->nsignals, p->nrequests,
           p->nmonitors, disabled ? " (some unavailable)" : "");
    return true;
}

/* Choose the best profile for a VIN from everything the loader offers. */
static void pick_profile_for_vin(cn_engine *e, char *out, size_t n)
{
    cn_strlcpy(out, "generic-obd2", n);
    char *index = e->loader(e->loader_ctx, CN_PROFILE_INDEX);
    cJSON *d = index ? cJSON_Parse(index) : NULL;
    free(index);
    int best = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, d)
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
        if (!cJSON_IsString(id)) continue;
        char *doc = e->loader(e->loader_ctx, id->valuestring);
        if (!doc) continue;
        int score = cn_profile_match_vin(doc, e->vin);
        free(doc);
        if (score > best) {
            best = score;
            cn_strlcpy(out, id->valuestring, n);
        }
    }
    cJSON_Delete(d);
}

void cn_engine_reload_profile(cn_engine *e)
{
    char id[40];
    if (strcmp(e->cfg.profile, "auto") != 0)
        cn_strlcpy(id, e->cfg.profile, sizeof(id));
    else if (e->vin[0])
        pick_profile_for_vin(e, id, sizeof(id));
    else if (e->state_profile[0])
        cn_strlcpy(id, e->state_profile, sizeof(id));
    else
        cn_strlcpy(id, "generic-obd2", sizeof(id));
    if (e->profile && !strcmp(e->profile->id, id)) return;
    if (e->trip.active) cn_trip_end(e, "profile changed");
    if (!load_profile(e, id) && strcmp(id, "generic-obd2") != 0) load_profile(e, "generic-obd2");
}

/* ───────────── engine lifecycle ───────────── */

cn_engine *cn_engine_create(const cn_platform *pf, cn_profile_loader loader, void *loader_ctx, const char *device_id,
                            const char *board)
{
    cn_engine *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->pf = *pf;
    e->loader = loader;
    e->loader_ctx = loader_ctx;
    cn_strlcpy(e->device_id, device_id, sizeof(e->device_id));
    cn_strlcpy(e->board, board, sizeof(e->board));
    e->boot_mono = now_ms(e);
    e->vbat = NAN;
    e->vbat_min_off = NAN;
    e->dtc_phase = -1;
    e->rpm_sig = e->speed_sig = e->odo_sig = e->fuel_rate_sig = e->mil_sig = -1;
    e->last_ui_ms = -1000000;
    cn_config_defaults(&e->cfg);
    char *cfg = pf->kv_load ? pf->kv_load(pf->ctx, "config") : NULL;
    if (cfg) cn_config_from_json(&e->cfg, cfg);
    free(cfg);
    load_state(e);
    e->boot_count++;
    float v = pf->vbat ? pf->vbat(pf->ctx) : NAN;
    e->has_vbat = !isnan(v);
    cn_log(e, 'i', "CANary %s on %s (%s), boot %u", CN_FW_VERSION, board, device_id, (unsigned)e->boot_count);
    cn_engine_reload_profile(e);
    if (!e->profile) {
        cn_log(e, 'e', "No usable profile; cannot continue");
        free(e);
        return NULL;
    }
    cn_trips_scan(e);
    save_state(e);
    return e;
}

void cn_engine_destroy(cn_engine *e)
{
    if (!e) return;
    if (e->trip.active) cn_trip_end(e, "shutdown");
    if (e->can_open && e->pf.can_close) e->pf.can_close(e->pf.ctx);
    cn_profile_free(e->profile);
    free(e->vars);
    free(e);
}

void cn_net_update(cn_engine *e, const cn_net_status *st)
{
    lock(e);
    bool was = e->net.connected;
    e->net = *st;
    if (st->connected && !was) {
        e->sync.connected_since_ms = now_ms(e);
        e->sync.did_connect_sync = false;
    }
    unlock(e);
    if (st->connected && !was) cn_log(e, 'i', "Wi-Fi connected to %s (%s)", st->ssid, st->ip);
    if (!st->connected && was) cn_log(e, 'i', "Wi-Fi disconnected");
}

/* ───────────── signals ───────────── */

void cn_signal_update(cn_engine *e, int idx, double value, int64_t now)
{
    cn_signal *s = &e->profile->signals[idx];
    if (isnan(value) || isinf(value)) return;
    if (s->has_range && (value < s->lo || value > s->hi)) return;
    s->value = value;
    s->valid = true;
    s->t_ms = now;
    if (e->trip.active) {
        s->n++;
        s->sum += value;
        if (value < s->vmin) s->vmin = value;
        if (value > s->vmax) s->vmax = value;
    }
    if (s->log) {
        /* A reading that has not changed is written again only often enough to show it is still alive:
         * whoever reads the log holds each value until it goes stale (cn_rate_stale_ms). This roughly
         * halves the size of a recording without losing anything. */
        int stale = cn_rate_stale_ms[s->rate];
        int repeat = stale ? stale * 2 / 5 : 60000;
        if (!s->logged || value != s->log_value || now - s->log_t >= repeat) {
            cn_trip_sample(e, idx, value, now);
            s->logged = true;
            s->log_value = value;
            s->log_t = now;
        }
    }
    if (idx == e->odo_sig && value > 0) {
        e->last_odo_km = value;
        e->last_odo_wall = e->pf.wall_s(e->pf.ctx);
    }
}

static void build_vars(cn_engine *e, int64_t now)
{
    cn_profile *p = e->profile;
    for (int i = 0; i < p->nsignals; i++) {
        cn_signal *s = &p->signals[i];
        int stale = cn_rate_stale_ms[s->rate];
        e->vars[i] = (s->valid && (stale == 0 || now - s->t_ms <= stale)) ? s->value : NAN;
    }
}

/* Device-side and calculated signals, once a second. */
static void update_derived(cn_engine *e, int64_t now)
{
    cn_profile *p = e->profile;
    build_vars(e, now);
    for (int i = 0; i < p->nsignals; i++) {
        cn_signal *s = &p->signals[i];
        if (s->active < 0) continue;
        cn_source *src = &s->src[s->active];
        if (src->kind == CN_SRC_CALC) {
            cn_signal_update(e, i, cn_expr_eval(src->calc, e->vars), now);
        } else if (src->kind == CN_SRC_DEVICE) {
            if (src->code == CN_DEV_VBAT)
                cn_signal_update(e, i, e->vbat, now);
            else if (src->code == CN_DEV_TRIP_TIME && e->trip.active)
                cn_signal_update(e, i, (now - e->trip.start_mono) / 1000.0, now);
        }
    }
}

int cn_dtc_severity(const cn_engine *e)
{
    int sev = CN_SEV_OK;
    for (int i = 0; i < e->ndtcs; i++) {
        const cn_dtc *d = &e->dtcs[i];
        int s;
        bool powertrain = true;
        if (d->ecu != 0xFF && e->profile && d->ecu < e->profile->necus) {
            const char *id = e->profile->ecus[d->ecu].id;
            powertrain = !strcmp(id, "engine") || !strcmp(id, "trans") || !strcmp(id, "obd");
        }
        if (d->kind == CN_DTC_PERMANENT)
            s = CN_SEV_ADVISORY;
        else if (!powertrain)
            s = CN_SEV_ADVISORY;
        else if (d->kind == CN_DTC_STORED && e->mil)
            s = CN_SEV_CRITICAL;
        else
            s = CN_SEV_WARNING;
        if (s > sev) sev = s;
    }
    return sev;
}

int cn_worst_severity(const cn_engine *e, int *count_above_ok)
{
    int worst = cn_dtc_severity(e), count = worst > CN_SEV_OK ? 1 : 0;
    for (int i = 0; e->profile && i < e->profile->nmonitors; i++) {
        const cn_monitor *m = &e->profile->monitors[i];
        if (!m->enabled || m->status == CN_SEV_NODATA || m->status == CN_SEV_OK) continue;
        count++;
        if (m->status > worst) worst = m->status;
    }
    if (count_above_ok) *count_above_ok = count;
    return worst;
}

/* ───────────── bus access ───────────── */

static cn_addr addr_for(cn_engine *e, int ecu)
{
    cn_ecu *c = &e->profile->ecus[ecu];
    cn_addr a;
    if (ecu == 0) {
        a = e->obd;
        if (c->rx) a.rx = c->rx;
    } else {
        a.tx = c->tx;
        a.rx = c->rx;
        a.ext = c->ext;
    }
    return a;
}

static bool open_can(cn_engine *e, int bitrate)
{
    if (e->can_open && e->can_bitrate == bitrate) return true;
    if (e->can_open) {
        e->pf.can_close(e->pf.ctx);
        e->can_open = false;
    }
    if (e->pf.can_open(e->pf.ctx, bitrate) != 0) {
        int64_t now = now_ms(e);
        if (now - e->last_can_open_ms > 60000 || !e->last_can_open_ms) cn_log(e, 'e', "CAN interface failed to start at %d bit/s", bitrate);
        e->last_can_open_ms = now;
        return false;
    }
    e->can_open = true;
    e->can_bitrate = bitrate;
    return true;
}

/* 0 = no answer, 1 = protocol found, -1 = nobody acknowledged (wrong bit rate or bus asleep) */
static int try_protocol(cn_engine *e, int bitrate, bool ext)
{
    if (!open_can(e, bitrate)) return -1;
    cn_addr a = {.tx = ext ? 0x18DB33F1u : 0x7DF, .rx = 0, .ext = ext};
    uint8_t req[2] = {0x01, 0x00}, resp[16];
    e->bus_tx++;
    int n = cn_isotp_request(&e->pf, &a, req, 2, resp, sizeof(resp), 300, NULL);
    if (n >= 6 && resp[1] == 0x00) {
        e->bus_rx++;
        e->obd = a;
        e->can_ext = ext;
        e->pid_support[0] = ((uint32_t)resp[2] << 24) | ((uint32_t)resp[3] << 16) | ((uint32_t)resp[4] << 8) | resp[5];
        return 1;
    }
    return n == CN_ERR_BUS ? -1 : 0;
}

static bool detect(cn_engine *e)
{
    cn_profile *p = e->profile;
    int pinned_rate = p->can_bitrate, pinned_bits = p->can_id_bits;
    int rates[2] = {e->can_bitrate ? e->can_bitrate : 500000, 0};
    rates[1] = rates[0] == 500000 ? 250000 : 500000;
    if (pinned_rate) {
        rates[0] = pinned_rate;
        rates[1] = 0;
    }
    for (int r = 0; r < 2 && rates[r]; r++) {
        bool first_ext = pinned_bits ? pinned_bits == 29 : e->can_ext;
        int rc = try_protocol(e, rates[r], first_ext);
        if (rc == 1) return true;
        if (rc == 0 && !pinned_bits) {
            /* someone acknowledged at this bit rate, so only the identifier length is in question */
            if (try_protocol(e, rates[r], !first_ext) == 1) return true;
            return false;
        }
        if (rc == 0) return false;
    }
    return false;
}

static void read_pid_support(cn_engine *e)
{
    uint8_t resp[16];
    for (int i = 1; i < 7; i++) {
        if (!(e->pid_support[i - 1] & 1u)) break; /* bit for PID (i*0x20) */
        uint8_t req[2] = {0x01, (uint8_t)(i * 0x20)};
        e->bus_tx++;
        int n = cn_isotp_request(&e->pf, &e->obd, req, 2, resp, sizeof(resp), 300, NULL);
        if (n < 6) break;
        e->bus_rx++;
        e->pid_support[i] = ((uint32_t)resp[2] << 24) | ((uint32_t)resp[3] << 16) | ((uint32_t)resp[4] << 8) | resp[5];
    }
    e->pid_support_known = true;
}

static void read_vin(cn_engine *e)
{
    static cn_isotp_msg msgs[3];
    uint8_t req[2] = {0x09, 0x02};
    e->bus_tx++;
    int n = cn_isotp_request_multi(&e->pf, &e->obd, req, 2, msgs, 3, 400);
    for (int i = 0; i < n; i++) {
        if (msgs[i].len < 19 || msgs[i].data[1] != 0x02) continue;
        const uint8_t *v = msgs[i].data + msgs[i].len - 17;
        char vin[18];
        bool ok = true;
        for (int k = 0; k < 17; k++) {
            char c = (char)v[k];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) ok = false;
            vin[k] = c;
        }
        vin[17] = 0;
        if (ok) {
            e->bus_rx++;
            if (strcmp(e->vin, vin) != 0) {
                cn_strlcpy(e->vin, vin, sizeof(e->vin));
                cn_log(e, 'i', "VIN %s", vin);
            }
            return;
        }
    }
}

/* ───────────── trouble codes ───────────── */

static void dtc_add(cn_engine *e, const cn_dtc *d)
{
    for (int i = 0; i < e->ndtc_tmp; i++) {
        cn_dtc *x = &e->dtc_tmp[i];
        if (!strcmp(x->code, d->code) && x->kind == d->kind && x->src == d->src && x->ftb == d->ftb) return;
    }
    if (e->ndtc_tmp < CN_MAX_DTCS) e->dtc_tmp[e->ndtc_tmp++] = *d;
}

/* Every trip log carries the trouble codes present during it. */
static void log_dtc_event(cn_engine *e)
{
    cn_buf b;
    cn_buf_init(&b);
    cn_buf_puts(&b, "{\"codes\":[");
    for (int i = 0; i < e->ndtcs; i++) {
        const cn_dtc *d = &e->dtcs[i];
        const char *ecu = d->ecu != 0xFF && d->ecu < e->profile->necus ? e->profile->ecus[d->ecu].id : "obd";
        cn_buf_printf(&b, "%s{\"code\":\"%s\",\"kind\":%d,\"ecu\":\"%s\",\"ftb\":%d,\"status\":%d}", i ? "," : "", d->code, d->kind,
                      ecu, d->ftb, d->status);
    }
    cn_buf_puts(&b, "]}");
    if (b.p) cn_trip_event(e, "dtc", b.p);
    cn_buf_free(&b);
    e->trip.dtc_logged = true;
}

/* One step of the sweep per call so live polling keeps flowing in between. */
static void dtc_step(cn_engine *e)
{
    static cn_isotp_msg msgs[4];
    cn_profile *p = e->profile;
    cn_dtc found[16];
    int phase = e->dtc_phase;
    if (phase == 0) e->ndtc_tmp = 0;
    if (phase < 3) {
        static const uint8_t modes[3] = {0x03, 0x07, 0x0A};
        static const uint8_t kinds[3] = {CN_DTC_STORED, CN_DTC_PENDING, CN_DTC_PERMANENT};
        uint8_t req[1] = {modes[phase]};
        e->bus_tx++;
        int n = cn_isotp_request_multi(&e->pf, &e->obd, req, 1, msgs, 4, 300);
        for (int i = 0; i < n; i++) {
            e->bus_rx++;
            int k = cn_dtc_parse_obd(msgs[i].data, msgs[i].len, kinds[phase], msgs[i].id, found, 16);
            for (int j = 0; j < k; j++) dtc_add(e, &found[j]);
        }
        e->dtc_phase++;
        return;
    }
    int ecu = phase - 3 + 1; /* skip ecu 0 (functional) */
    while (ecu < p->necus && !p->ecus[ecu].dtc) ecu++;
    if (ecu < p->necus) {
        cn_addr a = addr_for(e, ecu);
        uint8_t req[3] = {0x19, 0x02, 0x0C}; /* confirmed or pending */
        static uint8_t resp[CN_ISOTP_MAX];
        e->bus_tx++;
        int n = cn_isotp_request(&e->pf, &a, req, 3, resp, sizeof(resp), 300, NULL);
        if (n > 0) {
            e->bus_rx++;
            int k = cn_dtc_parse_uds(resp, n, a.rx, (uint8_t)ecu, found, 16);
            for (int j = 0; j < k; j++) {
                /* the engine ECU reports the same codes through OBD-II modes; keep one copy */
                bool dup = false;
                for (int x = 0; x < e->ndtc_tmp; x++)
                    if (!strcmp(e->dtc_tmp[x].code, found[j].code) && e->dtc_tmp[x].src == found[j].src &&
                        e->dtc_tmp[x].kind == found[j].kind)
                        dup = true;
                if (!dup) dtc_add(e, &found[j]);
            }
        }
        e->dtc_phase = ecu + 3;
        return;
    }
    /* sweep complete */
    uint32_t sig = 2166136261u;
    for (int i = 0; i < e->ndtc_tmp; i++) sig = cn_fnv1a(&e->dtc_tmp[i], offsetof(cn_dtc, ecu), sig);
    lock(e);
    memcpy(e->dtcs, e->dtc_tmp, sizeof(cn_dtc) * (size_t)e->ndtc_tmp);
    e->ndtcs = e->ndtc_tmp;
    e->dtc_read_wall = e->pf.wall_s(e->pf.ctx);
    bool changed = sig != e->dtc_sig;
    e->dtc_sig = sig;
    unlock(e);
    e->dtc_phase = -1;
    e->last_dtc_ms = now_ms(e);
    if (changed) {
        cn_buf b;
        cn_buf_init(&b);
        for (int i = 0; i < e->ndtcs; i++) cn_buf_printf(&b, "%s%s", i ? " " : "", e->dtcs[i].code);
        cn_log(e, e->ndtcs ? 'w' : 'i', e->ndtcs ? "Trouble codes: %s" : "No trouble codes%s", e->ndtcs ? b.p : "");
        cn_buf_free(&b);
    }
    if (e->trip.active && (changed || !e->trip.dtc_logged)) log_dtc_event(e);
}

/* ───────────── polling ───────────── */

static int choose_request(cn_engine *e, int64_t now, bool heartbeat_only)
{
    cn_profile *p = e->profile;
    if (heartbeat_only) {
        if (e->rpm_sig < 0) return -1;
        cn_signal *s = &p->signals[e->rpm_sig];
        if (s->active < 0 || s->src[s->active].req < 0) return -1;
        int ri = s->src[s->active].req;
        return now >= p->requests[ri].next_ms ? ri : -1;
    }
    e->pick_count++;
    int best = -1, probe = -1;
    int64_t best_late = -1;
    for (int i = 0; i < p->nrequests; i++) {
        cn_request *r = &p->requests[i];
        if (!r->needed || now < r->next_ms) continue;
        if (r->sup == CN_SUP_UNKNOWN) {
            if (probe < 0) probe = i;
            continue;
        }
        if (r->rate == CN_RATE_FAST) continue;
        int64_t late = now - r->next_ms;
        if (late > best_late) {
            best_late = late;
            best = i;
        }
    }
    /* probes take every third slot so live data keeps flowing while a new car is being learned */
    if (probe >= 0 && (e->pick_count % 3 == 0 || best < 0)) return probe;
    if (best >= 0) return best;
    for (int k = 1; k <= p->nrequests; k++) {
        int i = (e->rr_cursor + k) % p->nrequests;
        cn_request *r = &p->requests[i];
        if (r->needed && r->sup == CN_SUP_OK && r->rate == CN_RATE_FAST) {
            e->rr_cursor = i;
            return i;
        }
    }
    return probe;
}

static void decode(cn_engine *e, int ri, const uint8_t *resp, int len, int64_t now)
{
    cn_profile *p = e->profile;
    cn_request *r = &p->requests[ri];
    int skip = r->mode == 0x22 ? 3 : 2;
    if (len < skip) return;
    for (int i = 0; i < p->nsignals; i++) {
        cn_signal *s = &p->signals[i];
        if (s->active < 0) continue;
        cn_source *src = &s->src[s->active];
        if (src->req != ri) continue;
        double raw;
        if (!cn_extract_bits(resp + skip, len - skip, src->bit, src->len, src->sign, src->lsb, &raw)) continue;
        cn_signal_update(e, i, raw * src->mul / src->div + src->add, now);
    }
}

static void poll_one(cn_engine *e, int64_t now, bool heartbeat_only)
{
    cn_profile *p = e->profile;
    int ri = choose_request(e, now, heartbeat_only);
    if (ri < 0) {
        e->pf.delay_ms(e->pf.ctx, heartbeat_only ? 100 : 20);
        return;
    }
    cn_request *r = &p->requests[ri];
    cn_addr a = addr_for(e, r->ecu);
    uint8_t req[3], resp[128];
    int rl;
    if (r->mode == 0x22) {
        req[0] = 0x22;
        req[1] = (uint8_t)(r->code >> 8);
        req[2] = (uint8_t)r->code;
        rl = 3;
    } else {
        req[0] = r->mode;
        req[1] = (uint8_t)r->code;
        rl = 2;
    }
    bool was_unknown = r->sup == CN_SUP_UNKNOWN;
    e->bus_tx++;
    int n = cn_isotp_request(&e->pf, &a, req, rl, resp, sizeof(resp), was_unknown ? PROBE_TIMEOUT_MS : REQ_TIMEOUT_MS, NULL);
    now = now_ms(e);
    int period = cn_rate_period_ms[r->rate];
    lock(e);
    bool echo_ok = n > rl - 1 && (r->mode == 0x22 ? (resp[1] == req[1] && resp[2] == req[2]) : resp[1] == req[1]);
    if (n > 0 && echo_ok) {
        e->bus_rx++;
        e->last_response_ms = now;
        e->consecutive_timeouts = 0;
        r->fails = 0;
        r->ok_count++;
        r->last_ok_ms = now;
        r->last_err = 0;
        if (was_unknown) {
            r->sup = CN_SUP_OK;
            e->support_dirty = true;
            recompute_needed(e);
        }
        decode(e, ri, resp, n, now);
        r->next_ms = heartbeat_only ? now + 2000 : period < 0 ? INT64_MAX : now + period;
    } else {
        r->err_count++;
        r->last_err = (int16_t)(n > 0 ? CN_ERR_PROTO : n);
        bool alive = now - e->last_response_ms < 1500;
        if (CN_IS_NRC(n)) {
            int nrc = CN_NRC_OF(n);
            e->last_response_ms = now; /* a refusal still proves the control unit is awake */
            e->consecutive_timeouts = 0;
            bool hard = nrc == 0x31 || nrc == 0x11 || nrc == 0x12 || nrc == 0x13 || nrc == 0x33 || nrc == 0x7F || nrc == 0x7E;
            r->fails++;
            if ((was_unknown && (hard || r->fails >= 4)) || r->fails >= 10) {
                r->sup = CN_SUP_NO;
                e->support_dirty = true;
                recompute_needed(e);
            }
        } else if (n == CN_ERR_BUS) {
            e->bus_err++;
            e->consecutive_timeouts++;
        } else {
            if (!was_unknown || r->ecu == 0) e->consecutive_timeouts++;
            if (was_unknown && alive) {
                /* the car is talking but this request is ignored: not supported */
                if (++r->fails >= 2) {
                    r->sup = CN_SUP_NO;
                    e->support_dirty = true;
                    recompute_needed(e);
                }
            } else if (!was_unknown && alive && ++r->fails >= 25) {
                r->sup = CN_SUP_NO; /* answered once, then went quiet for good */
                recompute_needed(e);
            }
        }
        r->next_ms = now + (was_unknown ? 1500 : heartbeat_only ? 2000 : period > 0 ? period : 250);
    }
    unlock(e);
}

static void run_adhoc(cn_engine *e)
{
    cn_adhoc req;
    lock(e);
    req = e->adhoc;
    unlock(e);
    int64_t t0 = now_ms(e);
    int result = CN_ERR_BUS;
    uint32_t id = 0;
    static uint8_t resp[256];
    if (e->can_open || open_can(e, e->can_bitrate ? e->can_bitrate : 500000)) {
        e->bus_tx++;
        result = cn_isotp_request(&e->pf, &req.addr, req.req, req.req_len, resp, sizeof(resp), 1000, &id);
    }
    lock(e);
    e->adhoc.result = result;
    e->adhoc.resp_id = id;
    if (result > 0) memcpy(e->adhoc.resp, resp, (size_t)result);
    e->adhoc.elapsed_ms = (int)(now_ms(e) - t0);
    e->adhoc.pending = false;
    e->adhoc.done = true;
    if (result > 0 || CN_IS_NRC(result)) e->last_response_ms = now_ms(e);
    unlock(e);
}

static void set_state(cn_engine *e, int vs)
{
    if (e->vs == vs) return;
    e->vs = vs;
    e->vs_since_ms = now_ms(e);
}

static void go_off(cn_engine *e, const char *why)
{
    if (e->trip.active) cn_trip_end(e, why);
    int64_t now = now_ms(e);
    lock(e);
    set_state(e, CN_VS_OFF);
    e->off_since_ms = now;
    e->vbat_min_off = e->vbat;
    e->wake_armed = isnan(e->vbat) || e->vbat < e->cfg.wake_voltage - 0.15;
    e->consecutive_timeouts = 0;
    e->dtc_phase = -1;
    e->heartbeat_logged = false;
    /* stale readings must not look live in the UI */
    for (int i = 0; e->profile && i < e->profile->nsignals; i++) {
        cn_source *src = e->profile->signals[i].active >= 0 ? &e->profile->signals[i].src[e->profile->signals[i].active] : NULL;
        if (src && (src->kind == CN_SRC_OBD || src->kind == CN_SRC_UDS)) e->profile->signals[i].valid = false;
    }
    unlock(e);
    if (e->support_dirty) {
        save_support(e);
        e->support_dirty = false;
    }
    save_state(e);
    cn_log(e, 'i', "Vehicle is off (%s); no more requests until the engine runs again", why);
}

bool cn_power_should_wake(const cn_config *cfg, float vbat, float vbat_min_asleep)
{
    if (isnan(vbat)) return false;
    if (!isnan(vbat_min_asleep)) {
        if (vbat >= cfg->wake_voltage && vbat_min_asleep < cfg->wake_voltage - 0.15) return true;
        if (vbat >= vbat_min_asleep + 0.6 && vbat > 12.9) return true; /* alternator came on */
        return false;
    }
    return vbat >= cfg->wake_voltage;
}

bool cn_power_can_sleep(cn_engine *e, char *why, size_t why_len)
{
    int64_t now = now_ms(e);
    const char *reason = NULL;
    bool ok = false;
    lock(e);
    int64_t off_for = now - e->off_since_ms;
    if (!e->cfg.sleep_enabled)
        reason = "sleep is turned off";
    else if (!e->has_vbat || isnan(e->vbat))
        reason = "this board cannot measure battery voltage";
    else if (e->vbat < 6.0)
        reason = "running from USB power";
    else if (e->vs != CN_VS_OFF)
        reason = "vehicle is awake";
    else if (off_for < 30000)
        reason = "vehicle just turned off";
    else if (e->vbat < 11.6) {
        reason = "battery is low";
        ok = true;
    } else if (off_for < (int64_t)e->cfg.sleep_delay_s * 1000)
        reason = "waiting before sleep";
    else if ((e->sync.busy || (e->sync.pending_trips > 0 && e->net.connected)) &&
             off_for < (int64_t)e->cfg.sleep_delay_s * 1000 + 15 * 60 * 1000)
        reason = "upload in progress";
    else if (now - e->last_ui_ms < 3 * 60 * 1000 && off_for < 60 * 60 * 1000)
        reason = "web interface in use";
    else {
        reason = "vehicle off";
        ok = true;
    }
    unlock(e);
    if (why && why_len) cn_strlcpy(why, reason, why_len);
    return ok;
}

static void periodic(cn_engine *e, int64_t now)
{
    cn_profile *p = e->profile;
    /* battery voltage and derived signals: 1 Hz */
    if (now - e->last_derived_ms >= 1000) {
        e->last_derived_ms = now;
        float v = e->pf.vbat ? e->pf.vbat(e->pf.ctx) : NAN;
        lock(e);
        e->vbat = v;
        e->vbat_ms = now;
        if (e->vs == CN_VS_OFF && !isnan(v)) {
            if (isnan(e->vbat_min_off) || v < e->vbat_min_off) e->vbat_min_off = v;
            if (v < e->cfg.wake_voltage - 0.15) e->wake_armed = true;
        }
        update_derived(e, now);
        if (e->mil_sig >= 0 && p->signals[e->mil_sig].valid) e->mil = p->signals[e->mil_sig].value > 0;
        /* Recent history for the live view: every signal on the same clock, so all the small charts
         * cover the same few minutes whatever their polling rate. A signal that goes quiet starts over. */
        if (now - e->last_hist_ms >= CN_HIST_STEP_MS) {
            e->last_hist_ms = now;
            for (int i = 0; i < p->nsignals; i++) {
                cn_signal *s = &p->signals[i];
                int stale = cn_rate_stale_ms[s->rate];
                if (!s->valid || (stale && now - s->t_ms > stale)) {
                    s->hist_n = 0;
                    continue;
                }
                s->hist[s->hist_head] = (float)s->value;
                s->hist_head = (uint8_t)((s->hist_head + 1) % CN_HIST);
                if (s->hist_n < CN_HIST) s->hist_n++;
            }
        }
        unlock(e);
    }
    /* trip tick: 2 Hz */
    cn_trip *t = &e->trip;
    if (t->active && now - t->last_tick_ms >= TICK_MS) {
        double dt = (now - t->last_tick_ms) / 1000.0;
        t->last_tick_ms = now;
        lock(e);
        build_vars(e, now);
        double tt = (now - t->start_mono) / 1000.0;
        cn_monitors_tick(p, e->vars, tt, dt);
        if (e->speed_sig >= 0 && !isnan(e->vars[e->speed_sig])) {
            double sp = e->vars[e->speed_sig];
            t->distance_km += sp * dt / 3600.0;
            if (sp > t->max_speed) t->max_speed = sp;
        }
        if (e->fuel_rate_sig >= 0 && !isnan(e->vars[e->fuel_rate_sig])) t->fuel_l += e->vars[e->fuel_rate_sig] * dt / 3600.0;
        if (now - e->last_eval_ms >= 5000) {
            e->last_eval_ms = now;
            cn_monitors_evaluate(p, tt);
        }
        unlock(e);
    }
    if (now - e->last_retention_ms > 30 * 60 * 1000 || !e->last_retention_ms) {
        e->last_retention_ms = now ? now : 1;
        cn_trips_retention(e);
    }
    if (now - e->last_state_save_ms > 10 * 60 * 1000) save_state(e);
}

void cn_vehicle_step(cn_engine *e)
{
    int64_t now = now_ms(e);
    cn_profile *p = e->profile;
    periodic(e, now);
    if (e->profile_change_pending) {
        e->profile_change_pending = false;
        cn_engine_reload_profile(e);
        p = e->profile;
        if (e->vs == CN_VS_RUN || e->vs == CN_VS_KOEO) set_state(e, CN_VS_IDENT);
        return;
    }
    if (e->adhoc.pending) {
        run_adhoc(e);
        return;
    }
    switch (e->vs) {
    case CN_VS_INIT:
        set_state(e, CN_VS_DETECT);
        break;

    case CN_VS_DETECT:
        e->last_probe_ms = now;
        if (detect(e)) {
            cn_log(e, 'i', "Vehicle answered: CAN %d kbit/s, %d-bit identifiers", e->can_bitrate / 1000, e->can_ext ? 29 : 11);
            e->detect_fails = 0;
            e->last_response_ms = now_ms(e);
            set_state(e, CN_VS_IDENT);
        } else {
            e->detect_fails++;
            if (e->detect_fails == 1) cn_log(e, 'i', "No answer from the vehicle (ignition off?)");
            go_off(e, "no answer");
        }
        break;

    case CN_VS_IDENT:
        read_pid_support(e);
        if (!e->vin_tried || !e->vin[0]) {
            read_vin(e);
            e->vin_tried = true;
        }
        cn_engine_reload_profile(e);
        p = e->profile;
        lock(e);
        apply_pid_support(e);
        load_support(e);
        recompute_needed(e);
        for (int i = 0; i < p->nrequests; i++) p->requests[i].next_ms = 0;
        unlock(e);
        if (p->can_bitrate && p->can_bitrate != e->can_bitrate) {
            /* the selected profile pins a different bit rate than detection found: detect again */
            set_state(e, CN_VS_DETECT);
            break;
        }
        e->last_dtc_ms = 0;
        e->dtc_phase = 0;
        save_state(e);
        set_state(e, CN_VS_KOEO);
        break;

    case CN_VS_RUN:
    case CN_VS_KOEO: {
        bool ui_active = now - e->last_ui_ms < 60000;
        bool heartbeat_only = e->vs == CN_VS_KOEO && !ui_active && now - e->vs_since_ms > KOEO_FULL_MS;
        if (heartbeat_only && !e->heartbeat_logged) {
            e->heartbeat_logged = true;
            cn_log(e, 'i', "Ignition on, engine off: polling reduced to a heartbeat");
        }
        if (e->dtc_phase >= 0 && !heartbeat_only)
            dtc_step(e);
        else
            poll_one(e, now, heartbeat_only);
        now = now_ms(e);
        if (e->dtc_phase < 0 && !heartbeat_only && now - e->last_dtc_ms > DTC_INTERVAL_MS) e->dtc_phase = 0;

        /* has the car gone quiet? */
        if (e->consecutive_timeouts >= 6 && now - e->last_response_ms > 3000) {
            go_off(e, "ignition off");
            break;
        }
        double rpm = NAN;
        if (e->rpm_sig >= 0) {
            cn_signal *s = &p->signals[e->rpm_sig];
            if (s->valid && now - s->t_ms < 3000) rpm = s->value;
        }
        if (e->vs == CN_VS_KOEO) {
            if (rpm > 300) {
                set_state(e, CN_VS_RUN);
                e->heartbeat_logged = false;
                lock(e);
                for (int i = 0; i < p->nrequests; i++)
                    if (p->requests[i].rate >= CN_RATE_RARE) p->requests[i].next_ms = 0;
                unlock(e);
                cn_trip_begin(e);
                e->trip.rpm_zero_since = 0;
                if (e->dtc_read_wall || e->last_dtc_ms) log_dtc_event(e);
                e->last_dtc_ms = now - DTC_INTERVAL_MS + 45000; /* re-read codes once the engine has settled */
            } else if (!ui_active && now - e->vs_since_ms > KOEO_MAX_MS) {
                go_off(e, "ignition left on");
            }
        } else {
            cn_trip *t = &e->trip;
            if (rpm < 50) {
                if (!t->rpm_zero_since) {
                    t->rpm_zero_since = now;
                    /* engine just stopped: grab the slow values (odometer, fuel level) while the car still answers */
                    lock(e);
                    for (int i = 0; i < p->nrequests; i++)
                        if (p->requests[i].rate >= CN_RATE_RARE && p->requests[i].rate != CN_RATE_ONCE) p->requests[i].next_ms = 0;
                    unlock(e);
                } else if (now - t->rpm_zero_since > STOP_GRACE_MS) {
                    cn_trip_end(e, "engine off");
                    set_state(e, CN_VS_KOEO);
                }
            } else if (!isnan(rpm)) {
                t->rpm_zero_since = 0;
            }
        }
        if (e->support_dirty && e->probe_pending == 0) {
            save_support(e);
            e->support_dirty = false;
            int ok = 0;
            for (int i = 0; i < p->nrequests; i++)
                if (p->requests[i].sup == CN_SUP_OK) ok++;
            cn_log(e, 'i', "Finished learning this car: %d of %d requests answered", ok, p->nrequests);
        }
        break;
    }

    case CN_VS_OFF:
    default: {
        e->pf.delay_ms(e->pf.ctx, 200);
        bool wake = e->wake_request;
        if (!wake && now - e->last_probe_ms > 20000) {
            if (e->has_vbat) {
                if (e->wake_armed && cn_power_should_wake(&e->cfg, e->vbat, e->vbat_min_off)) wake = true;
                /* voltage says "charging" but the car did not answer last time: try again, rarely */
                if (!wake && !isnan(e->vbat) && e->vbat >= e->cfg.wake_voltage && now - e->last_probe_ms > 4 * 60 * 1000) wake = true;
            } else if (now - e->last_probe_ms > 30000) {
                wake = true; /* no voltage sensing on this board: look for the car now and then */
            }
        }
        if (wake) {
            e->wake_request = false;
            e->wake_armed = false;
            set_state(e, CN_VS_DETECT);
        }
        break;
    }
    }
}
