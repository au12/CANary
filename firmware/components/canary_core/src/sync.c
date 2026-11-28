/* Server sync: odometer on every Wi-Fi connection, then trip logs.
 * Speaks to Glovebox with CANary support (/api/canary/…) and falls back to
 * the plain LubeLogger odometer API when that is all the server offers.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "canary/core.h"

#include "cJSON.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK (64 * 1024)
#define KM_PER_MILE 1.609344

static int64_t now_ms(cn_engine *e) { return e->pf.mono_ms(e->pf.ctx); }
static void lock(cn_engine *e) { if (e->pf.lock) e->pf.lock(e->pf.ctx); }
static void unlock(cn_engine *e) { if (e->pf.unlock) e->pf.unlock(e->pf.ctx); }

static void set_error(cn_engine *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_error(cn_engine *e, const char *fmt, ...)
{
    char msg[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    lock(e);
    bool changed = strcmp(e->sync.last_error, msg) != 0;
    cn_strlcpy(e->sync.last_error, msg, sizeof(e->sync.last_error));
    unlock(e);
    if (changed) cn_log(e, 'w', "Sync: %s", msg);
}

static int request(cn_engine *e, const char *method, const char *path, const char *ctype, const void *body, size_t len,
                   cn_http_resp *resp)
{
    char url[400];
    snprintf(url, sizeof(url), "%s%s", e->cfg.gb_url, path);
    cn_http_req req = {.method = method, .url = url, .api_key = e->cfg.gb_api_key, .content_type = ctype, .body = body,
                       .body_len = len, .timeout_ms = 20000};
    memset(resp, 0, sizeof(*resp));
    if (!e->pf.http || e->pf.http(e->pf.ctx, &req, resp) != 0) {
        resp->status = -1;
        return -1;
    }
    /* Any contact with the server is a chance to set the clock. */
    if (resp->server_time > 1700000000 && e->pf.set_wall_s) {
        int64_t wall = e->pf.wall_s(e->pf.ctx);
        if (wall < 1700000000 || llabs(wall - resp->server_time) > 120) {
            e->pf.set_wall_s(e->pf.ctx, resp->server_time);
            if (wall < 1700000000) {
                cn_log(e, 'i', "Clock set from the server");
                lock(e);
                cn_trips_fix_clock(e);
                unlock(e);
            }
        }
    }
    return resp->status;
}

static bool looks_like_html(const cn_http_resp *r)
{
    const char *p = r->body ? r->body : "";
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    return *p == '<';
}

static bool is_redirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

/* Glovebox and LubeLogger answer a missing or wrong key by sending the caller to their login page. */
static bool sent_to_login(int status, const cn_http_resp *r)
{
    if (is_redirect(status)) return !r->location[0] || strstr(r->location, "ogin") != NULL;
    return status == 200 && looks_like_html(r);
}

static const char *explain(const cn_engine *e, int status, const cn_http_resp *r)
{
    if (status < 0) return "server not reachable";
    if (status == 401 || status == 403) return "API key rejected (needs the Editor role)";
    if (sent_to_login(status, r)) return e->cfg.gb_api_key[0] ? "the server asked for a login: the API key was not accepted" : "the server asked for a login: it needs an API key";
    if (is_redirect(status)) return !strncmp(r->location, "https://", 8) ? "the server moved to https: start the address with https://" : "the server redirects elsewhere: check the address";
    if (status == 404) return "not found (check the server address and vehicle)";
    return "unexpected reply";
}

/* Decide what the server speaks. */
static int detect_mode(cn_engine *e)
{
    cn_http_resp r;
    int st = request(e, "GET", "/api/canary/hello", NULL, NULL, 0, &r);
    int mode = CN_SYNC_NONE;
    if (st == 200 && !looks_like_html(&r) && r.body && strstr(r.body, "canary_api")) {
        mode = CN_SYNC_CANARY;
    } else if (st < 0) {
        set_error(e, "%s", explain(e, st, &r));
    } else if (st == 401 || st == 403 || is_redirect(st) || sent_to_login(st, &r)) {
        set_error(e, "%s", explain(e, st, &r));
    } else {
        /* no CANary endpoints: is it at least a LubeLogger-compatible server? */
        free(r.body);
        char path[96];
        snprintf(path, sizeof(path), "/api/vehicle/odometerrecords/latest?vehicleId=%d", e->cfg.gb_vehicle_id);
        st = request(e, "GET", path, NULL, NULL, 0, &r);
        if (st == 200 && !looks_like_html(&r))
            mode = CN_SYNC_LUBELOGGER;
        else
            set_error(e, "%s", explain(e, st, &r));
    }
    free(r.body);
    return mode;
}

static bool odometer_lubelogger(cn_engine *e, double odo_km)
{
    bool miles = strcmp(e->cfg.odo_unit, "km") != 0;
    long value = lround((miles ? odo_km / KM_PER_MILE : odo_km) + e->cfg.odo_offset);
    char path[112];
    cn_http_resp r;
    snprintf(path, sizeof(path), "/api/vehicle/odometerrecords/latest?vehicleId=%d", e->cfg.gb_vehicle_id);
    int st = request(e, "GET", path, NULL, NULL, 0, &r);
    if (st != 200 || looks_like_html(&r)) {
        set_error(e, "odometer: %s", explain(e, st, &r));
        free(r.body);
        return false;
    }
    const char *p = r.body ? r.body : "0";
    while (*p == '"' || *p == ' ') p++;
    double latest = strtod(p, NULL);
    free(r.body);
    if ((double)value <= latest) return true; /* never write a lower or duplicate reading */

    int64_t wall = e->pf.wall_s(e->pf.ctx);
    char iso[24] = "";
    if (wall > 0) cn_iso8601(wall + (int64_t)e->cfg.tz_offset_min * 60, iso);
    iso[10] = 0; /* yyyy-MM-dd in local time */
    char body[200];
    int n = snprintf(body, sizeof(body), "{\"date\":\"%s\",\"odometer\":\"%ld\",\"notes\":\"Synced by CANary\",\"tags\":\"canary\"}",
                     iso[0] ? iso : "1970-01-01", value);
    snprintf(path, sizeof(path), "/api/vehicle/odometerrecords/add?vehicleId=%d", e->cfg.gb_vehicle_id);
    st = request(e, "POST", path, "application/json", body, (size_t)n, &r);
    bool ok = st == 200 && !looks_like_html(&r) && !(r.body && strstr(r.body, "\"success\":false"));
    if (!ok) set_error(e, "odometer: %s", explain(e, st, &r));
    free(r.body);
    if (ok) cn_log(e, 'i', "Odometer sent: %ld %s", value, miles ? "mi" : "km");
    return ok;
}

static char *checkin_json(cn_engine *e)
{
    cn_buf b;
    cn_buf_init(&b);
    cn_jw w;
    cn_jw_init(&w, &b);
    lock(e);
    cn_profile *p = e->profile;
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "device", e->device_id);
    cn_jw_kstr(&w, "name", e->cfg.device_name);
    cn_jw_kstr(&w, "fw", CN_FW_VERSION);
    cn_jw_kstr(&w, "board", e->board);
    cn_jw_kstr(&w, "vin", e->vin);
    cn_jw_key(&w, "profile");
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "id", p->id);
    cn_jw_kstr(&w, "name", p->name);
    cn_jw_kstr(&w, "hash", p->hash);
    cn_jw_end_obj(&w);
    cn_jw_knum(&w, "odometer_km", e->last_odo_km, 1);
    cn_jw_kint(&w, "odometer_time", e->last_odo_wall);
    cn_jw_knum(&w, "battery_v", e->vbat, 2);
    cn_jw_kstr(&w, "state", cn_vs_name(e->vs));
    cn_jw_kint(&w, "time", e->pf.wall_s(e->pf.ctx));
    cn_jw_kint(&w, "tz_offset_min", e->cfg.tz_offset_min);
    cn_jw_kint(&w, "pending_trips", e->sync.pending_trips);
    uint64_t total = 0, freeb = 0;
    if (e->pf.storage_info && e->pf.storage_info(e->pf.ctx, &total, &freeb) == 0) {
        cn_jw_key(&w, "storage");
        cn_jw_obj(&w);
        cn_jw_kint(&w, "total", (long long)total);
        cn_jw_kint(&w, "free", (long long)freeb);
        cn_jw_end_obj(&w);
    }
    cn_jw_kstr(&w, "worst", cn_sev_name(cn_worst_severity(e, NULL)));
    cn_jw_kbool(&w, "mil", e->mil);
    cn_jw_key(&w, "findings");
    cn_json_findings(e, &w);
    cn_jw_key(&w, "dtcs");
    cn_json_dtcs(e, &w);
    cn_jw_end_obj(&w);
    unlock(e);
    return cn_buf_take(&b);
}

/* Returns false on failure. *have_profile tells whether the server already holds our profile. */
static bool checkin(cn_engine *e, bool *have_profile)
{
    char *body = checkin_json(e);
    if (!body) return false;
    char path[96];
    snprintf(path, sizeof(path), "/api/canary/checkin?vehicleId=%d", e->cfg.gb_vehicle_id);
    cn_http_resp r;
    int st = request(e, "POST", path, "application/json", body, strlen(body), &r);
    free(body);
    bool ok = st == 200 && !looks_like_html(&r);
    if (ok) {
        cJSON *d = cJSON_Parse(r.body ? r.body : "");
        *have_profile = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "have_profile"));
        const cJSON *odo = cJSON_GetObjectItemCaseSensitive(d, "odometer");
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(odo, "recorded"))) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(odo, "value"), *u = cJSON_GetObjectItemCaseSensitive(odo, "unit");
            cn_log(e, 'i', "Odometer recorded by the server: %.0f %s", cJSON_IsNumber(v) ? v->valuedouble : 0,
                   cJSON_IsString(u) ? u->valuestring : "");
        }
        /* A server that knows its time zone says what the offset is today, which keeps the device
         * right across daylight saving changes without anyone opening its page. */
        const cJSON *tz = cJSON_GetObjectItemCaseSensitive(d, "tz_offset_min");
        bool tz_changed = false;
        if (cJSON_IsNumber(tz) && fabs(tz->valuedouble) <= 900) {
            lock(e);
            tz_changed = e->cfg.tz_offset_min != (int)tz->valuedouble;
            e->cfg.tz_offset_min = (int)tz->valuedouble;
            unlock(e);
        }
        cJSON_Delete(d);
        if (tz_changed) {
            cn_log(e, 'i', "Time zone offset is now %d min (from the server)", e->cfg.tz_offset_min);
            cn_engine_save_config(e);
        }
    } else {
        set_error(e, "check-in: %s", explain(e, st, &r));
    }
    free(r.body);
    return ok;
}

static bool upload_profile(cn_engine *e)
{
    lock(e);
    char *json = strdup(e->profile->resolved_json);
    char hash[12];
    cn_strlcpy(hash, e->profile->hash, sizeof(hash));
    unlock(e);
    if (!json) return false;
    char path[96];
    snprintf(path, sizeof(path), "/api/canary/profile?hash=%s", hash);
    cn_http_resp r;
    int st = request(e, "PUT", path, "application/json", json, strlen(json), &r);
    free(json);
    free(r.body);
    if (st != 200) {
        set_error(e, "profile upload failed (%d)", st);
        return false;
    }
    return true;
}

static long json_long(const char *body, const char *key, long dflt)
{
    cJSON *d = cJSON_Parse(body ? body : "");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(d, key);
    long out = cJSON_IsNumber(v) ? (long)v->valuedouble : dflt;
    cJSON_Delete(d);
    return out;
}

static bool upload_trip(cn_engine *e, const cn_trip_info *ti)
{
    char *file = cn_trip_path(e, ti->id, "cnl");
    if (!file) return false;
    FILE *f = fopen(file, "rb");
    free(file);
    if (!f) {
        /* the raw file is gone; nothing to send, stop asking */
        cn_trip_mark_uploaded(e, ti->id, 0, true);
        return true;
    }
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    char path[240];
    cn_http_resp r;
    snprintf(path, sizeof(path), "/api/canary/trips/%s/status?vehicleId=%d", ti->id, e->cfg.gb_vehicle_id);
    int st = request(e, "GET", path, NULL, NULL, 0, &r);
    long offset = st == 200 && !looks_like_html(&r) ? json_long(r.body, "received", 0) : -1;
    if (offset < 0) set_error(e, "trip upload: %s", explain(e, st, &r));
    free(r.body);
    if (offset < 0) {
        fclose(f);
        return false;
    }
    if (offset > total) offset = 0;
    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        fclose(f);
        return false;
    }
    lock(e);
    cn_strlcpy(e->sync.uploading, ti->id, sizeof(e->sync.uploading));
    e->sync.up_total = (uint64_t)total;
    e->sync.up_done = (uint64_t)offset;
    unlock(e);
    bool ok = true, complete = false;
    while (ok && !complete) {
        if (!e->net.connected) {
            ok = false;
            break;
        }
        fseek(f, offset, SEEK_SET);
        size_t n = fread(buf, 1, CHUNK, f);
        long long start = ti->start_wall;
        lock(e);
        for (int i = 0; i < e->ntrips; i++) /* the clock may have been set since this copy was taken */
            if (!strcmp(e->trips[i].id, ti->id)) start = e->trips[i].start_wall;
        unlock(e);
        snprintf(path, sizeof(path), "/api/canary/trips/%s/chunk?vehicleId=%d&offset=%ld&total=%ld&crc=%08x&device=%s&start=%lld", ti->id,
                 e->cfg.gb_vehicle_id, offset, total, (unsigned)cn_crc32(0, buf, n), e->device_id, start);
        st = request(e, "POST", path, "application/octet-stream", buf, n, &r);
        if (st != 200 || looks_like_html(&r)) {
            set_error(e, "trip upload: %s", explain(e, st, &r));
            ok = false;
        } else {
            long received = json_long(r.body, "received", -1);
            cJSON *d = cJSON_Parse(r.body ? r.body : "");
            complete = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "complete"));
            cJSON_Delete(d);
            if (received < 0 || (!complete && received <= offset && n > 0)) {
                set_error(e, "trip upload: server did not accept the data");
                ok = false;
            } else {
                offset = received;
                lock(e);
                e->sync.up_done = (uint64_t)offset;
                unlock(e);
                cn_trip_mark_uploaded(e, ti->id, (uint64_t)offset, complete);
            }
        }
        free(r.body);
    }
    free(buf);
    fclose(f);
    lock(e);
    e->sync.uploading[0] = 0;
    unlock(e);
    if (complete) cn_log(e, 'i', "Trip %s uploaded (%ld kB)", ti->id, total / 1024);
    return ok && complete;
}

static int count_pending(cn_engine *e)
{
    int n = 0;
    for (int i = 0; i < e->ntrips; i++) {
        const cn_trip_info *ti = &e->trips[i];
        if (!ti->uploaded_at && !ti->raw_deleted && ti->bytes > 0) n++;
    }
    return n;
}

void cn_sync_step(cn_engine *e)
{
    int64_t now = now_ms(e);
    lock(e);
    bool configured = e->cfg.gb_url[0] && e->cfg.gb_vehicle_id > 0; /* the key is optional: a server without login needs none */
    e->sync.pending_trips = e->cfg.gb_upload_logs && configured ? count_pending(e) : 0;
    bool connected = e->net.connected;
    bool settled = now - e->sync.connected_since_ms > 3000;
    bool first = !e->sync.did_connect_sync;
    int64_t since = e->sync.last_attempt_ms ? now - e->sync.last_attempt_ms : INT64_MAX;
    bool failing = e->sync.last_error[0] != 0;
    bool retry = since > (failing ? 60000 : 15 * 60 * 1000);
    bool pending_retry = e->sync.pending_trips > 0 && since > 60000;
    bool due = e->sync.force || (first && (!failing || since > 30000)) || retry || pending_retry;
    if (!configured) e->sync.mode = CN_SYNC_NONE;
    unlock(e);
    if (!configured || !connected || !settled || !due) return;

    lock(e);
    e->sync.busy = true;
    e->sync.force = false;
    e->sync.last_attempt_ms = now;
    unlock(e);

    bool ok = true;
    int mode = detect_mode(e);
    lock(e);
    e->sync.mode = mode;
    double odo = e->last_odo_km;
    unlock(e);
    if (mode == CN_SYNC_NONE) {
        ok = false;
    } else if (mode == CN_SYNC_LUBELOGGER) {
        if (odo > 0) ok = odometer_lubelogger(e, odo);
    } else {
        bool have_profile = true;
        ok = checkin(e, &have_profile);
        if (ok && !have_profile) ok = upload_profile(e);
        if (ok && e->cfg.gb_upload_logs) {
            /* oldest first, so the server's history fills in order */
            for (int guard = 0; guard < CN_TRIP_CACHE && ok; guard++) {
                cn_trip_info ti;
                bool found = false;
                lock(e);
                for (int i = e->ntrips - 1; i >= 0; i--) {
                    if (!e->trips[i].uploaded_at && !e->trips[i].raw_deleted && e->trips[i].bytes > 0) {
                        ti = e->trips[i];
                        found = true;
                        break;
                    }
                }
                unlock(e);
                if (!found) break;
                ok = upload_trip(e, &ti);
            }
        }
    }
    lock(e);
    if (ok) {
        e->sync.did_connect_sync = true;
        e->sync.last_ok_wall = e->pf.wall_s(e->pf.ctx);
        e->sync.last_error[0] = 0;
        if (odo > 0) {
            e->sync.last_odo_sent_km = odo;
            e->sync.last_odo_sent_wall = e->sync.last_ok_wall;
        }
        e->sync.pending_trips = e->cfg.gb_upload_logs ? count_pending(e) : 0;
    }
    else if (!e->sync.last_error[0])
        cn_strlcpy(e->sync.last_error, "sync did not complete", sizeof(e->sync.last_error));
    e->sync.busy = false;
    unlock(e);
    if (ok) cn_trips_retention(e);
}
