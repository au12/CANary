/* Unit and end-to-end tests for the CANary core, run on the host with a
 * modelled car and a fake Glovebox. SPDX-License-Identifier: GPL-3.0-or-later */
#include "canary/core.h"
#include "cJSON.h"
#include "host_platform.h"
#include "simcar.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks, g_fails;
static const char *g_test = "";

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_fails++;                                                                 \
            fprintf(stdout, "  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_test, #cond); \
        }                                                                              \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                                            \
    do {                                                                                                 \
        g_checks++;                                                                                      \
        double _a = (a), _b = (b);                                                                       \
        if (!(fabs(_a - _b) <= (tol))) {                                                                 \
            g_fails++;                                                                                   \
            fprintf(stdout, "  FAIL %s:%d [%s] %s = %g, expected %g\n", __FILE__, __LINE__, g_test, #a, _a, _b); \
        }                                                                                                \
    } while (0)

#define PROFILES REPO_ROOT "/profiles"

/* ───────────── small pieces ───────────── */

static void test_util(void)
{
    g_test = "util";
    CHECK(cn_crc32(0, "123456789", 9) == 0xCBF43926u);
    uint8_t b[8];
    CHECK(cn_parse_hex("22 F4 0c", b, 8) == 3 && b[0] == 0x22 && b[2] == 0x0C);
    CHECK(cn_parse_hex("2", b, 8) < 0);
    CHECK(cn_glob("3VV??7AX*", "3VV2B7AX5JM012345"));
    CHECK(!cn_glob("3VV??7AX*", "3VW2B7AX5JM012345"));
    CHECK(cn_glob("*", "") && cn_glob("WVG*", "WVGZZZ5NZJW000001"));
    char iso[24];
    cn_iso8601(1795478400, iso);
    CHECK(!strcmp(iso, "2026-11-24T00:00:00Z"));
    cn_iso8601(951782400, iso); /* leap day */
    CHECK(!strcmp(iso, "2000-02-29T00:00:00Z"));
    char v[32];
    CHECK(cn_query_get("a=1&sig=rpm%2Cboost&points=50", "sig", v, sizeof(v)) && !strcmp(v, "rpm,boost"));
    CHECK(!cn_query_get("a=1", "b", v, sizeof(v)));
    cn_buf buf;
    cn_buf_init(&buf);
    cn_jw w;
    cn_jw_init(&w, &buf);
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "a", "x\"y\n");
    cn_jw_key(&w, "list");
    cn_jw_arr(&w);
    cn_jw_int(&w, 1);
    cn_jw_num(&w, NAN, 2);
    cn_jw_obj(&w);
    cn_jw_kbool(&w, "t", true);
    cn_jw_end_obj(&w);
    cn_jw_end_arr(&w);
    cn_jw_knum(&w, "n", -0.004, 2);
    cn_jw_end_obj(&w);
    CHECK(!strcmp(buf.p, "{\"a\":\"x\\\"y\\n\",\"list\":[1,null,{\"t\":true}],\"n\":0.00}"));
    cJSON *parsed = cJSON_Parse(buf.p);
    CHECK(parsed != NULL);
    cJSON_Delete(parsed);
    cn_buf_free(&buf);
}

static void test_bits_and_dtcs(void)
{
    g_test = "bits";
    double v;
    /* real captures from a 2018 Tiguan (OBDb): instrument cluster odometer */
    uint8_t odo[] = {0x62, 0x22, 0x03, 0x02, 0x87, 0x86};
    CHECK(cn_extract_bits(odo + 3, 3, 0, 24, false, false, &v) && v == 165766);
    uint8_t odo_hr[] = {0x62, 0x22, 0xB8, 0x09, 0xE1, 0x70, 0x9E};
    CHECK(cn_extract_bits(odo_hr + 3, 4, 0, 32, false, false, &v));
    CHECK_NEAR(v / 1000.0, 165769.374, 0.0005);
    uint8_t odo_ecm[] = {0x62, 0x10, 0xE0, 0x00, 0x01, 0x58, 0x4D};
    CHECK(cn_extract_bits(odo_ecm + 3, 4, 0, 32, false, false, &v) && v == 88141);
    /* BMW cluster capture used by OdoSync: 0x00015294 km */
    uint8_t bmw[] = {0x00, 0x01, 0x52, 0x94, 0x00, 0x01, 0x52, 0x94};
    CHECK(cn_extract_bits(bmw, 8, 32, 32, false, false, &v) && v == 86676);
    uint8_t s[] = {0xFF, 0x38, 0x80};
    CHECK(cn_extract_bits(s, 3, 0, 16, true, false, &v) && v == -200);
    CHECK(cn_extract_bits(s, 3, 0, 16, false, true, &v) && v == 0x38FF);
    CHECK(cn_extract_bits(s, 3, 16, 1, false, false, &v) && v == 1);
    CHECK(cn_extract_bits(s, 3, 17, 7, false, false, &v) && v == 0);
    CHECK(!cn_extract_bits(s, 3, 20, 8, false, false, &v));

    g_test = "dtc";
    cn_dtc d[8];
    uint8_t m03[] = {0x43, 0x02, 0x02, 0x99, 0x01, 0x71};
    CHECK(cn_dtc_parse_obd(m03, sizeof(m03), CN_DTC_STORED, 0x7E8, d, 8) == 2);
    CHECK(!strcmp(d[0].code, "P0299") && !strcmp(d[1].code, "P0171") && d[0].kind == CN_DTC_STORED);
    uint8_t none[] = {0x43, 0x00};
    CHECK(cn_dtc_parse_obd(none, 2, CN_DTC_STORED, 0x7E8, d, 8) == 0);
    uint8_t u19[] = {0x59, 0x02, 0xFF, 0x02, 0x99, 0x00, 0x09, 0xC1, 0x23, 0x4A, 0x04, 0x42, 0x00, 0x11, 0x08};
    CHECK(cn_dtc_parse_uds(u19, sizeof(u19), 0x77D, 3, d, 8) == 3);
    CHECK(!strcmp(d[0].code, "P0299") && d[0].kind == CN_DTC_STORED);
    CHECK(!strcmp(d[1].code, "U0123") && d[1].ftb == 0x4A && d[1].kind == CN_DTC_PENDING);
    CHECK(!strcmp(d[2].code, "C0200") && d[2].ftb == 0x11);
    char code[8];
    cn_dtc_format(0x80 | 0x16, 0x34, code);
    CHECK(!strcmp(code, "B1634"));

    g_test = "read-only";
    CHECK(cn_diag_sid_allowed(0x01) && cn_diag_sid_allowed(0x22) && cn_diag_sid_allowed(0x19) && cn_diag_sid_allowed(0x09));
    static const uint8_t writes[] = {0x04, 0x08, 0x10, 0x11, 0x14, 0x27, 0x28, 0x2C, 0x2E, 0x2F, 0x31, 0x34, 0x36, 0x3D, 0x3E, 0x85};
    for (size_t i = 0; i < sizeof(writes); i++) CHECK(!cn_diag_sid_allowed(writes[i]));
}

static const char *const k_vars[] = {"a", "b", "c", "rpm", "boost_target", "boost_actual"};
static int resolve_var(void *ctx, const char *name)
{
    (void)ctx;
    for (int i = 0; i < 6; i++)
        if (!strcmp(k_vars[i], name)) return i;
    return -1;
}

static void test_expr(void)
{
    g_test = "expr";
    char *text = cn_read_file(PROFILES "/tests/expr_vectors.json", NULL);
    CHECK(text != NULL);
    cJSON *doc = cJSON_Parse(text ? text : "{}");
    free(text);
    const cJSON *c;
    int n = 0;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(doc, "cases"))
    {
        const char *src = cJSON_GetObjectItem(c, "expr")->valuestring;
        char err[96];
        cn_expr *e = cn_expr_compile(src, resolve_var, NULL, err, sizeof(err));
        n++;
        if (cJSON_IsTrue(cJSON_GetObjectItem(c, "error"))) {
            if (e) printf("  expected a compile error: %s\n", src);
            CHECK(e == NULL);
            cn_expr_free(e);
            continue;
        }
        if (!e) printf("  compile failed: %s (%s)\n", src, err);
        CHECK(e != NULL);
        if (!e) continue;
        double vars[6];
        for (int i = 0; i < 6; i++) {
            const cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(c, "vars"), k_vars[i]);
            vars[i] = cJSON_IsNumber(v) ? v->valuedouble : NAN;
        }
        double got = cn_expr_eval(e, vars);
        const cJSON *want = cJSON_GetObjectItem(c, "expect");
        if (cJSON_IsNull(want)) {
            if (!isnan(got)) printf("  %s = %g, expected not-available\n", src, got);
            CHECK(isnan(got));
        } else {
            if (fabs(got - want->valuedouble) > 1e-9) printf("  %s = %g, expected %g\n", src, got, want->valuedouble);
            CHECK(fabs(got - want->valuedouble) <= 1e-9);
        }
        cn_expr_free(e);
    }
    CHECK(n > 30);
    cJSON_Delete(doc);
}

static void test_profiles(void)
{
    g_test = "profiles";
    char err[160] = "";
    cn_profile *g = cn_profile_load("generic-obd2", host_profile_loader, PROFILES, err, sizeof(err));
    if (!g) printf("  %s\n", err);
    CHECK(g != NULL);
    cn_profile *t = cn_profile_load("vw-tiguan-mk2", host_profile_loader, PROFILES, err, sizeof(err));
    if (!t) printf("  %s\n", err);
    CHECK(t != NULL);
    if (!g || !t) return;
    CHECK(g->nsignals >= 35 && g->nmonitors >= 12);
    CHECK(t->nsignals > g->nsignals && t->nmonitors > g->nmonitors);
    CHECK(t->can_bitrate == 500000 && t->can_id_bits == 11 && g->can_bitrate == 0);
    CHECK(t->ecus[0].rx == 0x7E8 && g->ecus[0].rx == 0);
    int odo = cn_profile_signal_index(t, "odometer");
    CHECK(odo >= 0);
    cn_signal *s = &t->signals[odo];
    /* cluster first, engine fallbacks, then the generic PID */
    CHECK(s->nsrc == 5 && s->src[0].kind == CN_SRC_UDS && s->src[0].code == 0x22B8 && s->src[1].code == 0x2203 && s->src[4].kind == CN_SRC_OBD && s->src[4].code == 0xA6);
    CHECK(!strcmp(t->ecus[s->src[0].ecu].id, "cluster") && t->ecus[s->src[0].ecu].tx == 0x714 && t->ecus[s->src[0].ecu].rx == 0x77E);
    int ba = cn_profile_signal_index(t, "boost_actual");
    CHECK(ba >= 0 && t->signals[ba].src[0].code == 0x202A && !strcmp(t->signals[ba].name, "Boost pressure (actual, absolute)"));
    for (int i = 0; i < t->nmonitors; i++) {
        if (!t->monitors[i].enabled) printf("  monitor %s disabled: %s\n", t->monitors[i].id, t->monitors[i].why_disabled);
        CHECK(t->monitors[i].enabled);
        CHECK(t->monitors[i].nlevels > 0);
    }
    for (int i = 0; i < g->nmonitors; i++) CHECK(g->monitors[i].enabled);
    CHECK(strlen(t->hash) == 8 && strcmp(t->hash, g->hash) != 0);
    cn_profile *t2 = cn_profile_load("vw-tiguan-mk2", host_profile_loader, PROFILES, err, sizeof(err));
    CHECK(t2 && !strcmp(t2->hash, t->hash));
    cn_profile_free(t2);
    CHECK(cn_profile_load("does-not-exist", host_profile_loader, PROFILES, err, sizeof(err)) == NULL);

    char *doc = host_profile_loader(PROFILES, "vw-tiguan-mk2"), *mqb = host_profile_loader(PROFILES, "vw-mqb");
    CHECK(cn_profile_match_vin(doc, "3VV2B7AX5JM012345") == 20);
    CHECK(cn_profile_match_vin(doc, "1HGCM82633A004352") == 0);
    CHECK(cn_profile_match_vin(mqb, "3VV2B7AX5JM012345") == 5);
    CHECK(cn_profile_match_vin(mqb, "WVWZZZAUZJW000001") == 5);
    free(doc);
    free(mqb);
    char *index = host_profile_loader(PROFILES, CN_PROFILE_INDEX);
    CHECK(index && strstr(index, "\"vw-tiguan-mk2\"") && strstr(index, "Generic OBD-II"));
    free(index);
    cn_profile_free(g);
    cn_profile_free(t);
}

static void test_monitor_math(void)
{
    g_test = "monitors";
    cn_profile *p = cn_profile_load("generic-obd2", host_profile_loader, PROFILES, NULL, 0);
    CHECK(p != NULL);
    if (!p) return;
    double *v = calloc((size_t)p->nsignals, sizeof(double));
    for (int i = 0; i < p->nsignals; i++) v[i] = NAN;
#define SET(id, val) v[cn_profile_signal_index(p, id)] = (val)
    cn_monitors_reset(p);
    SET("rpm", 3000);
    SET("accel_pedal", 90);
    SET("baro", 98);
    SET("coolant_temp", 60);
    SET("speed", 80);
    SET("vbat", 14.2);
    /* 20 s of full throttle with boost 25 kPa short, then 20 s on target (the first 2 s settle) */
    for (int i = 0; i < 84; i++) {
        SET("boost_target", 220);
        SET("boost_actual", i < 4 ? 150 : i < 44 ? 195 : 219);
        if (i == 60) SET("coolant_temp", 80);
        cn_monitors_tick(p, v, i * 0.5, 0.5);
    }
    cn_monitors_evaluate(p, 42);
    cn_monitor *under = NULL, *warm = NULL, *lean = NULL, *chg = NULL;
    for (int i = 0; i < p->nmonitors; i++) {
        cn_monitor *m = &p->monitors[i];
        if (!strcmp(m->id, "boost_under_target")) under = m;
        if (!strcmp(m->id, "warmup_slow")) warm = m;
        if (!strcmp(m->id, "fuel_trim_idle")) lean = m;
        if (!strcmp(m->id, "charging_low")) chg = m;
    }
    CHECK(under && warm && lean && chg);
    CHECK(under->n == 80 && under->has_result);
    CHECK_NEAR(under->result, 25, 0.01); /* p90 of forty 25s and forty 1s */
    CHECK(under->status == CN_SEV_WARNING);
    char text[200];
    cn_monitor_text(under, text, sizeof(text));
    CHECK(strstr(text, "25.0 kPa short") != NULL);
    /* time_to: reached at t=30 s, but not enough gated time for a verdict yet */
    CHECK(warm->reached && fabs(warm->time_to_s - 30) < 0.01 && warm->status == CN_SEV_NODATA);
    /* signals never seen: no verdict, not a false alarm */
    CHECK(lean->status == CN_SEV_NODATA && lean->n == 0);
    CHECK(chg->status == CN_SEV_NODATA); /* needs 120 s */
    for (int i = 0; i < 300; i++) cn_monitors_tick(p, v, 42 + i * 0.5, 0.5);
    cn_monitors_evaluate(p, 190);
    CHECK(chg->status == CN_SEV_OK);
    CHECK_NEAR(chg->result, 14.2, 1e-6);
    SET("vbat", 12.9);
    cn_monitors_reset(p);
    for (int i = 0; i < 300; i++) cn_monitors_tick(p, v, i * 0.5, 0.5);
    cn_monitors_evaluate(p, 150);
    CHECK(chg->status == CN_SEV_WARNING);
    CHECK(!strcmp(cn_monitor_headline(chg), "Charging voltage low") && !strcmp(chg->title, "Charging voltage"));

    /* a two-sided check: one definition, a different story for each direction */
    cn_monitors_reset(p);
    SET("rpm", 800);
    SET("speed", 0);
    SET("coolant_temp", 90);
    SET("fuel_trim_b1", 12);
    for (int i = 0; i < 120; i++) cn_monitors_tick(p, v, i * 0.5, 0.5);
    cn_monitors_evaluate(p, 60);
    char *const *causes;
    CHECK(lean->two_sided && lean->status == CN_SEV_ADVISORY && lean->side == 0);
    CHECK(!strcmp(cn_monitor_headline(lean), "Running lean at idle"));
    CHECK(cn_monitor_causes(lean, &causes) == 4 && strstr(causes[0], "Vacuum") != NULL);
    cn_monitors_reset(p);
    SET("fuel_trim_b1", -17);
    for (int i = 0; i < 120; i++) cn_monitors_tick(p, v, i * 0.5, 0.5);
    cn_monitors_evaluate(p, 60);
    CHECK(lean->status == CN_SEV_WARNING && lean->side == 1);
    CHECK(!strcmp(cn_monitor_headline(lean), "Running rich at idle"));
    cn_monitor_text(lean, text, sizeof(text));
    CHECK(strstr(text, "-17.0%") != NULL && strstr(text, "taking fuel out") != NULL);
    CHECK(cn_monitor_causes(lean, &causes) == 4 && strstr(causes[0], "injector") != NULL);
    cn_monitors_reset(p);
    SET("fuel_trim_b1", 2);
    for (int i = 0; i < 120; i++) cn_monitors_tick(p, v, i * 0.5, 0.5);
    cn_monitors_evaluate(p, 60);
    CHECK(lean->status == CN_SEV_OK && !strcmp(cn_monitor_headline(lean), "Fuel mixture at idle"));
    /* an engine that is already warm when the trip begins says nothing about warm-up time */
    CHECK(warm->skip && warm->n == 0 && warm->status == CN_SEV_NODATA);
#undef SET
    free(v);
    cn_profile_free(p);
}

/* ───────────── fake Glovebox ───────────── */

typedef struct {
    bool canary_api, reject_key;
    bool login_redirect; /* answer a bad key the way Glovebox does: with a redirect to its login page */
    const char *redirect_to;
    double latest_mi;
    int odo_posts, checkins, profile_puts, chunk_posts;
    double last_checkin_km;
    bool have_profile;
    char trip_id[40];
    cn_buf trip_data;
    int fail_chunk_at; /* drop the connection on this chunk (1-based), once */
    bool crc_ok;
    int64_t server_time;
    int requests;
} fake_gb;

static void reply(cn_http_resp *r, int status, const char *body)
{
    r->status = status;
    r->body = strdup(body);
    r->body_len = strlen(body);
}

static int fake_http(void *ctx, const cn_http_req *req, cn_http_resp *r)
{
    fake_gb *g = ctx;
    memset(r, 0, sizeof(*r));
    g->requests++;
    const char *path = strstr(req->url, "/api/");
    if (!path) return -1;
    r->server_time = g->server_time;
    if (g->reject_key || !req->api_key || strcmp(req->api_key, "secret") != 0) {
        if (g->login_redirect) {
            reply(r, 302, "");
            cn_strlcpy(r->location, g->redirect_to ? g->redirect_to : "/Login/Index", sizeof(r->location));
        } else
            reply(r, 401, "{\"success\":false}");
        return 0;
    }
    char q[64];
    const char *query = strchr(path, '?');
    if (!strncmp(path, "/api/canary/", 12) && !g->canary_api) {
        reply(r, 404, "<html>not found</html>");
    } else if (!strncmp(path, "/api/canary/hello", 17)) {
        reply(r, 200, "{\"server\":\"glovebox\",\"canary_api\":1}");
    } else if (!strncmp(path, "/api/canary/checkin", 19)) {
        g->checkins++;
        cJSON *d = cJSON_Parse(req->body);
        g->last_checkin_km = cJSON_GetObjectItem(d, "odometer_km")->valuedouble;
        cJSON_Delete(d);
        double mi = round(g->last_checkin_km / 1.609344);
        bool rec = mi > g->latest_mi;
        if (rec) g->latest_mi = mi;
        char body[200];
        snprintf(body, sizeof(body), "{\"ok\":true,\"have_profile\":%s,\"odometer\":{\"recorded\":%s,\"value\":%.0f,\"unit\":\"mi\"}}",
                 g->have_profile ? "true" : "false", rec ? "true" : "false", mi);
        reply(r, 200, body);
    } else if (!strncmp(path, "/api/canary/profile", 19)) {
        g->profile_puts++;
        g->have_profile = strstr(req->body, "\"monitors\"") != NULL;
        reply(r, 200, "{\"ok\":true}");
    } else if (!strncmp(path, "/api/canary/trips/", 18)) {
        const char *id = path + 18, *slash = strchr(id, '/');
        char tid[40];
        snprintf(tid, sizeof(tid), "%.*s", (int)(slash - id), id);
        if (strcmp(tid, g->trip_id) != 0) {
            cn_strlcpy(g->trip_id, tid, sizeof(g->trip_id));
            g->trip_data.len = 0;
        }
        char body[96];
        if (!strncmp(slash, "/status", 7)) {
            snprintf(body, sizeof(body), "{\"received\":%zu,\"complete\":false}", g->trip_data.len);
            reply(r, 200, body);
        } else {
            g->chunk_posts++;
            if (g->fail_chunk_at && g->chunk_posts == g->fail_chunk_at) {
                g->fail_chunk_at = 0;
                return -1;
            }
            long offset = cn_query_get(query + 1, "offset", q, sizeof(q)) ? atol(q) : -1;
            long total = cn_query_get(query + 1, "total", q, sizeof(q)) ? atol(q) : -1;
            cn_query_get(query + 1, "crc", q, sizeof(q));
            if (strtoul(q, NULL, 16) != cn_crc32(0, req->body, req->body_len)) g->crc_ok = false;
            if (offset == (long)g->trip_data.len) cn_buf_put(&g->trip_data, req->body, req->body_len);
            snprintf(body, sizeof(body), "{\"ok\":true,\"received\":%zu,\"complete\":%s}", g->trip_data.len,
                     (long)g->trip_data.len >= total ? "true" : "false");
            reply(r, 200, body);
        }
    } else if (!strncmp(path, "/api/vehicle/odometerrecords/latest", 35)) {
        char body[32];
        snprintf(body, sizeof(body), "%.0f", g->latest_mi);
        reply(r, 200, body);
    } else if (!strncmp(path, "/api/vehicle/odometerrecords/add", 32)) {
        g->odo_posts++;
        cJSON *d = cJSON_Parse(req->body);
        g->latest_mi = atof(cJSON_GetObjectItem(d, "odometer")->valuestring);
        CHECK(strlen(cJSON_GetObjectItem(d, "date")->valuestring) == 10);
        cJSON_Delete(d);
        reply(r, 200, "{\"success\":true,\"message\":\"Odometer Record Added\"}");
    } else {
        reply(r, 404, "{}");
    }
    return 0;
}

/* ───────────── end-to-end drives ───────────── */

typedef struct {
    simcar *car;
    host_platform *h;
    cn_engine *e;
    fake_gb gb;
    char dir[128];
} rig;

static void rig_start(rig *r, const char *name, const simcar_cfg *cfg, int minutes, bool configure_gb)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->dir, sizeof(r->dir), "/tmp/canary-test-%d-%s", (int)getpid(), name);
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", r->dir);
    if (system(cmd) != 0) abort();
    cn_mkdirs(r->dir);
    r->car = simcar_create(cfg);
    simcar_script_commute(r->car, minutes, false);
    r->h = host_platform_create(r->car, r->dir, true);
    r->gb.canary_api = true;
    r->gb.crc_ok = true;
    r->gb.latest_mi = 102990;
    r->h->http_hook = fake_http;
    r->h->http_ctx = &r->gb;
    if (configure_gb) {
        const char *cfgjson = "{\"glovebox\":{\"url\":\"http://gb.test\",\"api_key\":\"secret\",\"vehicle_id\":1,\"odometer_unit\":\"mi\"},"
                              "\"tz_offset_min\":-300}";
        r->h->pf.kv_save(r->h, "config", cfgjson);
    }
    r->e = cn_engine_create(&r->h->pf, host_profile_loader, PROFILES, "canary-test01", "sim");
}

/* Run the engine until virtual time `until_s`; Wi-Fi is "home": up only while the car is parked. */
static void rig_run(rig *r, double until_s, bool wifi)
{
    int64_t last_sync = -10000;
    while (r->h->vt_ms < (int64_t)(until_s * 1000)) {
        cn_vehicle_step(r->e);
        if (r->h->vt_ms - last_sync >= 1000) {
            last_sync = r->h->vt_ms;
            r->gb.server_time = r->h->wall_base ? r->h->wall_base + r->h->vt_ms / 1000 : 1795478400 + r->h->vt_ms / 1000;
            cn_net_status st = {0};
            st.connected = wifi && !simcar_ignition(r->car);
            cn_strlcpy(st.ssid, "home", sizeof(st.ssid));
            cn_strlcpy(st.ip, "10.0.10.50", sizeof(st.ip));
            cn_net_update(r->e, &st);
            cn_sync_step(r->e);
        }
    }
}

static void rig_stop(rig *r)
{
    cn_engine_destroy(r->e);
    host_platform_free(r->h);
    simcar_free(r->car);
    cn_buf_free(&r->gb.trip_data);
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", r->dir);
    if (system(cmd) != 0) abort();
}

static cn_monitor *mon(cn_engine *e, const char *id)
{
    for (int i = 0; i < e->profile->nmonitors; i++)
        if (!strcmp(e->profile->monitors[i].id, id)) return &e->profile->monitors[i];
    return NULL;
}

static cn_signal *sig(cn_engine *e, const char *id)
{
    int i = cn_profile_signal_index(e->profile, id);
    return i < 0 ? NULL : &e->profile->signals[i];
}

static void test_healthy_drive(void)
{
    g_test = "healthy drive";
    rig r;
    rig_start(&r, "healthy", NULL, 12, true);
    CHECK(r.e != NULL);
    if (!r.e) return;
    double odo0 = simcar_odometer(r.car);

    /* parked for the first 15 s: one probe, then silence */
    rig_run(&r, 12, true);
    CHECK(r.e->vs == CN_VS_OFF);
    unsigned probes = simcar_frames_seen(r.car);
    CHECK(probes >= 1 && probes <= 2);

    /* drive: engine starts at t=19 s */
    rig_run(&r, 120, true);
    CHECK(r.e->vs == CN_VS_RUN && r.e->trip.active);
    CHECK(!strcmp(r.e->vin, "3VV2B7AX5JM012345"));
    CHECK(!strcmp(r.e->profile->id, "vw-tiguan-mk2"));
    CHECK(r.e->can_bitrate == 500000 && !r.e->can_ext);
    char why[48];
    CHECK(!cn_power_can_sleep(r.e, why, sizeof(why)));

    rig_run(&r, 12 * 60 - 425, true); /* just before the engine is switched off */
    CHECK(r.e->probe_pending == 0);
    cn_signal *ba = sig(r.e, "boost_actual"), *odo = sig(r.e, "odometer"), *oil = sig(r.e, "oil_temp"), *kn = sig(r.e, "knock_retard_worst");
    CHECK(ba && ba->active == 0 && ba->valid);                       /* Volkswagen DID 202A answered */
    CHECK(odo && odo->active == 0 && odo->valid);                    /* instrument cluster */
    CHECK_NEAR(odo->value, simcar_odometer(r.car), 1.5);
    CHECK(oil && oil->active == 0 && oil->valid && oil->value > 20);
    CHECK(kn && kn->valid);
    cn_signal *bt = sig(r.e, "boost_target");
    CHECK(bt && bt->valid && bt->n > 100);
    CHECK(sig(r.e, "cat_temp")->active < 0);                         /* PID 3C: this car does not have it */
    CHECK(sig(r.e, "maf")->active < 0);
    CHECK(sig(r.e, "vbat")->valid && sig(r.e, "vbat")->value > 13.8);

    /* engine off, then parked with Wi-Fi in range */
    rig_run(&r, 12 * 60 + 240, true);
    CHECK(r.e->vs == CN_VS_OFF && !r.e->trip.active);
    CHECK(r.e->ntrips == 1);
    cn_trip_info *ti = &r.e->trips[0];
    double driven = simcar_odometer(r.car) - odo0;
    CHECK(driven > 8 && driven < 30);
    CHECK_NEAR(ti->distance_km, driven, driven * 0.04);
    CHECK(ti->duration_s > 600 && ti->duration_s < 760);
    CHECK(ti->samples > 5000);
    CHECK(ti->worst == CN_SEV_OK);
    for (int i = 0; i < r.e->profile->nmonitors; i++) {
        cn_monitor *m = &r.e->profile->monitors[i];
        if (m->status != CN_SEV_NODATA && m->status != CN_SEV_OK) printf("  healthy car flagged by %s (%g)\n", m->id, m->result);
        CHECK(m->status == CN_SEV_NODATA || m->status == CN_SEV_OK);
    }
    CHECK(mon(r.e, "boost_under_target")->status == CN_SEV_OK);
    CHECK(mon(r.e, "chain_stretch_intake")->status == CN_SEV_OK);
    if (mon(r.e, "knock_retard")->status != CN_SEV_OK) printf("  knock n=%u result=%g\n", (unsigned)mon(r.e, "knock_retard")->n, mon(r.e, "knock_retard")->result);
    CHECK(mon(r.e, "knock_retard")->status == CN_SEV_OK);
    CHECK(mon(r.e, "charging_low")->status == CN_SEV_OK);
    CHECK(r.e->ndtcs == 0 && !r.e->mil);
    CHECK(mon(r.e, "catalyst_overheat")->unavailable && mon(r.e, "fuel_trim_bank_split")->unavailable);
    CHECK(!mon(r.e, "boost_under_target")->unavailable && !mon(r.e, "knock_retard")->unavailable);

    /* the raw log is well formed */
    char *path = cn_trip_path(r.e, ti->id, "cnl");
    size_t len = 0;
    char *log = cn_read_file(path, &len);
    CHECK(log && !strncmp(log, "#CNL 1\n#device canary-test01\n", 29));
    CHECK(log && strstr(log, "#vin 3VV2B7AX5JM012345\n") && strstr(log, "#profile vw-tiguan-mk2 "));
    CHECK(log && strstr(log, " rpm rpm fast Engine speed\n") && strstr(log, "\n!") && strstr(log, " end {\"id\":\""));
    CHECK(len == ti->bytes && len > 100000);
    free(path);

    /* sync happened once the car was parked on Wi-Fi */
    CHECK(r.gb.checkins >= 1);
    CHECK_NEAR(r.gb.last_checkin_km, simcar_odometer(r.car), 1.5);
    CHECK_NEAR(r.gb.latest_mi, round(simcar_odometer(r.car) / 1.609344), 1);
    CHECK(r.gb.profile_puts == 1 && r.gb.have_profile);
    CHECK(r.gb.crc_ok);
    CHECK(r.gb.trip_data.len == len && log && !memcmp(r.gb.trip_data.p, log, len));
    CHECK(ti->uploaded_at > 0 && ti->uploaded_bytes == len);
    CHECK(r.e->sync.mode == CN_SYNC_CANARY && r.e->sync.pending_trips == 0 && !r.e->sync.last_error[0]);
    free(log);

    /* summary on disk survives a rescan */
    cn_trips_scan(r.e);
    CHECK(r.e->ntrips == 1 && r.e->trips[0].uploaded_at > 0 && r.e->trips[0].samples == ti->samples);

    /* bus etiquette: once off, nothing more is transmitted while the battery just rests */
    unsigned before = simcar_frames_seen(r.car);
    CHECK(cn_power_can_sleep(r.e, why, sizeof(why)) == false || r.h->vt_ms > 0); /* delay not yet over is fine */
    rig_run(&r, 12 * 60 + 400, true);
    CHECK(simcar_frames_seen(r.car) == before);
    CHECK(cn_power_can_sleep(r.e, why, sizeof(why)));

    /* raw data goes away a week after upload, the summary stays */
    r.h->wall_base += 6 * 86400;
    cn_trips_retention(r.e);
    path = cn_trip_path(r.e, r.e->trips[0].id, "cnl");
    CHECK(access(path, F_OK) == 0 && !r.e->trips[0].raw_deleted);
    r.h->wall_base += 2 * 86400;
    cn_trips_retention(r.e);
    CHECK(access(path, F_OK) != 0 && r.e->trips[0].raw_deleted);
    free(path);
    path = cn_trip_path(r.e, r.e->trips[0].id, "json");
    CHECK(access(path, F_OK) == 0);
    free(path);
    rig_stop(&r);
}

static void test_faulty_drive(void)
{
    g_test = "faulty drive";
    simcar_cfg cfg = {.boost_leak = 0.22, .chain_stretch = 4.6, .idle_lean = 16, .knock_cyl3 = 5.5, .misfire_rate = 5,
                      .stored_dtcs = "P0299", .pending_dtcs = "P0171", .abs_dtcs = "C10B8", .mil = true};
    rig r;
    rig_start(&r, "faulty", &cfg, 12, true);
    r.gb.fail_chunk_at = 2; /* Wi-Fi drops mid-upload */
    rig_run(&r, 12 * 60 + 60, true);
    CHECK(r.e->ntrips == 1);
    cn_monitor *m;
    m = mon(r.e, "boost_under_target");
    CHECK(m->status >= CN_SEV_WARNING && m->result > 20);
    m = mon(r.e, "chain_stretch_intake");
    CHECK(m->status == CN_SEV_WARNING);
    CHECK_NEAR(m->result, 4.6, 0.05);
    m = mon(r.e, "fuel_trim_idle");
    CHECK(m->status >= CN_SEV_ADVISORY && m->result > 9);
    CHECK(!strcmp(cn_monitor_headline(m), "Running lean at idle"));
    m = mon(r.e, "knock_one_cylinder");
    CHECK(m->status >= CN_SEV_ADVISORY);
    m = mon(r.e, "knock_retard");
    CHECK(m->status >= CN_SEV_ADVISORY);
    m = mon(r.e, "misfires");
    CHECK(m->status >= CN_SEV_WARNING && m->result >= 30);
    CHECK(mon(r.e, "charging_low")->status == CN_SEV_OK);
    CHECK(mon(r.e, "rail_pressure_low")->status == CN_SEV_OK);
    /* trouble codes from OBD-II modes and from the brake module, without duplicates */
    bool p0299 = false, p0171 = false, c10b8 = false;
    int n299 = 0;
    for (int i = 0; i < r.e->ndtcs; i++) {
        cn_dtc *d = &r.e->dtcs[i];
        if (!strcmp(d->code, "P0299") && d->kind == CN_DTC_STORED) p0299 = true, n299++;
        if (!strcmp(d->code, "P0171") && d->kind == CN_DTC_PENDING) p0171 = true;
        if (!strcmp(d->code, "C10B8") && d->ecu != 0xFF && !strcmp(r.e->profile->ecus[d->ecu].id, "abs")) c10b8 = true;
    }
    CHECK(p0299 && p0171 && c10b8);
    CHECK(r.e->mil);
    CHECK(cn_dtc_severity(r.e) == CN_SEV_CRITICAL);
    if (r.e->trips[0].worst != CN_SEV_CRITICAL || r.e->trips[0].nfindings < 5) printf("  worst=%d findings=%d mil=%d\n", r.e->trips[0].worst, r.e->trips[0].nfindings, r.e->mil);
    CHECK(r.e->trips[0].worst == CN_SEV_CRITICAL && r.e->trips[0].nfindings >= 5);
    /* upload was interrupted and then resumed without duplicating data */
    rig_run(&r, 12 * 60 + 300, true);
    char *path = cn_trip_path(r.e, r.e->trips[0].id, "cnl");
    size_t len = 0;
    char *log = cn_read_file(path, &len);
    CHECK(r.gb.trip_data.len == len && log && !memcmp(r.gb.trip_data.p, log, len));
    CHECK(r.e->trips[0].uploaded_at > 0 && r.gb.crc_ok);
    CHECK(log && strstr(log, " dtc {\"codes\":[{\"code\":\"P0299\""));
    /* the log names the kind and the control unit in words, exactly as the API does */
    CHECK(log && strstr(log, "\"kind\":\"stored\"") && strstr(log, "\"ecu\":\"abs\",\"module\":\"Brakes / ABS\""));
    free(log);
    free(path);

    /* API smoke test on real state */
    cn_api_req req = {.method = "GET", .path = "/api/findings"};
    cn_api_resp resp;
    memset(&resp, 0, sizeof(resp));
    CHECK(cn_api_handle(r.e, &req, &resp) && resp.status == 200);
    cJSON *d = cJSON_Parse(resp.body.p);
    CHECK(d && cJSON_GetArraySize(cJSON_GetObjectItem(d, "monitors")) == r.e->profile->nmonitors);
    cJSON_Delete(d);
    cn_buf_free(&resp.body);
    static const char *const gets[] = {"/api/status", "/api/live", "/api/signals", "/api/dtcs", "/api/trips", "/api/config", "/api/events", "/api/support"};
    for (size_t i = 0; i < sizeof(gets) / sizeof(gets[0]); i++) {
        req.path = gets[i];
        req.query = "hist=1";
        memset(&resp, 0, sizeof(resp));
        CHECK(cn_api_handle(r.e, &req, &resp) && resp.status == 200);
        d = cJSON_Parse(resp.body.p ? resp.body.p : "");
        if (!d) printf("  %s returned invalid JSON\n", gets[i]);
        CHECK(d != NULL);
        cJSON_Delete(d);
        cn_buf_free(&resp.body);
    }
    char series_path[80];
    snprintf(series_path, sizeof(series_path), "/api/trips/%s/series", r.e->trips[0].id);
    req.path = series_path;
    req.query = "sig=rpm,boost_actual,nope&points=100";
    memset(&resp, 0, sizeof(resp));
    CHECK(cn_api_handle(r.e, &req, &resp) && resp.status == 200);
    d = cJSON_Parse(resp.body.p);
    const cJSON *ser = cJSON_GetObjectItem(d, "series");
    CHECK(cJSON_GetArraySize(ser) == 3);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetArrayItem(ser, 0), "v")) > 80);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(ser, 2), "found")));
    cJSON_Delete(d);
    cn_buf_free(&resp.body);
    /* secrets never leave through the API */
    req.path = "/api/config";
    req.query = NULL;
    memset(&resp, 0, sizeof(resp));
    cn_api_handle(r.e, &req, &resp);
    CHECK(resp.body.p && !strstr(resp.body.p, "secret") && strstr(resp.body.p, "\"api_key_set\":true"));
    cn_buf_free(&resp.body);
    /* a write service is refused before it reaches the bus */
    unsigned tx = r.h->tx_frames;
    cn_api_req post = {.method = "POST", .path = "/api/diag/request", .body = "{\"tx\":\"7E0\",\"rx\":\"7E8\",\"data\":\"14FFFFFF\"}"};
    memset(&resp, 0, sizeof(resp));
    CHECK(cn_api_handle(r.e, &post, &resp) && resp.status == 403 && r.h->tx_frames == tx);
    cn_buf_free(&resp.body);
    uint8_t clear[] = {0x04}, out[8];
    cn_addr a = {0x7DF, 0, false};
    CHECK(cn_isotp_request(&r.h->pf, &a, clear, 1, out, sizeof(out), 100, NULL) == CN_ERR_DENIED && r.h->tx_frames == tx);
    rig_stop(&r);
}

static void test_generic_car(void)
{
    g_test = "generic 29-bit car";
    simcar_cfg cfg = {.ext_ids = true, .no_uds = true, .vin = "1HGCM82633A004352", .boost_leak = 0.25, .thermostat_open = true};
    rig r;
    rig_start(&r, "generic", &cfg, 30, false);
    r.gb.canary_api = false; /* a plain LubeLogger server */
    r.gb.latest_mi = 200000; /* someone typed a higher reading: never go below it */
    const char *cfgjson = "{\"glovebox\":{\"url\":\"http://gb.test\",\"api_key\":\"secret\",\"vehicle_id\":3,\"odometer_unit\":\"km\"}}";
    cn_config_from_json(&r.e->cfg, cfgjson);
    rig_run(&r, 30 * 60 + 200, true);
    CHECK(!strcmp(r.e->profile->id, "generic-obd2"));
    CHECK(r.e->can_ext && r.e->can_bitrate == 500000 && r.e->obd.tx == 0x18DB33F1u);
    CHECK(!strcmp(r.e->vin, "1HGCM82633A004352"));
    CHECK(r.e->ntrips == 1);
    CHECK(sig(r.e, "odometer")->active >= 0); /* PID A6 */
    CHECK(mon(r.e, "boost_under_target")->status >= CN_SEV_WARNING); /* via standard PID 70 */
    cn_monitor *w = mon(r.e, "warmup_slow");
    CHECK(w->status >= CN_SEV_ADVISORY && !w->reached);
    CHECK(r.e->sync.mode == CN_SYNC_LUBELOGGER);
    CHECK(r.gb.odo_posts == 0 && r.gb.latest_mi == 200000);
    r.gb.latest_mi = 1000;
    r.e->sync.force = true;
    rig_run(&r, 30 * 60 + 260, true);
    CHECK(r.gb.odo_posts == 1);
    CHECK_NEAR(r.gb.latest_mi, simcar_odometer(r.car), 1.0); /* km, as configured */
    CHECK(r.e->trips[0].uploaded_at == 0); /* raw logs need the CANary API */
    rig_stop(&r);
}

static void test_clock_and_errors(void)
{
    g_test = "clock unknown";
    rig r;
    rig_start(&r, "clock", NULL, 12, true);
    r.h->wall_base = 0; /* first boot, no RTC time */
    r.gb.reject_key = true;
    rig_run(&r, 60, false); /* drive away with no Wi-Fi contact yet */
    rig_run(&r, 12 * 60 + 100, true);
    CHECK(r.e->ntrips == 1);
    CHECK(r.e->sync.last_error[0] != 0 && strstr(r.e->sync.last_error, "API key"));
    CHECK(r.e->trips[0].uploaded_at == 0);
    /* a server that answers with its login page instead of a refusal gets the same plain explanation */
    r.gb.login_redirect = true;
    r.e->sync.last_error[0] = 0;
    rig_run(&r, 12 * 60 + 160, true);
    CHECK(strstr(r.e->sync.last_error, "asked for a login") && strstr(r.e->sync.last_error, "API key"));
    CHECK(r.e->trips[0].uploaded_at == 0);
    /* and a reverse proxy that insists on https is named for what it is */
    r.gb.redirect_to = "https://gb.test/api/canary/hello";
    r.e->sync.last_error[0] = 0;
    rig_run(&r, 12 * 60 + 200, true);
    CHECK(strstr(r.e->sync.last_error, "https://"));
    r.gb.login_redirect = false;
    /* the server's Date header still set the clock, so the trip got a real start time */
    CHECK(r.h->wall_base != 0);
    r.gb.reject_key = false;
    rig_run(&r, 12 * 60 + 240, true);
    CHECK(!r.e->sync.last_error[0] && r.e->trips[0].uploaded_at > 1700000000);
    char *path = cn_trip_path(r.e, r.e->trips[0].id, "json");
    char *sum = cn_read_file(path, NULL);
    cJSON *d = cJSON_Parse(sum ? sum : "");
    CHECK(d && cJSON_GetObjectItem(d, "start")->valuedouble > 1700000000);
    cJSON_Delete(d);
    free(sum);
    free(path);
    CHECK(!strncmp(r.e->trips[0].id, "nt-", 3));
    rig_stop(&r);

    g_test = "power";
    cn_config c;
    cn_config_defaults(&c);
    CHECK(!cn_power_should_wake(&c, 12.5f, 12.4f));
    CHECK(cn_power_should_wake(&c, 13.6f, 12.4f));
    CHECK(!cn_power_should_wake(&c, 13.6f, 13.5f)); /* charger connected the whole time */
    CHECK(cn_power_should_wake(&c, 13.05f, 12.4f)); /* weak alternator: the jump is enough */
    CHECK(!cn_power_should_wake(&c, NAN, 12.4f));

    g_test = "config";
    cn_config_from_json(&c, "{\"wifi\":{\"networks\":[{\"ssid\":\"home\",\"pass\":\"hunter22\"}]},\"glovebox\":{\"url\":\"http://x/\",\"api_key\":\"k\"}}");
    CHECK(c.nwifi == 1 && !strcmp(c.wifi[0].pass, "hunter22") && !strcmp(c.gb_url, "http://x"));
    cn_config_from_json(&c, "{\"wifi\":{\"networks\":[{\"ssid\":\"home\"},{\"ssid\":\"shop\",\"pass\":\"abc\"}]},\"power\":{\"sleep_voltage\":99}}");
    CHECK(c.nwifi == 2 && !strcmp(c.wifi[0].pass, "hunter22") && !strcmp(c.wifi[1].pass, "abc"));
    CHECK(c.sleep_voltage == 14.5 && c.wake_voltage > c.sleep_voltage);
    char *red = cn_config_to_json(&c, true), *full = cn_config_to_json(&c, false);
    CHECK(!strstr(red, "hunter22") && strstr(full, "hunter22") && strstr(red, "\"pass_set\":true"));
    cn_config c2;
    cn_config_defaults(&c2);
    CHECK(cn_config_from_json(&c2, full) && c2.nwifi == 2 && !strcmp(c2.gb_api_key, "k"));
    CHECK(!cn_config_from_json(&c2, "not json"));
    free(red);
    free(full);
}

int main(void)
{
    static void (*const tests[])(void) = {test_util, test_bits_and_dtcs, test_expr, test_profiles, test_monitor_math,
                                          test_healthy_drive, test_faulty_drive, test_generic_car, test_clock_and_errors};
    FILE *quiet = freopen("/dev/null", "w", stderr); /* engine log chatter */
    (void)quiet;
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_fails;
        tests[i]();
        printf("%s %s\n", g_fails == before ? "ok  " : "FAIL", g_test);
    }
    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
