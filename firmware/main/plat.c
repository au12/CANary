/* cn_platform for ESP32: time, CAN (TWAI), NVS, HTTP client, locking.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"

#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "plat";
static SemaphoreHandle_t s_lock;
static bool s_can_up;
static int s_can_bitrate;

static int64_t p_mono(void *ctx) { return esp_timer_get_time() / 1000; }

static int64_t p_wall(void *ctx)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (int64_t)t : 0;
}

static void p_set_wall(void *ctx, int64_t unix_s)
{
    struct timeval tv = {.tv_sec = (time_t)unix_s};
    settimeofday(&tv, NULL);
    bsp_rtc_write(unix_s);
}

static void p_delay(void *ctx, int ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

/* ───────────── CAN ───────────── */

static int can_start(int bitrate)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(BOARD_CAN_TX, BOARD_CAN_RX, TWAI_MODE_NORMAL);
    g.tx_queue_len = 8;
    g.rx_queue_len = 64;
    g.alerts_enabled = TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED | TWAI_ALERT_BUS_OFF;
    twai_timing_config_t t500 = TWAI_TIMING_CONFIG_500KBITS(), t250 = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, bitrate == 250000 ? &t250 : &t500, &f) != ESP_OK) return -1;
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return -1;
    }
#if BOARD_CAN_STBY >= 0
    gpio_set_level(BOARD_CAN_STBY, 0);
#endif
    s_can_up = true;
    s_can_bitrate = bitrate;
    return 0;
}

static void can_stop(void)
{
    if (!s_can_up) return;
    twai_stop();
    twai_driver_uninstall();
#if BOARD_CAN_STBY >= 0
    gpio_set_level(BOARD_CAN_STBY, 1);
#endif
    s_can_up = false;
}

static int p_can_open(void *ctx, int bitrate)
{
    if (bitrate != 500000 && bitrate != 250000) return -1;
    can_stop();
    return can_start(bitrate);
}

static void p_can_close(void *ctx) { can_stop(); }
void plat_can_shutdown(void) { can_stop(); }

static int p_can_send(void *ctx, const cn_can_frame *f)
{
    if (!s_can_up) return -1;
    twai_status_info_t st;
    if (twai_get_status_info(&st) == ESP_OK && st.state != TWAI_STATE_RUNNING) {
        /* bus-off or stopped: start over rather than wait out a recovery sequence */
        can_stop();
        if (can_start(s_can_bitrate) != 0) return -1;
    }
    twai_message_t m = {.identifier = f->id, .data_length_code = f->dlc};
    m.extd = f->ext ? 1 : 0;
    m.ss = 1; /* single shot: never keep hammering a bus that does not acknowledge */
    memcpy(m.data, f->data, 8);
    uint32_t alerts;
    twai_read_alerts(&alerts, 0); /* clear stale */
    if (twai_transmit(&m, pdMS_TO_TICKS(30)) != ESP_OK) return -1;
    if (twai_read_alerts(&alerts, pdMS_TO_TICKS(25)) != ESP_OK) return -1;
    return (alerts & TWAI_ALERT_TX_SUCCESS) ? 0 : -1;
}

static int p_can_recv(void *ctx, cn_can_frame *f, int timeout_ms)
{
    if (!s_can_up) {
        if (timeout_ms > 0) vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return 0;
    }
    twai_message_t m;
    for (;;) {
        esp_err_t err = twai_receive(&m, timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : 0);
        if (err == ESP_ERR_TIMEOUT) return 0;
        if (err != ESP_OK) return -1;
        if (m.rtr) continue;
        f->id = m.identifier;
        f->ext = m.extd;
        f->dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
        memset(f->data, 0, 8);
        memcpy(f->data, m.data, f->dlc);
        return 1;
    }
}

static float p_vbat(void *ctx) { return bsp_vbat(); }

/* ───────────── key/value store ───────────── */

static char *p_kv_load(void *ctx, const char *key)
{
    nvs_handle_t h;
    if (nvs_open("canary", NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t len = 0;
    char *out = NULL;
    if (nvs_get_blob(h, key, NULL, &len) == ESP_OK && len > 0 && len < 16384) {
        out = malloc(len + 1);
        if (out && nvs_get_blob(h, key, out, &len) == ESP_OK) {
            out[len] = 0;
        } else {
            free(out);
            out = NULL;
        }
    }
    nvs_close(h);
    return out;
}

static int p_kv_save(void *ctx, const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("canary", NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t err = nvs_set_blob(h, key, val, strlen(val));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    return err == ESP_OK ? 0 : -1;
}

static void p_lock(void *ctx) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void p_unlock(void *ctx) { xSemaphoreGiveRecursive(s_lock); }

static const char *p_data_dir(void *ctx) { return sd_mounted() ? "/sdcard/canary" : NULL; }
static int p_storage_info(void *ctx, uint64_t *total, uint64_t *free_bytes) { return sd_info(total, free_bytes); }
static uint32_t p_random(void *ctx) { return esp_random(); }

/* ───────────── HTTP client ───────────── */

typedef struct {
    cn_buf body;
    int64_t date;
} http_ctx;

static int64_t parse_http_date(const char *s)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int d, y, hh, mm, ss;
    char mon[4];
    if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6) return 0;
    const char *m = strstr(months, mon);
    if (!m || y < 2024) return 0;
    int mi = (int)(m - months) / 3;
    int64_t days = (int64_t)(y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400 + cum[mi] + d - 1;
    if (mi > 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

static esp_err_t http_event(esp_http_client_event_t *ev)
{
    http_ctx *c = ev->user_data;
    if (ev->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(ev->header_key, "Date")) {
        c->date = parse_http_date(ev->header_value);
    } else if (ev->event_id == HTTP_EVENT_ON_DATA && ev->data_len > 0 && c->body.len < 64 * 1024) {
        cn_buf_put(&c->body, ev->data, (size_t)ev->data_len);
    }
    return ESP_OK;
}

static int p_http(void *ctx, const cn_http_req *req, cn_http_resp *resp)
{
    memset(resp, 0, sizeof(*resp));
    http_ctx hc;
    memset(&hc, 0, sizeof(hc));
    cn_buf_init(&hc.body);
    esp_http_client_config_t cfg = {.url = req->url, .timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : 15000, .event_handler = http_event,
                                    .user_data = &hc, .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 2048,
                                    .buffer_size_tx = 1024, .keep_alive_enable = false};
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return -1;
    esp_http_client_set_method(cl, !strcmp(req->method, "POST") ? HTTP_METHOD_POST : !strcmp(req->method, "PUT") ? HTTP_METHOD_PUT : HTTP_METHOD_GET);
    esp_http_client_set_header(cl, "Accept", "application/json");
    esp_http_client_set_header(cl, "User-Agent", "CANary/" CN_FW_VERSION);
    if (req->api_key && req->api_key[0]) esp_http_client_set_header(cl, "x-api-key", req->api_key);
    if (req->body_len || strcmp(req->method, "GET") != 0) {
        esp_http_client_set_header(cl, "Content-Type", req->content_type ? req->content_type : "application/octet-stream");
        esp_http_client_set_post_field(cl, req->body ? req->body : "", (int)req->body_len);
    }
    esp_err_t err = esp_http_client_perform(cl);
    int rc = -1;
    if (err == ESP_OK) {
        resp->status = esp_http_client_get_status_code(cl);
        resp->server_time = hc.date;
        resp->body_len = hc.body.len;
        resp->body = cn_buf_take(&hc.body);
        rc = 0;
    } else {
        ESP_LOGW(TAG, "%s %s: %s", req->method, req->url, esp_err_to_name(err));
        cn_buf_free(&hc.body);
    }
    esp_http_client_cleanup(cl);
    return rc;
}

void plat_fill(cn_platform *pf)
{
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    *pf = (cn_platform){.mono_ms = p_mono, .wall_s = p_wall, .set_wall_s = p_set_wall, .delay_ms = p_delay, .can_open = p_can_open,
                        .can_close = p_can_close, .can_send = p_can_send, .can_recv = p_can_recv, .vbat = p_vbat, .http = p_http,
                        .kv_load = p_kv_load, .kv_save = p_kv_save, .lock = p_lock, .unlock = p_unlock, .data_dir = p_data_dir,
                        .storage_info = p_storage_info, .random32 = p_random};
}
