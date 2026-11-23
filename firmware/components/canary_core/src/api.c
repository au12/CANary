/* JSON API behind the on-device web interface. Platform independent: the
 * firmware and the desktop simulator both route HTTP requests through
 * cn_api_handle. SPDX-License-Identifier: GPL-3.0-or-later */
#include "canary/core.h"

#include "cJSON.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int64_t now_ms(cn_engine *e) { return e->pf.mono_ms(e->pf.ctx); }
static void lock(cn_engine *e) { if (e->pf.lock) e->pf.lock(e->pf.ctx); }
static void unlock(cn_engine *e) { if (e->pf.unlock) e->pf.unlock(e->pf.ctx); }

static void fail(cn_api_resp *r, int status, const char *msg)
{
    r->status = status;
    r->body.len = 0;
    cn_jw w;
    cn_jw_init(&w, &r->body);
    cn_jw_obj(&w);
    cn_jw_kbool(&w, "ok", false);
    cn_jw_kstr(&w, "error", msg);
    cn_jw_end_obj(&w);
}

static void ok_only(cn_api_resp *r) { cn_buf_puts(&r->body, "{\"ok\":true}"); }

static const char *src_text(cn_engine *e, const cn_signal *s, char *buf, size_t n)
{
    if (s->active < 0) return "none";
    const cn_source *src = &s->src[s->active];
    switch (src->kind) {
    case CN_SRC_OBD: snprintf(buf, n, "OBD-II PID %02X", src->code); break;
    case CN_SRC_UDS: snprintf(buf, n, "%s %04X", e->profile->ecus[src->ecu].name, src->code); break;
    case CN_SRC_DEVICE: snprintf(buf, n, "CANary"); break;
    default: snprintf(buf, n, "calculated");
    }
    return buf;
}

static void api_status(cn_engine *e, cn_api_resp *r)
{
    int64_t now = now_ms(e), wall = e->pf.wall_s(e->pf.ctx);
    char why[48];
    bool can_sleep = cn_power_can_sleep(e, why, sizeof(why));
    uint64_t total = 0, freeb = 0;
    bool storage = e->pf.data_dir && e->pf.data_dir(e->pf.ctx) && e->pf.storage_info &&
                   e->pf.storage_info(e->pf.ctx, &total, &freeb) == 0;
    cn_jw w;
    cn_jw_init(&w, &r->body);
    lock(e);
    cn_profile *p = e->profile;
    cn_jw_obj(&w);
    cn_jw_key(&w, "device");
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "id", e->device_id);
    cn_jw_kstr(&w, "name", e->cfg.device_name);
    cn_jw_kstr(&w, "fw", CN_FW_VERSION);
    cn_jw_kstr(&w, "board", e->board);
    cn_jw_kint(&w, "uptime_s", (now - e->boot_mono) / 1000);
    cn_jw_kint(&w, "time", wall);
    cn_jw_kbool(&w, "time_set", wall > 1700000000);
    cn_jw_kint(&w, "tz_offset_min", e->cfg.tz_offset_min);
    cn_jw_end_obj(&w);

    int supported = 0;
    for (int i = 0; i < p->nrequests; i++)
        if (p->requests[i].sup == CN_SUP_OK) supported++;
    int live = 0;
    for (int i = 0; i < p->nsignals; i++)
        if (p->signals[i].active >= 0) live++;
    cn_jw_key(&w, "vehicle");
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "state", cn_vs_name(e->vs));
    cn_jw_kint(&w, "state_for_s", (now - e->vs_since_ms) / 1000);
    cn_jw_kstr(&w, "vin", e->vin);
    cn_jw_key(&w, "profile");
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "id", p->id);
    cn_jw_kstr(&w, "name", p->name);
    cn_jw_kstr(&w, "hash", p->hash);
    cn_jw_end_obj(&w);
    cn_jw_kint(&w, "can_bitrate", e->can_bitrate);
    cn_jw_kint(&w, "can_id_bits", e->can_ext ? 29 : 11);
    cn_jw_kint(&w, "learning", e->probe_pending);
    cn_jw_kint(&w, "requests_supported", supported);
    cn_jw_kint(&w, "requests_total", p->nrequests);
    cn_jw_kint(&w, "signals_live", live);
    cn_jw_kint(&w, "signals_total", p->nsignals);
    cn_jw_kint(&w, "bus_tx", e->bus_tx);
    cn_jw_kint(&w, "bus_rx", e->bus_rx);
    cn_jw_kint(&w, "bus_err", e->bus_err);
    cn_jw_end_obj(&w);

    cn_jw_knum(&w, "battery_v", e->vbat, 2);
    cn_jw_knum(&w, "odometer_km", e->last_odo_km > 0 ? e->last_odo_km : NAN, 1);
    cn_jw_kint(&w, "odometer_time", e->last_odo_wall);
    cn_jw_kbool(&w, "units_imperial", e->cfg.units_imperial);

    cn_jw_key(&w, "trip");
    cn_jw_obj(&w);
    cn_jw_kbool(&w, "active", e->trip.active);
    if (e->trip.active) {
        cn_jw_kstr(&w, "id", e->trip.id);
        cn_jw_kint(&w, "duration_s", (now - e->trip.start_mono) / 1000);
        cn_jw_knum(&w, "distance_km", e->trip.distance_km, 2);
        cn_jw_kint(&w, "samples", e->trip.samples);
        cn_jw_kint(&w, "bytes", (long long)e->trip.bytes);
        cn_jw_kbool(&w, "recording", e->trip.f != NULL);
    }
    cn_jw_kstr(&w, "last_id", e->last_trip_id);
    cn_jw_kint(&w, "count", e->ntrips);
    cn_jw_end_obj(&w);

    int above = 0;
    int worst = cn_worst_severity(e, &above);
    int assessed = 0;
    int available = 0;
    for (int i = 0; i < p->nmonitors; i++) {
        if (p->monitors[i].enabled && p->monitors[i].status != CN_SEV_NODATA) assessed++;
        if (p->monitors[i].enabled && !p->monitors[i].unavailable) available++;
    }
    cn_jw_key(&w, "health");
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "worst", cn_sev_name(worst));
    cn_jw_kint(&w, "attention", above);
    cn_jw_kint(&w, "assessed", assessed);
    cn_jw_kint(&w, "monitors", available);
    cn_jw_kbool(&w, "mil", e->mil);
    cn_jw_kint(&w, "dtc_count", e->ndtcs);
    cn_jw_kint(&w, "dtc_read", e->dtc_read_wall);
    cn_jw_end_obj(&w);

    cn_jw_key(&w, "net");
    cn_jw_obj(&w);
    cn_jw_kbool(&w, "connected", e->net.connected);
    cn_jw_kstr(&w, "ssid", e->net.ssid);
    cn_jw_kstr(&w, "ip", e->net.ip);
    cn_jw_kint(&w, "rssi", e->net.rssi);
    cn_jw_kbool(&w, "ap_active", e->net.ap_active);
    cn_jw_kstr(&w, "ap_ssid", e->net.ap_ssid);
    cn_jw_end_obj(&w);

    static const char *const modes[] = {"none", "lubelogger", "canary"};
    cn_jw_key(&w, "sync");
    cn_jw_obj(&w);
    cn_jw_kbool(&w, "configured", e->cfg.gb_url[0] && e->cfg.gb_api_key[0] && e->cfg.gb_vehicle_id > 0);
    cn_jw_kstr(&w, "mode", modes[e->sync.mode]);
    cn_jw_kbool(&w, "busy", e->sync.busy);
    cn_jw_kint(&w, "last_ok", e->sync.last_ok_wall);
    cn_jw_kstr(&w, "last_error", e->sync.last_error);
    cn_jw_kint(&w, "pending_trips", e->sync.pending_trips);
    cn_jw_kstr(&w, "uploading", e->sync.uploading);
    cn_jw_kint(&w, "up_done", (long long)e->sync.up_done);
    cn_jw_kint(&w, "up_total", (long long)e->sync.up_total);
    cn_jw_knum(&w, "odometer_sent_km", e->sync.last_odo_sent_km > 0 ? e->sync.last_odo_sent_km : NAN, 1);
    cn_jw_kint(&w, "odometer_sent_at", e->sync.last_odo_sent_wall);
    cn_jw_end_obj(&w);

    cn_jw_key(&w, "storage");
    cn_jw_obj(&w);
    cn_jw_kbool(&w, "present", storage);
    cn_jw_kint(&w, "total", (long long)total);
    cn_jw_kint(&w, "free", (long long)freeb);
    cn_jw_kbool(&w, "logging", e->cfg.log_enabled);
    cn_jw_kint(&w, "retention_days", e->cfg.retention_days);
    cn_jw_end_obj(&w);

    cn_jw_key(&w, "power");
    cn_jw_obj(&w);
    cn_jw_kbool(&w, "sleep_enabled", e->cfg.sleep_enabled);
    cn_jw_kbool(&w, "can_sleep", can_sleep);
    cn_jw_kstr(&w, "why", why);
    cn_jw_end_obj(&w);
    cn_jw_end_obj(&w);
    unlock(e);
}

static void api_signals(cn_engine *e, const cn_api_req *req, cn_api_resp *r)
{
    char tmp[8];
    bool hist = cn_query_get(req->query, "hist", tmp, sizeof(tmp)) && tmp[0] == '1';
    int64_t now = now_ms(e);
    cn_jw w;
    cn_jw_init(&w, &r->body);
    lock(e);
    cn_profile *p = e->profile;
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "profile", p->id);
    cn_jw_kint(&w, "hist_step_ms", CN_HIST_STEP_MS);
    cn_jw_key(&w, "signals");
    cn_jw_arr(&w);
    for (int i = 0; i < p->nsignals; i++) {
        cn_signal *s = &p->signals[i];
        char src[48];
        int stale = cn_rate_stale_ms[s->rate];
        bool fresh = s->valid && (stale == 0 || now - s->t_ms <= stale);
        cn_jw_obj(&w);
        cn_jw_kstr(&w, "id", s->id);
        cn_jw_kstr(&w, "name", s->name);
        cn_jw_kstr(&w, "unit", s->unit);
        cn_jw_kstr(&w, "group", s->group);
        cn_jw_kint(&w, "dec", s->dec);
        cn_jw_kstr(&w, "rate", cn_rate_name(s->rate));
        cn_jw_kstr(&w, "source", src_text(e, s, src, sizeof(src)));
        cn_jw_kbool(&w, "available", s->active >= 0);
        cn_jw_knum(&w, "v", fresh ? s->value : NAN, s->dec + 1);
        if (s->n) {
            cn_jw_knum(&w, "min", s->vmin, s->dec + 1);
            cn_jw_knum(&w, "max", s->vmax, s->dec + 1);
            cn_jw_knum(&w, "avg", s->sum / s->n, s->dec + 1);
        }
        if (hist) {
            cn_jw_key(&w, "hist");
            cn_jw_arr(&w);
            for (int k = 0; k < s->hist_n; k++) {
                int idx = (s->hist_head - s->hist_n + k + CN_HIST * 2) % CN_HIST;
                cn_jw_num(&w, s->hist[idx], s->dec + 1);
            }
            cn_jw_end_arr(&w);
        }
        cn_jw_end_obj(&w);
    }
    cn_jw_end_arr(&w);
    cn_jw_end_obj(&w);
    unlock(e);
}

static void api_live(cn_engine *e, cn_api_resp *r)
{
    int64_t now = now_ms(e);
    cn_jw w;
    cn_jw_init(&w, &r->body);
    lock(e);
    cn_profile *p = e->profile;
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "state", cn_vs_name(e->vs));
    cn_jw_kstr(&w, "profile", p->hash);
    cn_jw_key(&w, "v");
    cn_jw_obj(&w);
    for (int i = 0; i < p->nsignals; i++) {
        cn_signal *s = &p->signals[i];
        int stale = cn_rate_stale_ms[s->rate];
        if (!s->valid || (stale && now - s->t_ms > stale)) continue;
        cn_jw_knum(&w, s->id, s->value, s->dec + 1);
    }
    cn_jw_end_obj(&w);
    cn_jw_end_obj(&w);
    unlock(e);
}

static void api_findings(cn_engine *e, cn_api_resp *r)
{
    static const char *const ops[] = {"gt", "ge", "lt", "le"};
    cn_jw w;
    cn_jw_init(&w, &r->body);
    lock(e);
    cn_profile *p = e->profile;
    cn_jw_obj(&w);
    cn_jw_kbool(&w, "trip_active", e->trip.active);
    cn_jw_kstr(&w, "trip", e->trip.active ? e->trip.id : e->last_trip_id);
    cn_jw_kstr(&w, "worst", cn_sev_name(cn_worst_severity(e, NULL)));
    cn_jw_key(&w, "monitors");
    cn_jw_arr(&w);
    for (int i = 0; i < p->nmonitors; i++) {
        cn_monitor *m = &p->monitors[i];
        char text[220];
        cn_monitor_text(m, text, sizeof(text));
        cn_jw_obj(&w);
        cn_jw_kstr(&w, "id", m->id);
        cn_jw_kstr(&w, "title", m->title);
        cn_jw_kstr(&w, "category", m->category);
        bool off = !m->enabled || (m->unavailable && m->status == CN_SEV_NODATA);
        cn_jw_kstr(&w, "status", off ? "unavailable" : cn_sev_name(m->status));
        cn_jw_knum(&w, "value", m->has_result ? m->result : NAN, m->dec + 1);
        cn_jw_kint(&w, "dec", m->dec);
        cn_jw_kstr(&w, "unit", m->unit);
        cn_jw_kstr(&w, "text", text);
        cn_jw_kstr(&w, "stat", cn_stat_name(m->stat));
        cn_jw_knum(&w, "seen_s", m->n * 0.5, 0);
        cn_jw_knum(&w, "need_s", m->min_time_s, 0);
        if (m->detail) cn_jw_kstr(&w, "detail", m->detail);
        if (!m->enabled)
            cn_jw_kstr(&w, "why", m->why_disabled);
        else if (m->unavailable)
            cn_jw_kstr(&w, "why", m->missing);
        cn_jw_key(&w, "causes");
        cn_jw_arr(&w);
        for (int c = 0; c < m->ncauses; c++) cn_jw_str(&w, m->causes[c]);
        cn_jw_end_arr(&w);
        cn_jw_key(&w, "levels");
        cn_jw_arr(&w);
        for (int l = 0; l < m->nlevels; l++) {
            cn_jw_obj(&w);
            cn_jw_kstr(&w, "severity", cn_sev_name(m->levels[l].sev));
            cn_jw_kstr(&w, "op", ops[m->levels[l].op]);
            cn_jw_knum(&w, "value", m->levels[l].thr, -1);
            cn_jw_end_obj(&w);
        }
        cn_jw_end_arr(&w);
        cn_jw_end_obj(&w);
    }
    cn_jw_end_arr(&w);
    cn_jw_end_obj(&w);
    unlock(e);
}

static void api_dtcs(cn_engine *e, cn_api_resp *r)
{
    static const char *const kinds[] = {"", "stored", "pending", "permanent"};
    cn_jw w;
    cn_jw_init(&w, &r->body);
    lock(e);
    cn_profile *p = e->profile;
    cn_jw_obj(&w);
    cn_jw_kint(&w, "read_at", e->dtc_read_wall);
    cn_jw_kbool(&w, "mil", e->mil);
    cn_jw_kstr(&w, "severity", cn_sev_name(cn_dtc_severity(e)));
    cn_jw_key(&w, "codes");
    cn_jw_arr(&w);
    for (int i = 0; i < e->ndtcs; i++) {
        const cn_dtc *d = &e->dtcs[i];
        char src[16];
        snprintf(src, sizeof(src), "%X", (unsigned)d->src);
        cn_jw_obj(&w);
        cn_jw_kstr(&w, "code", d->code);
        cn_jw_kstr(&w, "kind", kinds[d->kind <= 3 ? d->kind : 0]);
        cn_jw_kstr(&w, "ecu", d->ecu != 0xFF && d->ecu < p->necus ? p->ecus[d->ecu].name : "Engine (OBD-II)");
        cn_jw_kstr(&w, "src", src);
        cn_jw_kint(&w, "ftb", d->ftb);
        cn_jw_kint(&w, "status", d->status);
        cn_jw_end_obj(&w);
    }
    cn_jw_end_arr(&w);
    cn_jw_key(&w, "modules");
    cn_jw_arr(&w);
    cn_jw_str(&w, "Engine (OBD-II)");
    for (int i = 1; i < p->necus; i++)
        if (p->ecus[i].dtc) cn_jw_str(&w, p->ecus[i].name);
    cn_jw_end_arr(&w);
    cn_jw_end_obj(&w);
    unlock(e);
}

static void trip_info_json(cn_jw *w, const cn_trip_info *ti)
{
    cn_jw_obj(w);
    cn_jw_kstr(w, "id", ti->id);
    cn_jw_kint(w, "start", ti->start_wall);
    cn_jw_kint(w, "duration_s", ti->duration_s);
    cn_jw_knum(w, "distance_km", ti->distance_km, 2);
    cn_jw_knum(w, "max_speed", ti->max_speed, 0);
    cn_jw_knum(w, "odo_end_km", ti->odo_end_km, 1);
    cn_jw_kint(w, "samples", ti->samples);
    cn_jw_kint(w, "bytes", (long long)ti->bytes);
    cn_jw_kint(w, "uploaded_bytes", (long long)ti->uploaded_bytes);
    cn_jw_kint(w, "uploaded_at", ti->uploaded_at);
    cn_jw_kbool(w, "raw", !ti->raw_deleted && ti->bytes > 0);
    cn_jw_kstr(w, "worst", cn_sev_name(ti->worst));
    cn_jw_kint(w, "findings", ti->nfindings);
    cn_jw_end_obj(w);
}

static void api_trips(cn_engine *e, cn_api_resp *r)
{
    cn_jw w;
    cn_jw_init(&w, &r->body);
    lock(e);
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "active", e->trip.active ? e->trip.id : "");
    cn_jw_kint(&w, "retention_days", e->cfg.retention_days);
    cn_jw_key(&w, "trips");
    cn_jw_arr(&w);
    for (int i = 0; i < e->ntrips; i++) trip_info_json(&w, &e->trips[i]);
    cn_jw_end_arr(&w);
    cn_jw_end_obj(&w);
    unlock(e);
}

/* Downsampled series straight from the raw log, so charts never need the whole file in the browser. */
#define MAX_SERIES 6
static void api_series(cn_engine *e, const char *id, const cn_api_req *req, cn_api_resp *r)
{
    char sigs[160] = "", tmp[16];
    cn_query_get(req->query, "sig", sigs, sizeof(sigs));
    int points = 300;
    if (cn_query_get(req->query, "points", tmp, sizeof(tmp))) points = atoi(tmp);
    if (points < 20) points = 20;
    if (points > 800) points = 800;
    char *want[MAX_SERIES];
    int nwant = 0;
    for (char *tok = strtok(sigs, ","); tok && nwant < MAX_SERIES; tok = strtok(NULL, ",")) want[nwant++] = tok;
    if (!nwant) {
        fail(r, 400, "no signals requested");
        return;
    }
    double dur_s = 0;
    lock(e);
    if (e->trip.active && !strcmp(e->trip.id, id)) {
        dur_s = (now_ms(e) - e->trip.start_mono) / 1000.0;
        if (e->trip.f) fflush(e->trip.f);
    }
    for (int i = 0; i < e->ntrips && dur_s == 0; i++)
        if (!strcmp(e->trips[i].id, id)) dur_s = e->trips[i].duration_s;
    unlock(e);
    char *path = cn_trip_path(e, id, "cnl");
    FILE *f = path ? fopen(path, "r") : NULL;
    free(path);
    if (!f) {
        fail(r, 404, "raw data for this trip is not on the device");
        return;
    }
    if (dur_s < 1) dur_s = 1;
    double bucket_ms = dur_s * 1000.0 / points;
    typedef struct {
        float sum, lo, hi;
        uint16_t n;
    } cell;
    cell *cells = calloc((size_t)nwant * (size_t)points, sizeof(cell));
    int idx_of[MAX_SERIES];
    char units[MAX_SERIES][12], names[MAX_SERIES][52];
    for (int i = 0; i < MAX_SERIES; i++) {
        idx_of[i] = -1;
        units[i][0] = names[i][0] = 0;
    }
    if (!cells) {
        fclose(f);
        fail(r, 500, "out of memory");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            int idx;
            char sid[CN_ID_LEN], unit[12], rate[12];
            int consumed = 0;
            if (sscanf(line, "#sig %d %27s %11s %11s %n", &idx, sid, unit, rate, &consumed) >= 4) {
                for (int k = 0; k < nwant; k++)
                    if (!strcmp(want[k], sid)) {
                        idx_of[k] = idx;
                        cn_strlcpy(units[k], strcmp(unit, "-") ? unit : "", sizeof(units[k]));
                        char *nm = line + consumed;
                        nm[strcspn(nm, "\r\n")] = 0;
                        cn_strlcpy(names[k], nm, sizeof(names[k]));
                    }
            }
            continue;
        }
        if (line[0] == '!' || line[0] < '0' || line[0] > '9') continue;
        char *end;
        long t = strtol(line, &end, 10);
        long idx = strtol(end, &end, 10);
        for (int k = 0; k < nwant; k++) {
            if (idx_of[k] != idx) continue;
            double v = strtod(end, NULL);
            int b = (int)(t / bucket_ms);
            if (b < 0) b = 0;
            if (b >= points) b = points - 1;
            cell *c = &cells[k * points + b];
            if (!c->n || v < c->lo) c->lo = (float)v;
            if (!c->n || v > c->hi) c->hi = (float)v;
            c->sum += (float)v;
            if (c->n < 65535) c->n++;
        }
    }
    fclose(f);
    cn_jw w;
    cn_jw_init(&w, &r->body);
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "id", id);
    cn_jw_knum(&w, "duration_s", dur_s, 0);
    cn_jw_key(&w, "series");
    cn_jw_arr(&w);
    for (int k = 0; k < nwant; k++) {
        cn_jw_obj(&w);
        cn_jw_kstr(&w, "id", want[k]);
        cn_jw_kstr(&w, "name", names[k][0] ? names[k] : want[k]);
        cn_jw_kstr(&w, "unit", units[k]);
        cn_jw_kbool(&w, "found", idx_of[k] >= 0);
        static const char *const keys[] = {"t", "v", "lo", "hi"};
        for (int part = 0; part < 4; part++) {
            cn_jw_key(&w, keys[part]);
            cn_jw_arr(&w);
            for (int b = 0; b < points; b++) {
                cell *c = &cells[k * points + b];
                if (!c->n) continue;
                if (part == 0)
                    cn_jw_num(&w, (b + 0.5) * bucket_ms / 1000.0, 1);
                else
                    cn_jw_num(&w, part == 1 ? c->sum / c->n : part == 2 ? c->lo : c->hi, -1);
            }
            cn_jw_end_arr(&w);
        }
        cn_jw_end_obj(&w);
    }
    cn_jw_end_arr(&w);
    cn_jw_end_obj(&w);
    free(cells);
}

static void api_config_get(cn_engine *e, cn_api_resp *r)
{
    lock(e);
    char *cfg = cn_config_to_json(&e->cfg, true);
    unlock(e);
    char *index = e->loader(e->loader_ctx, CN_PROFILE_INDEX);
    cn_jw w;
    cn_jw_init(&w, &r->body);
    cn_jw_obj(&w);
    cn_jw_key(&w, "config");
    cn_jw_raw(&w, cfg);
    cn_jw_key(&w, "profiles");
    cn_jw_raw(&w, index ? index : "[]");
    cn_jw_kstr(&w, "active_profile", e->profile->id);
    cn_jw_end_obj(&w);
    free(cfg);
    free(index);
}

static void api_config_post(cn_engine *e, const cn_api_req *req, cn_api_resp *r)
{
    if (!req->body) {
        fail(r, 400, "empty request");
        return;
    }
    lock(e);
    char old_profile[40];
    cn_strlcpy(old_profile, e->cfg.profile, sizeof(old_profile));
    bool ok = cn_config_from_json(&e->cfg, req->body);
    if (ok) {
        e->cfg_gen++;
        e->sync.force = true;
        e->sync.did_connect_sync = false;
        e->sync.last_error[0] = 0;
    }
    bool profile_changed = strcmp(old_profile, e->cfg.profile) != 0;
    unlock(e);
    if (!ok) {
        fail(r, 400, "settings could not be read");
        return;
    }
    cn_engine_save_config(e);
    if (profile_changed) e->profile_change_pending = true; /* applied by the vehicle task */
    cn_log(e, 'i', "Settings saved");
    api_config_get(e, r);
}

static void api_events(cn_engine *e, cn_api_resp *r)
{
    cn_jw w;
    cn_jw_init(&w, &r->body);
    lock(e);
    cn_jw_obj(&w);
    cn_jw_key(&w, "events");
    cn_jw_arr(&w);
    for (int k = 0; k < e->ev_count; k++) {
        const cn_event *ev = &e->events[(e->ev_head - 1 - k + CN_EVENTS * 2) % CN_EVENTS];
        char lvl[2] = {ev->level, 0};
        cn_jw_obj(&w);
        cn_jw_kint(&w, "t", ev->t_wall);
        cn_jw_kint(&w, "up", ev->t_mono / 1000);
        cn_jw_kstr(&w, "level", lvl);
        cn_jw_kstr(&w, "text", ev->text);
        cn_jw_end_obj(&w);
    }
    cn_jw_end_arr(&w);
    cn_jw_end_obj(&w);
    unlock(e);
}

static void api_support(cn_engine *e, cn_api_resp *r)
{
    static const char *const sup[] = {"unknown", "yes", "no"};
    cn_jw w;
    cn_jw_init(&w, &r->body);
    lock(e);
    cn_profile *p = e->profile;
    cn_jw_obj(&w);
    cn_jw_kint(&w, "learning", e->probe_pending);
    cn_jw_key(&w, "requests");
    cn_jw_arr(&w);
    for (int i = 0; i < p->nrequests; i++) {
        cn_request *q = &p->requests[i];
        char code[12];
        snprintf(code, sizeof(code), q->mode == 0x22 ? "%04X" : "%02X", q->code);
        cn_jw_obj(&w);
        cn_jw_kstr(&w, "ecu", p->ecus[q->ecu].name);
        cn_jw_kstr(&w, "service", q->mode == 0x22 ? "UDS 22" : "OBD 01");
        cn_jw_kstr(&w, "code", code);
        cn_jw_kstr(&w, "rate", cn_rate_name(q->rate));
        cn_jw_kstr(&w, "supported", sup[q->sup]);
        cn_jw_kbool(&w, "in_use", q->needed && q->sup == CN_SUP_OK);
        cn_jw_kint(&w, "ok", q->ok_count);
        cn_jw_kint(&w, "errors", q->err_count);
        cn_jw_key(&w, "signals");
        cn_jw_arr(&w);
        for (int s = 0; s < p->nsignals; s++)
            for (int k = 0; k < p->signals[s].nsrc; k++)
                if (p->signals[s].src[k].req == i) cn_jw_str(&w, p->signals[s].id);
        cn_jw_end_arr(&w);
        cn_jw_end_obj(&w);
    }
    cn_jw_end_arr(&w);
    cn_jw_end_obj(&w);
    unlock(e);
}

/* Send one read-only request typed by the user: a tool for building profiles for new cars. */
static void api_diag(cn_engine *e, const cn_api_req *req, cn_api_resp *r)
{
    cJSON *d = cJSON_Parse(req->body ? req->body : "");
    const cJSON *tx = cJSON_GetObjectItemCaseSensitive(d, "tx"), *rx = cJSON_GetObjectItemCaseSensitive(d, "rx"),
                *data = cJSON_GetObjectItemCaseSensitive(d, "data");
    cn_adhoc a;
    memset(&a, 0, sizeof(a));
    bool ok1 = false, ok2 = true;
    if (cJSON_IsString(tx)) a.addr.tx = cn_parse_u32(tx->valuestring, 16, &ok1);
    if (cJSON_IsString(rx) && rx->valuestring[0]) a.addr.rx = cn_parse_u32(rx->valuestring, 16, &ok2);
    a.req_len = cJSON_IsString(data) ? cn_parse_hex(data->valuestring, a.req, 7) : -1;
    cJSON_Delete(d);
    if (!ok1 || !ok2 || a.req_len < 1) {
        fail(r, 400, "need tx (hex id) and data (1-7 hex bytes)");
        return;
    }
    if (!cn_diag_sid_allowed(a.req[0])) {
        fail(r, 403, "CANary is read-only: that service could change something in the car");
        return;
    }
    a.addr.ext = a.addr.tx > 0x7FF;
    a.pending = true;
    lock(e);
    bool busy = e->adhoc.pending;
    if (!busy) e->adhoc = a;
    unlock(e);
    if (busy) {
        fail(r, 409, "another request is in progress");
        return;
    }
    for (int i = 0; i < 300; i++) { /* the vehicle task picks it up */
        e->pf.delay_ms(e->pf.ctx, 10);
        lock(e);
        bool done = e->adhoc.done;
        if (done) a = e->adhoc;
        unlock(e);
        if (done) break;
    }
    cn_jw w;
    cn_jw_init(&w, &r->body);
    cn_jw_obj(&w);
    if (!a.done) {
        lock(e);
        e->adhoc.pending = false;
        unlock(e);
        cn_jw_kbool(&w, "ok", false);
        cn_jw_kstr(&w, "error", "the vehicle task did not respond");
    } else if (a.result > 0) {
        cn_buf hex;
        cn_buf_init(&hex);
        for (int i = 0; i < a.result; i++) cn_buf_printf(&hex, "%02X", a.resp[i]);
        char id[12];
        snprintf(id, sizeof(id), "%X", (unsigned)a.resp_id);
        cn_jw_kbool(&w, "ok", true);
        cn_jw_kstr(&w, "from", id);
        cn_jw_kstr(&w, "data", hex.p ? hex.p : "");
        cn_jw_kint(&w, "ms", a.elapsed_ms);
        cn_buf_free(&hex);
    } else {
        char msg[48];
        if (CN_IS_NRC(a.result))
            snprintf(msg, sizeof(msg), "refused by the control unit (NRC 0x%02X)", CN_NRC_OF(a.result));
        else
            snprintf(msg, sizeof(msg), "%s", a.result == CN_ERR_TIMEOUT ? "no answer" : a.result == CN_ERR_BUS ? "CAN bus error" : "bad reply");
        cn_jw_kbool(&w, "ok", false);
        cn_jw_kstr(&w, "error", msg);
        cn_jw_kint(&w, "ms", a.elapsed_ms);
    }
    cn_jw_end_obj(&w);
}

static void api_time(cn_engine *e, const cn_api_req *req, cn_api_resp *r)
{
    cJSON *d = cJSON_Parse(req->body ? req->body : "");
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(d, "unix"), *tz = cJSON_GetObjectItemCaseSensitive(d, "tz_offset_min");
    bool tz_changed = false;
    if (cJSON_IsNumber(tz) && fabs(tz->valuedouble) <= 900) {
        lock(e);
        tz_changed = e->cfg.tz_offset_min != (int)tz->valuedouble;
        e->cfg.tz_offset_min = (int)tz->valuedouble;
        unlock(e);
    }
    if (cJSON_IsNumber(u) && u->valuedouble > 1700000000 && e->pf.set_wall_s) {
        int64_t wall = e->pf.wall_s(e->pf.ctx), browser = (int64_t)u->valuedouble;
        /* a browser clock is good enough to start with; a server clock will refine it */
        if (wall < 1700000000) {
            e->pf.set_wall_s(e->pf.ctx, browser);
            cn_log(e, 'i', "Clock set from the browser");
        }
    }
    cJSON_Delete(d);
    if (tz_changed) cn_engine_save_config(e);
    ok_only(r);
}

bool cn_api_handle(cn_engine *e, const cn_api_req *req, cn_api_resp *r)
{
    const char *p = req->path;
    if (strncmp(p, "/api/", 5) != 0) return false;
    p += 5;
    bool get = !strcmp(req->method, "GET"), post = !strcmp(req->method, "POST");
    r->status = 200;
    r->content_type = "application/json";
    e->last_ui_ms = now_ms(e);

    if (get && !strcmp(p, "status")) {
        api_status(e, r);
    } else if (get && !strcmp(p, "live")) {
        api_live(e, r);
    } else if (get && !strcmp(p, "signals")) {
        api_signals(e, req, r);
    } else if (get && !strcmp(p, "findings")) {
        api_findings(e, r);
    } else if (get && !strcmp(p, "dtcs")) {
        api_dtcs(e, r);
    } else if (get && !strcmp(p, "trips")) {
        api_trips(e, r);
    } else if (get && !strncmp(p, "trips/", 6)) {
        char id[40];
        const char *rest = strchr(p + 6, '/');
        size_t n = rest ? (size_t)(rest - (p + 6)) : strlen(p + 6);
        if (n >= sizeof(id)) n = sizeof(id) - 1;
        memcpy(id, p + 6, n);
        id[n] = 0;
        if (rest && !strcmp(rest, "/series")) {
            api_series(e, id, req, r);
        } else {
            bool raw = rest && !strcmp(rest, "/log");
            char *path = cn_trip_path(e, id, raw ? "cnl" : "json");
            if (!path || (rest && !raw)) {
                fail(r, 404, "not found");
            } else {
                cn_strlcpy(r->send_file, path, sizeof(r->send_file));
                if (raw) {
                    r->content_type = "text/plain";
                    snprintf(r->download_name, sizeof(r->download_name), "%s.cnl", id);
                    lock(e);
                    if (e->trip.active && e->trip.f && !strcmp(e->trip.id, id)) fflush(e->trip.f);
                    unlock(e);
                }
            }
            free(path);
        }
    } else if (get && !strcmp(p, "config")) {
        api_config_get(e, r);
    } else if (post && !strcmp(p, "config")) {
        api_config_post(e, req, r);
    } else if (get && !strcmp(p, "events")) {
        api_events(e, r);
    } else if (get && !strcmp(p, "support")) {
        api_support(e, r);
    } else if (get && !strcmp(p, "devicelog")) {
        const char *dir = e->pf.data_dir ? e->pf.data_dir(e->pf.ctx) : NULL;
        if (!dir) {
            fail(r, 404, "no SD card");
        } else {
            snprintf(r->send_file, sizeof(r->send_file), "%s/device.log", dir);
            r->content_type = "text/plain";
            cn_strlcpy(r->download_name, "canary-device.log", sizeof(r->download_name));
        }
    } else if (post && !strcmp(p, "sync")) {
        lock(e);
        e->sync.force = true;
        e->sync.last_error[0] = 0;
        unlock(e);
        ok_only(r);
    } else if (post && !strcmp(p, "wake")) {
        e->wake_request = true;
        ok_only(r);
    } else if (post && !strcmp(p, "reboot")) {
        e->reboot_requested = true;
        ok_only(r);
    } else if (post && !strcmp(p, "storage/format")) {
        if (e->trip.active) {
            fail(r, 409, "a trip is being recorded");
        } else {
            e->format_requested = true;
            ok_only(r);
        }
    } else if (post && !strcmp(p, "time")) {
        api_time(e, req, r);
    } else if (post && !strcmp(p, "diag/request")) {
        api_diag(e, req, r);
    } else {
        return false; /* let the platform try (Wi-Fi scan, firmware update) before answering 404 */
    }
    if (r->body.oom) fail(r, 500, "out of memory");
    return true;
}
