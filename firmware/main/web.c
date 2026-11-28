/* HTTP server: the JSON API from the core, the embedded web UI, firmware
 * update and a captive portal for first-time setup.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "lwip/sockets.h"

static const char *TAG = "web";
static httpd_handle_t s_httpd;

static const char k_recovery_page[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>CANary recovery</title>"
    "<style>body{font:16px system-ui;margin:2rem auto;max-width:34rem;padding:0 1rem;color:#1c1d1f}button{font:inherit;padding:.6rem 1rem}"
    "code{background:#eee;padding:0 .3em}</style><h1>CANary recovery</h1>"
    "<p>The device started in recovery mode. Choose a firmware file (<code>.bin</code>) to install. CANary and the stock WiCAN Pro firmware both work.</p>"
    "<input type=file id=f accept='.bin'> <button onclick='go()'>Install</button><p id=s></p>"
    "<script>async function go(){const f=document.getElementById('f').files[0],s=document.getElementById('s');if(!f)return;"
    "s.textContent='Uploading…';try{const r=await fetch('/api/ota',{method:'POST',body:f});const j=await r.json();"
    "s.textContent=j.ok?'Installed. The device is restarting.':'Failed: '+j.error}catch(e){s.textContent='Upload failed: '+e}}</script>";

static void json_error(httpd_req_t *req, const char *status, const char *msg)
{
    char body[200];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
}

static bool authorized(httpd_req_t *req)
{
    if (!g_engine || !g_engine->cfg.ui_password[0]) return true;
    char hdr[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK && !strncmp(hdr, "Basic ", 6)) {
        unsigned char dec[192]; /* "name:password": a long remembered user name must not lock anyone out */
        size_t n = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[n] = 0;
            const char *pass = strchr((const char *)dec, ':');
            if (pass && !strcmp(pass + 1, g_engine->cfg.ui_password)) return true;
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"CANary\"");
    httpd_resp_sendstr(req, "Password required");
    return false;
}

static esp_err_t stream_file(httpd_req_t *req, const char *path, const char *type, const char *download)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        json_error(req, "404 Not Found", "not found");
        return ESP_OK;
    }
    char disp[120];
    httpd_resp_set_type(req, type);
    if (download && download[0]) {
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", download);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }
    char *buf = malloc(4096);
    size_t n;
    esp_err_t err = ESP_OK;
    while (buf && err == ESP_OK && (n = fread(buf, 1, 4096, f)) > 0) err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
    free(buf);
    fclose(f);
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t handle_ota(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h = 0;
    char *buf = malloc(4096);
    const char *error = NULL;
    if (!part || !buf) error = "no update partition";
    if (!error && req->content_len < 64 * 1024) error = "that file is too small to be firmware";
    if (!error && (size_t)req->content_len > part->size) error = "firmware is too large for this device";
    if (!error && g_engine && g_engine->trip.active) cn_trip_end(g_engine, "firmware update");
    if (!error && esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) error = "could not start the update";
    int remaining = req->content_len, stalls = 0;
    bool first = true;
    while (!error && remaining > 0) {
        int n = httpd_req_recv(req, buf, MIN(remaining, 4096));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++stalls > 3) error = "upload interrupted";
            continue;
        }
        stalls = 0;
        if (n <= 0) {
            error = "upload interrupted";
            break;
        }
        if (first && (uint8_t)buf[0] != 0xE9) {
            error = "not an ESP32 firmware image";
            break;
        }
        first = false;
        if (esp_ota_write(h, buf, (size_t)n) != ESP_OK) error = "write to flash failed";
        remaining -= n;
    }
    free(buf);
    if (!error && esp_ota_end(h) != ESP_OK) error = "the image failed verification";
    else if (error && h) esp_ota_abort(h);
    if (!error && esp_ota_set_boot_partition(part) != ESP_OK) error = "could not switch to the new firmware";
    if (error) {
        ESP_LOGW(TAG, "update failed: %s", error);
        json_error(req, "400 Bad Request", error);
        return ESP_OK;
    }
    if (g_engine) {
        cn_log(g_engine, 'i', "Firmware installed to %s; restarting", part->label);
        g_engine->reboot_requested = true;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    if (!g_engine) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    return ESP_OK;
}

static void system_json(httpd_req_t *req)
{
    static const char *const reasons[] = {"unknown", "power on", "external", "software", "panic", "interrupt watchdog", "task watchdog",
                                          "other watchdog", "deep sleep", "brownout", "sdio"};
    esp_reset_reason_t rr = esp_reset_reason();
    const esp_partition_t *run = esp_ota_get_running_partition();
    cn_buf b;
    cn_buf_init(&b);
    cn_jw w;
    cn_jw_init(&w, &b);
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "fw", CN_FW_VERSION);
    cn_jw_kstr(&w, "build", g_web_build);
    cn_jw_kstr(&w, "idf", esp_get_idf_version());
    cn_jw_kstr(&w, "board", BOARD_NAME);
    cn_jw_kstr(&w, "device", g_device_id);
    cn_jw_kbool(&w, "recovery", g_recovery);
    cn_jw_kstr(&w, "reset_reason", rr < sizeof(reasons) / sizeof(reasons[0]) ? reasons[rr] : "unknown");
    cn_jw_kstr(&w, "partition", run ? run->label : "");
    cn_jw_kint(&w, "heap_free", (long long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cn_jw_kint(&w, "heap_min", (long long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cn_jw_kint(&w, "psram_free", (long long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cn_jw_kstr(&w, "obd_chip", bsp_chip_version());
    cn_jw_kstr(&w, "sd_card", sd_card_name());
    cn_jw_kbool(&w, "sd_mounted", sd_mounted());
    cn_jw_kint(&w, "uptime_s", esp_timer_get_time() / 1000000);
    cn_jw_end_obj(&w);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, b.p ? b.p : "{}", (ssize_t)b.len);
    cn_buf_free(&b);
}

static esp_err_t handle_api(httpd_req_t *req, const char *path, const char *query)
{
    bool post = req->method == HTTP_POST;
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (post && !strcmp(path, "/api/ota")) return handle_ota(req);
    if (!post && !strcmp(path, "/api/system")) {
        system_json(req);
        return ESP_OK;
    }
    if (!g_engine) {
        json_error(req, "503 Service Unavailable", "recovery mode");
        return ESP_OK;
    }
    if (!post && !strcmp(path, "/api/wifi/scan")) {
        char *json = net_scan_json();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json ? json : "{\"networks\":[]}");
        free(json);
        return ESP_OK;
    }
    if (post && !strcmp(path, "/api/ap")) {
        net_ap_enable(15);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");
        return ESP_OK;
    }
    char *body = NULL;
    if (post && req->content_len > 0) {
        if (req->content_len > 32 * 1024) {
            json_error(req, "413 Payload Too Large", "request too large");
            return ESP_OK;
        }
        body = malloc(req->content_len + 1);
        int got = 0, stalls = 0;
        while (body && got < (int)req->content_len) {
            int n = httpd_req_recv(req, body + got, req->content_len - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++stalls <= 3) continue;
            if (n <= 0) break;
            got += n;
        }
        if (!body || got < (int)req->content_len) {
            free(body);
            json_error(req, "400 Bad Request", "incomplete request");
            return ESP_OK;
        }
        body[got] = 0;
    }
    cn_api_req areq = {.method = post ? "POST" : "GET", .path = path, .query = query, .body = body, .body_len = body ? req->content_len : 0};
    cn_api_resp resp;
    memset(&resp, 0, sizeof(resp));
    esp_err_t err = ESP_OK;
    if (!cn_api_handle(g_engine, &areq, &resp)) {
        json_error(req, "404 Not Found", "no such endpoint");
    } else if (resp.send_file[0]) {
        err = stream_file(req, resp.send_file, resp.content_type, resp.download_name);
    } else {
        char status[8];
        snprintf(status, sizeof(status), "%d", resp.status);
        if (resp.status != 200) httpd_resp_set_status(req, status);
        httpd_resp_set_type(req, resp.content_type);
        err = httpd_resp_send(req, resp.body.p ? resp.body.p : "", (ssize_t)resp.body.len);
    }
    cn_buf_free(&resp.body);
    free(body);
    return err;
}

/* Did this request come in through our own access point (192.168.4.x)? */
static bool from_access_point(httpd_req_t *req)
{
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) return false;
    const uint8_t *ip4 = NULL;
    if (addr.sin6_family == AF_INET) {
        ip4 = (const uint8_t *)&((struct sockaddr_in *)&addr)->sin_addr;
    } else if (addr.sin6_family == AF_INET6) {
        const uint8_t *b = (const uint8_t *)&addr.sin6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        if (!memcmp(b, mapped, 12)) ip4 = b + 12;
    }
    return ip4 && ip4[0] == 192 && ip4[1] == 168 && ip4[2] == 4;
}

static esp_err_t handler(httpd_req_t *req)
{
    char uri[CONFIG_HTTPD_MAX_URI_LEN + 1];
    strlcpy(uri, req->uri, sizeof(uri));
    char *query = strchr(uri, '?');
    if (query) *query++ = 0;

    /* Captive portal: a phone that just joined the access point asks for some outside host. Send it to us. */
    char host[64] = "";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    bool ours = !host[0] || !strncmp(host, "192.168.", 8) || !strncmp(host, "10.", 3) || !strncmp(host, "172.", 4) ||
                !strncasecmp(host, "canary", 6);
    if (!ours && req->method == HTTP_GET && from_access_point(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    if (!authorized(req)) return ESP_OK;
    if (!strncmp(uri, "/api/", 5)) return handle_api(req, uri, query);
    if (req->method != HTTP_GET) {
        json_error(req, "405 Method Not Allowed", "not allowed");
        return ESP_OK;
    }
    if (g_recovery) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, k_recovery_page, HTTPD_RESP_USE_STRLEN);
    }
    const char *want = !strcmp(uri, "/") ? "/index.html" : uri;
    for (int i = 0; i < g_web_asset_count; i++) {
        if (strcmp(g_web_assets[i].path, want) != 0) continue;
        httpd_resp_set_type(req, g_web_assets[i].type);
        if (g_web_assets[i].gzip) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", !strcmp(want, "/index.html") ? "no-cache" : "max-age=86400");
        return httpd_resp_send(req, (const char *)g_web_assets[i].data, (ssize_t)g_web_assets[i].len);
    }
    /* unknown paths get the app (client-side routing) */
    if (!strchr(want, '.')) {
        httpd_resp_set_type(req, g_web_assets[0].type);
        if (g_web_assets[0].gzip) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        return httpd_resp_send(req, (const char *)g_web_assets[0].data, (ssize_t)g_web_assets[0].len);
    }
    httpd_resp_send_404(req);
    return ESP_OK;
}

void web_start(void)
{
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 12288;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        s_httpd = NULL;
        return;
    }
    static const httpd_uri_t get = {.uri = "/*", .method = HTTP_GET, .handler = handler};
    static const httpd_uri_t post = {.uri = "/*", .method = HTTP_POST, .handler = handler};
    httpd_register_uri_handler(s_httpd, &get);
    httpd_register_uri_handler(s_httpd, &post);
}

void web_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
}
