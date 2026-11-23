/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_platform.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL /* macOS has no per-call flag; SIGPIPE is ignored for the whole process instead */
#define MSG_NOSIGNAL 0
#endif

static int64_t p_mono(void *ctx)
{
    host_platform *h = ctx;
    if (h->stepped) return h->vt_ms;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double ms = (now.tv_sec - h->t0.tv_sec) * 1000.0 + (now.tv_nsec - h->t0.tv_nsec) / 1e6;
    return (int64_t)(ms * h->speed);
}

static void car_sync(host_platform *h)
{
    if (h->car) simcar_advance(h->car, p_mono(h) / 1000.0);
}

static int64_t p_wall(void *ctx)
{
    host_platform *h = ctx;
    return h->wall_base ? h->wall_base + p_mono(h) / 1000 : 0;
}

static void p_set_wall(void *ctx, int64_t unix_s)
{
    host_platform *h = ctx;
    if (h->hold_clock) return;
    h->wall_base = unix_s - p_mono(h) / 1000;
}

static void p_delay(void *ctx, int ms)
{
    host_platform *h = ctx;
    if (h->stepped) {
        h->vt_ms += ms;
        return;
    }
    double real = ms / h->speed;
    struct timespec ts = {(time_t)(real / 1000), (long)(fmod(real, 1000) * 1e6)};
    nanosleep(&ts, NULL);
}

static int p_can_open(void *ctx, int bitrate)
{
    (void)ctx;
    return bitrate == 500000 || bitrate == 250000 ? 0 : -1;
}

static void p_can_close(void *ctx) { (void)ctx; }

static int p_can_send(void *ctx, const cn_can_frame *f)
{
    host_platform *h = ctx;
    pthread_mutex_lock(&h->mu);
    car_sync(h);
    h->tx_frames++;
    if (h->car) simcar_rx(h->car, f);
    if (h->stepped) h->vt_ms += 1;
    pthread_mutex_unlock(&h->mu);
    return 0;
}

static int p_can_recv(void *ctx, cn_can_frame *f, int timeout_ms)
{
    host_platform *h = ctx;
    int64_t deadline = p_mono(h) + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&h->mu);
        car_sync(h);
        bool got = h->car && simcar_tx(h->car, f);
        if (got && h->stepped) h->vt_ms += 2; /* a little bus latency */
        pthread_mutex_unlock(&h->mu);
        if (got) return 1;
        if (h->stepped) {
            if (timeout_ms > 0) h->vt_ms = deadline;
            return 0;
        }
        if (p_mono(h) >= deadline) return 0;
        struct timespec ts = {0, 500000};
        nanosleep(&ts, NULL);
    }
}

static float p_vbat(void *ctx)
{
    host_platform *h = ctx;
    if (h->no_vbat || !h->car) return NAN;
    pthread_mutex_lock(&h->mu);
    car_sync(h);
    float v = (float)simcar_voltage(h->car);
    pthread_mutex_unlock(&h->mu);
    return v;
}

static char *p_kv_load(void *ctx, const char *key)
{
    host_platform *h = ctx;
    char path[300];
    snprintf(path, sizeof(path), "%s/kv-%s.json", h->state_dir, key);
    return cn_read_file(path, NULL);
}

static int p_kv_save(void *ctx, const char *key, const char *val)
{
    host_platform *h = ctx;
    char path[300];
    snprintf(path, sizeof(path), "%s/kv-%s.json", h->state_dir, key);
    return cn_write_file(path, val, strlen(val));
}

static void p_lock(void *ctx) { pthread_mutex_lock(&((host_platform *)ctx)->mu); }
static void p_unlock(void *ctx) { pthread_mutex_unlock(&((host_platform *)ctx)->mu); }

static const char *p_data_dir(void *ctx)
{
    host_platform *h = ctx;
    return h->has_storage ? h->sd_dir : NULL;
}

static int p_storage_info(void *ctx, uint64_t *total, uint64_t *free_bytes)
{
    host_platform *h = ctx;
    if (!h->has_storage) return -1;
    *total = h->sd_total;
    *free_bytes = h->sd_free;
    return 0;
}

static uint32_t p_random(void *ctx)
{
    host_platform *h = ctx;
    h->rng = h->rng * 1664525u + 1013904223u;
    return h->rng;
}

static int p_http(void *ctx, const cn_http_req *req, cn_http_resp *resp)
{
    host_platform *h = ctx;
    if (h->http_hook) return h->http_hook(h->http_ctx, req, resp);
    return host_http_request(ctx, req, resp);
}

host_platform *host_platform_create(simcar *car, const char *state_dir, bool stepped)
{
    host_platform *h = calloc(1, sizeof(*h));
    h->car = car;
    h->stepped = stepped;
    h->speed = 1.0;
    h->rng = 0xC0FFEE;
    clock_gettime(CLOCK_MONOTONIC, &h->t0);
    h->wall_base = stepped ? 1795478400 /* 2026-11-24T00:00:00Z */ : (int64_t)time(NULL);
    cn_strlcpy(h->state_dir, state_dir, sizeof(h->state_dir));
    snprintf(h->sd_dir, sizeof(h->sd_dir), "%s/sd", state_dir);
    cn_mkdirs(h->sd_dir);
    h->has_storage = true;
    h->sd_total = 32ull << 30;
    h->sd_free = 31ull << 30;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&h->mu, &attr);
    h->pf = (cn_platform){.ctx = h, .mono_ms = p_mono, .wall_s = p_wall, .set_wall_s = p_set_wall, .delay_ms = p_delay,
                          .can_open = p_can_open, .can_close = p_can_close, .can_send = p_can_send, .can_recv = p_can_recv,
                          .vbat = p_vbat, .http = p_http, .kv_load = p_kv_load, .kv_save = p_kv_save, .lock = p_lock,
                          .unlock = p_unlock, .data_dir = p_data_dir, .storage_info = p_storage_info, .random32 = p_random};
    return h;
}

void host_platform_free(host_platform *h)
{
    if (!h) return;
    pthread_mutex_destroy(&h->mu);
    free(h);
}

char *host_profile_loader(void *ctx, const char *id)
{
    const char *dir = ctx;
    char path[400];
    if (strcmp(id, CN_PROFILE_INDEX) != 0) {
        for (const char *c = id; *c; c++)
            if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')) return NULL;
        snprintf(path, sizeof(path), "%s/%s.json", dir, id);
        return cn_read_file(path, NULL);
    }
    cn_buf b;
    cn_buf_init(&b);
    cn_jw w;
    cn_jw_init(&w, &b);
    cn_jw_arr(&w);
    struct dirent **list;
    int n = scandir(dir, &list, NULL, alphasort);
    for (int i = 0; i < n; i++) {
        size_t len = strlen(list[i]->d_name);
        if (len > 5 && !strcmp(list[i]->d_name + len - 5, ".json")) {
            snprintf(path, sizeof(path), "%s/%s", dir, list[i]->d_name);
            char *doc = cn_read_file(path, NULL);
            if (doc) {
                char id2[64], name[96] = "", desc[300] = "";
                cn_strlcpy(id2, list[i]->d_name, sizeof(id2));
                id2[len - 5 < sizeof(id2) ? len - 5 : sizeof(id2) - 1] = 0;
                /* cheap field pull: the documents are trusted project files */
                const char *keys[2] = {"\"name\"", "\"description\""};
                char *outs[2] = {name, desc};
                size_t caps[2] = {sizeof(name), sizeof(desc)};
                for (int k = 0; k < 2; k++) {
                    char *p = strstr(doc, keys[k]);
                    if (!p) continue;
                    p = strchr(p + strlen(keys[k]), '"');
                    if (!p) continue;
                    p++;
                    size_t o = 0;
                    while (*p && *p != '"' && o + 1 < caps[k]) outs[k][o++] = *p++;
                    outs[k][o] = 0;
                }
                cn_jw_obj(&w);
                cn_jw_kstr(&w, "id", id2);
                cn_jw_kstr(&w, "name", name[0] ? name : id2);
                cn_jw_kstr(&w, "description", desc);
                cn_jw_end_obj(&w);
                free(doc);
            }
        }
        free(list[i]);
    }
    if (n >= 0) free(list);
    cn_jw_end_arr(&w);
    return cn_buf_take(&b);
}

/* ───────────── tiny HTTP client ───────────── */

static int64_t parse_http_date(const char *s)
{
    /* "Tue, 24 Nov 2026 01:02:03 GMT" */
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int d, y, hh, mm, ss;
    char mon[4];
    if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return 0;
    const char *m = strstr(months, mon);
    if (!m) return 0;
    struct tm tm = {.tm_year = y - 1900, .tm_mon = (int)(m - months) / 3, .tm_mday = d, .tm_hour = hh, .tm_min = mm, .tm_sec = ss};
    return (int64_t)timegm(&tm);
}

int host_http_request(void *ctx, const cn_http_req *req, cn_http_resp *resp)
{
    (void)ctx;
    memset(resp, 0, sizeof(*resp));
    char host[128], port[8] = "80";
    const char *u = req->url;
    if (strncmp(u, "http://", 7) != 0) return -1;
    u += 7;
    const char *slash = strchr(u, '/');
    size_t hl = slash ? (size_t)(slash - u) : strlen(u);
    if (hl >= sizeof(host)) return -1;
    memcpy(host, u, hl);
    host[hl] = 0;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        cn_strlcpy(port, colon + 1, sizeof(port));
    }
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *ai;
    if (getaddrinfo(host, port, &hints, &ai) != 0) return -1;
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    struct timeval tv = {req->timeout_ms > 0 ? req->timeout_ms / 1000 : 20, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        freeaddrinfo(ai);
        if (fd >= 0) close(fd);
        return -1;
    }
    freeaddrinfo(ai);
    cn_buf b;
    cn_buf_init(&b);
    cn_buf_printf(&b, "%s %s HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\nAccept: application/json\r\nUser-Agent: CANary/%s\r\n", req->method,
                  slash ? slash : "/", host, port, CN_FW_VERSION);
    if (req->api_key && req->api_key[0]) cn_buf_printf(&b, "x-api-key: %s\r\n", req->api_key);
    if (req->body_len || !strcmp(req->method, "POST") || !strcmp(req->method, "PUT"))
        cn_buf_printf(&b, "Content-Type: %s\r\nContent-Length: %zu\r\n", req->content_type ? req->content_type : "application/octet-stream",
                      req->body_len);
    cn_buf_puts(&b, "\r\n");
    bool ok = send(fd, b.p, b.len, MSG_NOSIGNAL) == (ssize_t)b.len;
    size_t off = 0;
    while (ok && off < req->body_len) {
        ssize_t n = send(fd, (const char *)req->body + off, req->body_len - off, MSG_NOSIGNAL);
        if (n <= 0) ok = false;
        else off += (size_t)n;
    }
    cn_buf_free(&b);
    cn_buf in;
    cn_buf_init(&in);
    char tmp[4096];
    ssize_t n;
    while (ok && (n = recv(fd, tmp, sizeof(tmp), 0)) > 0) cn_buf_put(&in, tmp, (size_t)n);
    close(fd);
    if (!ok || !in.p) {
        cn_buf_free(&in);
        return -1;
    }
    char *hdr_end = strstr(in.p, "\r\n\r\n");
    if (!hdr_end || sscanf(in.p, "HTTP/%*s %d", &resp->status) != 1) {
        cn_buf_free(&in);
        return -1;
    }
    *hdr_end = 0;
    char *date = strcasestr(in.p, "\r\nDate:");
    if (date) resp->server_time = parse_http_date(date + 7 + strspn(date + 7, " "));
    char *loc = strcasestr(in.p, "\r\nLocation:");
    if (loc) {
        loc += 11 + strspn(loc + 11, " ");
        size_t ll = strcspn(loc, "\r\n");
        if (ll >= sizeof(resp->location)) ll = sizeof(resp->location) - 1;
        memcpy(resp->location, loc, ll);
        resp->location[ll] = 0;
    }
    bool chunked = strcasestr(in.p, "Transfer-Encoding: chunked") != NULL;
    char *body = hdr_end + 4;
    size_t blen = in.len - (size_t)(body - in.p);
    cn_buf out;
    cn_buf_init(&out);
    if (chunked) {
        char *p = body, *end = body + blen;
        while (p < end) {
            long sz = strtol(p, &p, 16);
            char *nl = strstr(p, "\r\n");
            if (!nl || sz <= 0) break;
            p = nl + 2;
            if (p + sz > end) sz = end - p;
            cn_buf_put(&out, p, (size_t)sz);
            p += sz + 2;
        }
    } else {
        cn_buf_put(&out, body, blen);
    }
    cn_buf_free(&in);
    resp->body_len = out.len;
    resp->body = cn_buf_take(&out);
    return 0;
}
