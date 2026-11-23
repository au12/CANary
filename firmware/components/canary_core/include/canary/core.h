/* CANary core — portable vehicle-health engine.
 * Copyright (C) 2026 CANary contributors. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything in canary_core is plain C11 with no hardware dependencies. A
 * platform (ESP32 firmware, desktop simulator, unit tests) plugs in through
 * cn_platform. The core never writes to the vehicle: only read services are
 * ever placed on the bus (see cn_diag_sid_allowed).
 */
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CN_FW_VERSION
#define CN_FW_VERSION "0.0.0-dev"
#endif

/* ───────────────────────────── util ───────────────────────────── */

typedef struct {
    char *p;
    size_t len, cap;
    bool oom;
} cn_buf;

void cn_buf_init(cn_buf *b);
void cn_buf_free(cn_buf *b);
void cn_buf_put(cn_buf *b, const char *s, size_t n);
void cn_buf_puts(cn_buf *b, const char *s);
void cn_buf_printf(cn_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *cn_buf_take(cn_buf *b); /* caller frees; never NULL unless oom */

/* Minimal streaming JSON writer on top of cn_buf. */
typedef struct {
    cn_buf *b;
    uint32_t first; /* bit per depth: no comma needed yet */
    int depth;
} cn_jw;

void cn_jw_init(cn_jw *w, cn_buf *b);
void cn_jw_obj(cn_jw *w);
void cn_jw_end_obj(cn_jw *w);
void cn_jw_arr(cn_jw *w);
void cn_jw_end_arr(cn_jw *w);
void cn_jw_key(cn_jw *w, const char *k);
void cn_jw_str(cn_jw *w, const char *s);
void cn_jw_num(cn_jw *w, double v, int decimals); /* NaN/inf → null */
void cn_jw_int(cn_jw *w, long long v);
void cn_jw_bool(cn_jw *w, bool v);
void cn_jw_null(cn_jw *w);
void cn_jw_raw(cn_jw *w, const char *json);
/* key + value shorthands */
void cn_jw_kstr(cn_jw *w, const char *k, const char *s);
void cn_jw_knum(cn_jw *w, const char *k, double v, int decimals);
void cn_jw_kint(cn_jw *w, const char *k, long long v);
void cn_jw_kbool(cn_jw *w, const char *k, bool v);

void cn_strlcpy(char *dst, const char *src, size_t n);
int cn_parse_hex(const char *s, uint8_t *out, int cap); /* returns bytes, <0 on error */
uint32_t cn_parse_u32(const char *s, int base, bool *ok);
uint32_t cn_crc32(uint32_t crc, const void *data, size_t len);
uint32_t cn_fnv1a(const void *data, size_t len, uint32_t seed);
void cn_iso8601(int64_t unix_s, char out[24]);
void cn_compact_time(int64_t unix_s, char out[20]); /* 20261122T233355Z */
bool cn_glob(const char *pattern, const char *s);   /* '?' any char, '*' any run */
char *cn_read_file(const char *path, size_t *len);  /* malloc'd, NUL terminated */
int cn_write_file(const char *path, const char *data, size_t len); /* atomic via .tmp */
int cn_mkdirs(const char *path);

/* ───────────────────────── platform interface ───────────────────────── */

typedef struct {
    uint32_t id;
    uint8_t ext; /* 29-bit identifier */
    uint8_t dlc;
    uint8_t data[8];
} cn_can_frame;

typedef struct {
    const char *method; /* "GET", "POST", "PUT" */
    const char *url;
    const char *api_key;      /* sent as x-api-key when set */
    const char *content_type; /* for body */
    const void *body;
    size_t body_len;
    int timeout_ms;
} cn_http_req;

typedef struct {
    int status;  /* HTTP status, or <0 on transport error */
    char *body;  /* malloc'd, NUL terminated (may be NULL) */
    size_t body_len;
    int64_t server_time; /* from Date header when available, else 0 */
} cn_http_resp;

typedef struct cn_platform {
    void *ctx;
    int64_t (*mono_ms)(void *ctx);
    int64_t (*wall_s)(void *ctx); /* unix seconds; 0 when the clock is not set */
    void (*set_wall_s)(void *ctx, int64_t unix_s);
    void (*delay_ms)(void *ctx, int ms);
    /* CAN */
    int (*can_open)(void *ctx, int bitrate);
    void (*can_close)(void *ctx);
    int (*can_send)(void *ctx, const cn_can_frame *f);           /* 0 ok, <0 error */
    int (*can_recv)(void *ctx, cn_can_frame *f, int timeout_ms); /* 1 frame, 0 timeout, <0 error */
    /* supply voltage at the OBD port; NaN when the board cannot measure it */
    float (*vbat)(void *ctx);
    /* outbound HTTP; returns 0 when a response was received */
    int (*http)(void *ctx, const cn_http_req *req, cn_http_resp *resp);
    /* small persistent key/value store (NVS on the device) */
    char *(*kv_load)(void *ctx, const char *key);
    int (*kv_save)(void *ctx, const char *key, const char *val);
    /* recursive lock protecting engine state */
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    /* bulk storage (SD card). data_dir may be NULL when no card is mounted. */
    const char *(*data_dir)(void *ctx);
    int (*storage_info)(void *ctx, uint64_t *total, uint64_t *free_bytes);
    uint32_t (*random32)(void *ctx);
} cn_platform;

/* ───────────────────────────── ISO-TP ───────────────────────────── */

#define CN_ERR_TIMEOUT (-1)
#define CN_ERR_BUS (-2)
#define CN_ERR_PROTO (-3)
#define CN_ERR_OVERFLOW (-4)
#define CN_ERR_DENIED (-5)
#define CN_ERR_NRC(n) (-(0x100 + (int)(n)))
#define CN_IS_NRC(e) ((e) <= -0x100)
#define CN_NRC_OF(e) ((-(e)) - 0x100)

#define CN_ISOTP_MAX 1024

typedef struct {
    uint32_t tx; /* request identifier */
    uint32_t rx; /* expected response identifier, 0 = any OBD responder */
    bool ext;    /* 29-bit identifiers */
} cn_addr;

typedef struct {
    uint32_t id;
    int len;
    uint8_t data[CN_ISOTP_MAX];
} cn_isotp_msg;

/* One request, one response. Returns response length or a CN_ERR_*.
 * Negative responses come back as CN_ERR_NRC(code); 0x78 (pending) is waited out. */
int cn_isotp_request(const cn_platform *pf, const cn_addr *a, const uint8_t *req, int req_len, uint8_t *resp,
                     int resp_cap, int timeout_ms, uint32_t *resp_id);
/* Functional request collecting every responder for timeout_ms. Returns count. */
int cn_isotp_request_multi(const cn_platform *pf, const cn_addr *a, const uint8_t *req, int req_len,
                           cn_isotp_msg *out, int max, int timeout_ms);

/* ───────────────────────────── diagnostics ───────────────────────────── */

/* Read-only allow-list of service identifiers. */
bool cn_diag_sid_allowed(uint8_t sid);

typedef struct {
    char code[8];   /* P0299 */
    uint8_t ftb;    /* UDS failure type byte, 0 for OBD-II modes */
    uint8_t status; /* UDS status byte, 0 for OBD-II modes */
    uint8_t kind;   /* CN_DTC_* */
    uint8_t ecu;    /* index into profile ecus, 0xFF = by responder id */
    uint32_t src;   /* responder CAN id */
} cn_dtc;

enum { CN_DTC_STORED = 1, CN_DTC_PENDING = 2, CN_DTC_PERMANENT = 3 };

void cn_dtc_format(uint8_t hi, uint8_t lo, char out[8]);
/* Parse an OBD-II mode 03/07/0A positive response (CAN format). */
int cn_dtc_parse_obd(const uint8_t *resp, int len, uint8_t kind, uint32_t src, cn_dtc *out, int max);
/* Parse UDS 0x59 0x02 response. */
int cn_dtc_parse_uds(const uint8_t *resp, int len, uint32_t src, uint8_t ecu, cn_dtc *out, int max);
/* Extract bits (MSB-first numbering) from a payload. Returns false when out of range. */
bool cn_extract_bits(const uint8_t *data, int data_len, int bit, int nbits, bool sign, bool lsb, double *out);

/* ───────────────────────────── expressions ───────────────────────────── */

typedef struct cn_expr cn_expr;
/* Resolves an identifier to a variable index; return <0 for unknown names. */
typedef int (*cn_expr_resolver)(void *ctx, const char *name);

cn_expr *cn_expr_compile(const char *src, cn_expr_resolver resolve, void *ctx, char *err, size_t err_len);
void cn_expr_free(cn_expr *e);
/* vars[i] may be NaN for "not available"; the result is then NaN. */
double cn_expr_eval(const cn_expr *e, const double *vars);
/* Calls fn for every variable index the expression references. */
void cn_expr_vars(const cn_expr *e, void (*fn)(void *ctx, int var), void *ctx);

/* ───────────────────────────── profile model ───────────────────────────── */

#define CN_ID_LEN 28
#define CN_MAX_ECUS 16
#define CN_MAX_SRC 4
#define CN_MAX_SIGNALS 160
#define CN_MAX_REQUESTS 200
#define CN_MAX_MONITORS 64
#define CN_MAX_LEVELS 4
#define CN_MAX_CAUSES 6
#define CN_HIST 90
#define CN_HIST_STEP_MS 2000
#define CN_RESERVOIR 128
#define CN_MAX_DTCS 48

enum { CN_RATE_FAST = 0, CN_RATE_MED, CN_RATE_SLOW, CN_RATE_RARE, CN_RATE_ONCE, CN_RATE__N };
enum { CN_SRC_OBD = 1, CN_SRC_UDS, CN_SRC_DEVICE, CN_SRC_CALC };
enum { CN_SUP_UNKNOWN = 0, CN_SUP_OK, CN_SUP_NO };
enum { CN_DEV_VBAT = 1, CN_DEV_TRIP_TIME };
enum { CN_SEV_OK = 0, CN_SEV_ADVISORY, CN_SEV_WARNING, CN_SEV_CRITICAL, CN_SEV_NODATA = 0xFF };
enum {
    CN_STAT_MEAN = 0, CN_STAT_MIN, CN_STAT_MAX, CN_STAT_P50, CN_STAT_P90, CN_STAT_P95, CN_STAT_P99,
    CN_STAT_LAST, CN_STAT_FIRST, CN_STAT_DELTA, CN_STAT_RANGE, CN_STAT_TIME, CN_STAT_FRACTION,
    CN_STAT_TIME_TO, CN_STAT_COUNT, CN_STAT__N
};
enum { CN_OP_GT = 0, CN_OP_GE, CN_OP_LT, CN_OP_LE };

extern const int cn_rate_period_ms[CN_RATE__N];
extern const int cn_rate_stale_ms[CN_RATE__N];
const char *cn_rate_name(int rate);
const char *cn_sev_name(int sev);
const char *cn_stat_name(int stat);

typedef struct {
    char id[16];
    char name[40];
    uint32_t tx, rx;
    bool ext;
    bool dtc; /* include in UDS trouble-code sweeps */
} cn_ecu;

typedef struct {
    uint8_t kind;
    uint8_t ecu;
    uint8_t mode;  /* OBD mode, or 0x22 for UDS */
    uint8_t len;   /* bits */
    uint16_t code; /* PID, DID or CN_DEV_* */
    uint16_t bit;
    bool sign, lsb;
    double mul, div, add;
    cn_expr *calc;
    int16_t req; /* index into requests, -1 for device/calc */
} cn_source;

typedef struct {
    char id[CN_ID_LEN];
    char name[52];
    char unit[12];
    char group[16];
    uint8_t rate, dec, nsrc;
    int8_t active; /* selected source, -1 none yet */
    bool log;
    bool has_range;
    double lo, hi; /* plausibility range; outside is discarded */
    cn_source src[CN_MAX_SRC];
    /* runtime */
    bool valid;
    double value;
    int64_t t_ms;
    uint32_t n;
    double sum, vmin, vmax;
    float hist[CN_HIST];
    uint8_t hist_n, hist_head;
    int64_t hist_t;
} cn_signal;

typedef struct {
    uint8_t ecu, mode, rate, sup;
    uint16_t code;
    uint8_t fails;
    bool needed;
    int64_t next_ms;
    int64_t last_ok_ms;
    uint32_t ok_count, err_count;
    int16_t last_err;
} cn_request;

typedef struct {
    uint8_t sev, op;
    double thr;
} cn_level;

typedef struct {
    char id[CN_ID_LEN];
    char title[64];
    char category[16];
    char unit[12];
    uint8_t stat, dec, nlevels, ncauses;
    float min_time_s;
    float settle_s; /* the gate must have held this long before samples count (skips turbo spool-up and the like) */
    cn_level levels[CN_MAX_LEVELS];
    cn_expr *when, *value;
    char *summary; /* "{value}" is replaced with the result */
    char *ok_text;
    char *detail;
    char *causes[CN_MAX_CAUSES];
    bool enabled;
    char why_disabled[48];
    bool unavailable;        /* a signal it needs is not reported by this car */
    char missing[CN_ID_LEN]; /* that signal */
    /* accumulators (reset per trip) */
    uint32_t n, n_true, edges, gate_run;
    bool prev_true, reached;
    double sum, vmin, vmax, first, last;
    float time_to_s;
    float res[CN_RESERVOIR];
    uint32_t res_seen, rng;
    /* result */
    uint8_t status;
    bool has_result;
    double result;
} cn_monitor;

typedef struct {
    char id[40];
    char name[64];
    char hash[12];
    int can_bitrate; /* 0 = auto */
    int can_id_bits; /* 0 = auto, 11 or 29 */
    int necus, nsignals, nrequests, nmonitors;
    cn_ecu ecus[CN_MAX_ECUS];
    cn_signal *signals;   /* [CN_MAX_SIGNALS] */
    cn_request *requests; /* [CN_MAX_REQUESTS] */
    cn_monitor *monitors; /* [CN_MAX_MONITORS] */
    char *resolved_json;  /* merged profile, uploaded to the server for report generation */
} cn_profile;

/* Source of profile documents by id (built-in table, SD card, tests…). Returns malloc'd JSON or NULL. */
typedef char *(*cn_profile_loader)(void *ctx, const char *id);

cn_profile *cn_profile_load(const char *id, cn_profile_loader load, void *ctx, char *err, size_t err_len);
void cn_profile_free(cn_profile *p);
int cn_profile_signal_index(const cn_profile *p, const char *id);
/* Does a profile document's "match" block accept this VIN? Returns a score (0 = no match). */
int cn_profile_match_vin(const char *profile_json, const char *vin);

/* ───────────────────────────── configuration ───────────────────────────── */

#define CN_MAX_WIFI 4

typedef struct {
    char ssid[33];
    char pass[65];
} cn_wifi_net;

typedef struct {
    char device_name[32];
    cn_wifi_net wifi[CN_MAX_WIFI];
    int nwifi;
    char ap_pass[65];
    int ap_mode; /* 0 = only when no Wi-Fi is configured or on button press, 1 = always */
    char ui_password[40];
    /* Glovebox / LubeLogger */
    char gb_url[128];
    char gb_api_key[80];
    int gb_vehicle_id;
    bool gb_upload_logs;
    char odo_unit[4]; /* "mi" or "km": used when talking to a plain LubeLogger server */
    double odo_offset;
    /* vehicle */
    char profile[40]; /* "auto" or a profile id */
    /* power */
    bool sleep_enabled;
    double sleep_voltage, wake_voltage;
    int sleep_delay_s;
    /* logging */
    bool log_enabled;
    int retention_days;
    bool units_imperial;
    int tz_offset_min; /* local time = UTC + this; kept current by the web UI */
} cn_config;

void cn_config_defaults(cn_config *c);
bool cn_config_from_json(cn_config *c, const char *json);  /* missing keys keep current values */
char *cn_config_to_json(const cn_config *c, bool redact); /* redact hides secrets for the UI */

/* ───────────────────────────── engine ───────────────────────────── */

enum { CN_VS_INIT = 0, CN_VS_DETECT, CN_VS_IDENT, CN_VS_RUN, CN_VS_KOEO, CN_VS_OFF };
const char *cn_vs_name(int vs);

typedef struct {
    int64_t t_wall, t_mono;
    char level; /* 'i', 'w', 'e' */
    char text[112];
} cn_event;

#define CN_EVENTS 160

typedef struct {
    char id[36];
    int64_t start_wall;
    int duration_s;
    double distance_km, odo_start_km, odo_end_km, max_speed, fuel_l;
    uint32_t samples;
    uint64_t bytes, uploaded_bytes;
    int64_t uploaded_at;
    uint8_t worst;      /* worst finding severity */
    uint8_t nfindings;  /* findings above ok */
    bool raw_deleted;
    int64_t end_mono;   /* this boot only: lets a trip recorded before the clock was set be dated later */
} cn_trip_info;

#define CN_TRIP_CACHE 240

typedef struct {
    bool active;
    char id[36];
    int64_t start_mono, start_wall;
    FILE *f;
    char path[160];
    uint32_t samples;
    uint64_t bytes;
    int64_t last_flush_ms, last_tick_ms, last_dist_ms;
    double distance_km, odo_start_km, max_speed, fuel_l;
    int64_t rpm_zero_since;
    bool dtc_logged;
} cn_trip;

typedef struct {
    bool connected;
    char ssid[33];
    char ip[20];
    int rssi;
    bool ap_active;
    char ap_ssid[33];
} cn_net_status;

enum { CN_SYNC_NONE = 0, CN_SYNC_LUBELOGGER, CN_SYNC_CANARY };

typedef struct {
    int mode;
    bool busy, force;
    int64_t last_attempt_ms, last_ok_wall, connected_since_ms;
    bool did_connect_sync; /* one sync per Wi-Fi connection */
    char last_error[96];
    char uploading[36];
    uint64_t up_done, up_total;
    double last_odo_sent_km;
    int64_t last_odo_sent_wall;
    int pending_trips;
    char server_profile_hash[12];
} cn_sync;

typedef struct {
    bool pending, done;
    cn_addr addr;
    uint8_t req[8];
    int req_len;
    int result;
    uint32_t resp_id;
    uint8_t resp[256];
    int elapsed_ms;
} cn_adhoc;

typedef struct cn_engine {
    cn_platform pf;
    cn_config cfg;
    cn_profile *profile;
    cn_profile_loader loader;
    void *loader_ctx;
    char device_id[20];
    char board[24];

    /* vehicle */
    int vs;
    int64_t vs_since_ms;
    cn_addr obd;      /* functional addressing in use */
    int can_bitrate;
    bool can_ext;
    bool can_open;
    char vin[18];
    bool vin_tried;
    uint32_t pid_support[7]; /* mode 01 bitmaps for 0x01-0xE0 */
    bool pid_support_known;
    int rpm_sig, speed_sig, odo_sig, fuel_rate_sig;
    int probe_pending;
    int rr_cursor;
    int consecutive_timeouts;
    int64_t last_response_ms, last_probe_ms, last_dtc_ms, off_since_ms;
    float vbat, vbat_min_off;
    int64_t vbat_ms;
    bool wake_armed;
    double *vars; /* latest values for expressions, NaN when stale */
    double last_odo_km;
    int64_t last_odo_wall;

    cn_dtc dtcs[CN_MAX_DTCS];
    int ndtcs;
    bool mil;
    int64_t dtc_read_wall;
    uint32_t dtc_sig; /* hash of the last set, to detect changes */

    cn_trip trip;
    cn_trip_info trips[CN_TRIP_CACHE];
    int ntrips;
    char last_trip_id[36];

    cn_net_status net;
    cn_sync sync;
    cn_adhoc adhoc;

    cn_event events[CN_EVENTS];
    int ev_head, ev_count;
    FILE *ev_file;

    int64_t boot_mono, last_ui_ms;
    bool reboot_requested, format_requested;
    uint32_t bus_tx, bus_rx, bus_err;

    /* internals */
    int dtc_phase;
    cn_dtc dtc_tmp[CN_MAX_DTCS];
    int ndtc_tmp;
    uint32_t pick_count, boot_count;
    bool has_vbat, wake_request, support_dirty, heartbeat_logged;
    int mil_sig, detect_fails;
    int64_t last_derived_ms, last_eval_ms, last_retention_ms, last_state_save_ms, last_can_open_ms;
    char state_profile[40]; /* profile used last time, so the right one loads before the VIN is read */
    uint32_t cfg_gen;       /* bumped when settings change; the platform re-reads Wi-Fi settings */
    bool profile_change_pending;
} cn_engine;

int cn_dtc_severity(const cn_engine *e);
void cn_json_findings(cn_engine *e, cn_jw *w); /* monitors with a result */
void cn_json_dtcs(cn_engine *e, cn_jw *w);
/* Lists the profiles a loader knows: load(ctx, "@index") returns [{"id":..,"name":..}, …]. */
#define CN_PROFILE_INDEX "@index"
void cn_engine_reload_profile(cn_engine *e);

cn_engine *cn_engine_create(const cn_platform *pf, cn_profile_loader loader, void *loader_ctx, const char *device_id,
                            const char *board);
void cn_engine_destroy(cn_engine *e);
void cn_engine_save_config(cn_engine *e);
/* Vehicle side: call in a loop from one task. Blocks at most a few hundred ms. */
void cn_vehicle_step(cn_engine *e);
/* Server side: call every second or so from another task. */
void cn_sync_step(cn_engine *e);
void cn_net_update(cn_engine *e, const cn_net_status *st);
void cn_log(cn_engine *e, char level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Power advice for the platform: true when it is safe and sensible to sleep now. */
bool cn_power_can_sleep(cn_engine *e, char *why, size_t why_len);
/* While sleeping, the platform samples the supply voltage and asks whether to wake. */
bool cn_power_should_wake(const cn_config *cfg, float vbat, float vbat_min_asleep);

/* signals / monitors */
void cn_signal_update(cn_engine *e, int idx, double value, int64_t now_ms);
void cn_monitors_reset(cn_profile *p);
void cn_monitors_tick(cn_profile *p, const double *vars, double trip_time_s, double dt_s);
void cn_monitors_evaluate(cn_profile *p, double trip_time_s);
double cn_monitor_stat(const cn_monitor *m, double trip_time_s, bool *ok);
void cn_monitor_text(const cn_monitor *m, char *out, size_t n);
int cn_worst_severity(const cn_engine *e, int *count_above_ok);

/* trips */
void cn_trip_begin(cn_engine *e);
void cn_trip_sample(cn_engine *e, int sig, double value, int64_t now_ms);
void cn_trip_event(cn_engine *e, const char *kind, const char *json);
void cn_trip_end(cn_engine *e, const char *reason);
void cn_trips_scan(cn_engine *e);
void cn_trips_retention(cn_engine *e);
char *cn_trip_path(cn_engine *e, const char *trip_id, const char *ext);
int cn_trip_mark_uploaded(cn_engine *e, const char *trip_id, uint64_t bytes, bool complete);
void cn_trips_fix_clock(cn_engine *e); /* call once the wall clock becomes known */

/* ───────────────────────────── HTTP API ───────────────────────────── */

typedef struct {
    const char *method;
    const char *path;  /* without query string */
    const char *query; /* may be NULL */
    const char *body;  /* NUL terminated, may be NULL */
    size_t body_len;
} cn_api_req;

typedef struct {
    int status;
    const char *content_type;
    cn_buf body;
    char send_file[160]; /* when set the platform streams this file instead of body */
    char download_name[64];
} cn_api_resp;

/* Returns false when the path is not an API route (platform serves static files). */
bool cn_api_handle(cn_engine *e, const cn_api_req *req, cn_api_resp *resp);
bool cn_query_get(const char *query, const char *key, char *out, size_t n);

#ifdef __cplusplus
}
#endif
