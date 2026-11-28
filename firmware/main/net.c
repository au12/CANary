/* Wi-Fi: joins the strongest known network, offers its own access point for
 * setup, announces itself as canary.local and keeps the clock set.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"

#include <string.h>
#include <sys/time.h>

static const char *TAG = "net";

static esp_netif_t *s_sta_if, *s_ap_if;
static bool s_started, s_connected, s_connecting, s_ap_on, s_ever_connected, s_sntp_started, s_mdns_started;
static char s_ip[20], s_ssid[33], s_ap_ssid[33];
static int s_rssi, s_fails, s_ap_clients, s_blind;
static int64_t s_ap_until_ms, s_next_try_ms, s_connect_started_ms;
static uint32_t s_cfg_gen = 0xFFFFFFFF;
static wifi_mode_t s_mode = WIFI_MODE_NULL;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* Runs in the network stack's own task with a small stack: only note the time here, and let
 * net_poll (sync task) write the clock chip and fix up undated drives on the SD card. */
static volatile int64_t s_synced_s;

static void on_time_sync(struct timeval *tv) { s_synced_s = tv->tv_sec; }

static void after_time_sync(void)
{
    if (!s_synced_s) return;
    s_synced_s = 0;
    struct timeval now;
    gettimeofday(&now, NULL);
    bsp_rtc_write(now.tv_sec);
    if (g_engine) {
        g_engine->pf.lock(g_engine->pf.ctx);
        cn_trips_fix_clock(g_engine);
        g_engine->pf.unlock(g_engine->pf.ctx);
    }
}

/* Whether any of the configured networks has ever been joined, remembered across restarts, so the
 * rescue access point opens after a password mistake but not on every drive away from home. */
static int s_wifi_ok = -1;

static bool wifi_ok_get(void)
{
    if (s_wifi_ok < 0) {
        uint8_t v = 0;
        nvs_handle_t h;
        if (nvs_open("canary", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "wifi_ok", &v);
            nvs_close(h);
        }
        s_wifi_ok = v;
    }
    return s_wifi_ok == 1;
}

static void wifi_ok_set(bool ok)
{
    if (s_wifi_ok == (ok ? 1 : 0)) return;
    s_wifi_ok = ok ? 1 : 0;
    nvs_handle_t h;
    if (nvs_open("canary", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "wifi_ok", ok ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_connecting = false;
        s_ip[0] = 0;
        const wifi_event_sta_disconnected_t *ev = data;
        int reason = ev ? ev->reason : 0;
        if (reason == WIFI_REASON_NO_AP_FOUND || (reason >= WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY && reason <= WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD)) {
            /* not there (usually a look for a hidden network while away): no reason to back off */
            s_next_try_ms = now_ms() + 20000;
        } else {
            s_fails++;
            int backoff = s_fails < 3 ? 5000 : s_fails < 6 ? 15000 : 45000;
            s_next_try_ms = now_ms() + backoff;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_connecting = false;
        s_ever_connected = true;
        s_fails = 0;
        s_blind = 0;
        if (!s_sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            cfg.sync_cb = on_time_sync;
            esp_netif_sntp_init(&cfg);
            s_sntp_started = true;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
    }
}

/* Captive portal: answer every DNS question with our own address so phones open the setup page. */
static void dns_task(void *arg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[320];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || !s_ap_on) continue;
        /* only answer clients of our own access point, and only plain queries with one question */
        if ((ntohl(from.sin_addr.s_addr) & 0xFFFFFF00u) != 0xC0A80400u) continue;
        if ((buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) continue;
        /* find the end of the question: the name, then type and class. Anything after it (an EDNS
         * record, which phones add) is dropped, or the answer would land in the wrong place. */
        int q = 12;
        while (q < n && buf[q] != 0 && (buf[q] & 0xC0) == 0) q += buf[q] + 1;
        if (q >= n || buf[q] != 0 || q + 5 > n) continue;
        int qend = q + 5;
        bool type_a = buf[q + 1] == 0 && buf[q + 2] == 1;
        buf[2] = 0x84 | (buf[2] & 0x01); /* response, authoritative, keep "recursion desired" */
        buf[3] = 0x00;
        buf[6] = 0;
        buf[7] = type_a ? 1 : 0; /* other types (AAAA, HTTPS) get an empty answer, not a wrong one */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        static const uint8_t answer[16] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1E, 0x00, 0x04, 192, 168, 4, 1};
        if (type_a) memcpy(buf + qend, answer, sizeof(answer));
        sendto(fd, buf, qend + (type_a ? sizeof(answer) : 0), 0, (struct sockaddr *)&from, fl);
    }
}

void net_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta_if = esp_netif_create_default_wifi_sta();
    s_ap_if = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_if, "canary");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM); /* credentials live in CANary's own settings */
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "CANary-%02X%02X", mac[4], mac[5]);
    xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
}

static void configure_ap(const char *pass)
{
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    if (strlen(pass) >= 8) {
        strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_config(WIFI_IF_AP, &ap);
}

/* Scan and join the strongest network we have a password for. */
static void try_connect(const cn_wifi_net *nets, int n)
{
    int best = -1, best_rssi = -127;
    wifi_scan_config_t sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE, .scan_time.active = {.min = 60, .max = 110}};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        s_next_try_ms = now_ms() + 10000;
        return;
    }
    uint16_t count = 24;
    static wifi_ap_record_t recs[24];
    if (esp_wifi_scan_get_ap_records(&count, recs) != ESP_OK) count = 0;
    for (int i = 0; i < count; i++)
        for (int k = 0; k < n; k++)
            if (!strcmp((const char *)recs[i].ssid, nets[k].ssid) && recs[i].rssi > best_rssi) {
                best = k;
                best_rssi = recs[i].rssi;
            }
    if (best < 0 && (s_blind++ % 3) == 2) {
        /* A hidden network never shows its name in a scan. Now and then ask for one of ours by name;
         * if it is not there this fails within a few seconds. */
        best = (s_blind / 3) % n;
    } else if (best < 0) {
        /* nothing known in range (out on the road): look again later, less often if someone is using the access point */
        s_next_try_ms = now_ms() + (s_ap_clients ? 90000 : 20000);
        return;
    }
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, nets[best].ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, nets[best].pass, sizeof(sta.sta.password));
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sta.sta.threshold.authmode = nets[best].pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH; /* WPA3 routers that only offer hash-to-element */
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    strlcpy(s_ssid, nets[best].ssid, sizeof(s_ssid));
    s_connecting = true;
    s_connect_started_ms = now_ms();
    if (esp_wifi_connect() != ESP_OK) {
        s_connecting = false;
        s_next_try_ms = now_ms() + 10000;
    }
}

void net_ap_enable(int minutes) { s_ap_until_ms = now_ms() + (int64_t)minutes * 60000; }

void net_poll(void)
{
    cn_wifi_net nets[CN_MAX_WIFI];
    int n = 0, ap_mode = 0;
    char ap_pass[65] = "canary-setup";
    uint32_t gen = 0;
    if (g_engine) {
        g_engine->pf.lock(g_engine->pf.ctx);
        n = g_engine->cfg.nwifi;
        memcpy(nets, g_engine->cfg.wifi, sizeof(nets));
        ap_mode = g_engine->cfg.ap_mode;
        strlcpy(ap_pass, g_engine->cfg.ap_pass, sizeof(ap_pass));
        gen = g_engine->cfg_gen;
        g_engine->pf.unlock(g_engine->pf.ctx);
    }
    int64_t now = now_ms();
    /* Rescue: networks are configured but none has ever worked since boot, so offer the access point for a while. */
    bool rescue = n > 0 && !s_ever_connected && !wifi_ok_get() && now > 75000 && now < 75000 + 10 * 60000;
    /* never pull the access point out from under someone who is using it */
    bool want_ap = g_recovery || n == 0 || ap_mode == 1 || now < s_ap_until_ms || rescue || (s_ap_on && s_ap_clients > 0);
    wifi_mode_t mode = want_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    bool cfg_changed = gen != s_cfg_gen;
    if (!s_started || mode != s_mode || cfg_changed) {
        if (want_ap) configure_ap(ap_pass);
        if (!s_started) {
            esp_wifi_set_mode(mode);
            if (want_ap) configure_ap(ap_pass);
            esp_wifi_start();
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            s_started = true;
        } else if (mode != s_mode) {
            esp_wifi_set_mode(mode);
            if (want_ap) configure_ap(ap_pass);
        }
        if (cfg_changed && s_cfg_gen != 0xFFFFFFFF) {
            /* a changed list of networks has to prove itself again before the rescue access point stops opening */
            static uint32_t s_nets_sig;
            uint32_t sig = 2166136261u;
            for (int k = 0; k < n; k++)
                for (const char *c = nets[k].ssid; *c; c++) sig = (sig ^ (uint8_t)*c) * 16777619u;
            if (s_nets_sig && sig != s_nets_sig && !s_connected) wifi_ok_set(false);
            s_nets_sig = sig;
        }
        if (cfg_changed && s_cfg_gen != 0xFFFFFFFF && (s_connected || s_connecting)) {
            /* settings changed: drop the link and choose again */
            bool keep = false;
            for (int k = 0; k < n; k++)
                if (!strcmp(nets[k].ssid, s_ssid)) keep = true;
            if (!keep) esp_wifi_disconnect();
        }
        s_mode = mode;
        s_cfg_gen = gen;
        s_ap_on = want_ap;
        s_fails = 0;
        s_next_try_ms = now + 500;
        if (want_ap) ESP_LOGI(TAG, "access point %s is up", s_ap_ssid);
    }
    if (!s_mdns_started && mdns_init() == ESP_OK) {
        mdns_hostname_set("canary");
        mdns_instance_name_set("CANary");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        s_mdns_started = true;
    }
    if (s_connecting && now - s_connect_started_ms > 20000) {
        s_connecting = false;
        esp_wifi_disconnect();
    }
    if (n > 0 && !g_recovery && !s_connected && !s_connecting && now >= s_next_try_ms) try_connect(nets, n);
    after_time_sync();
    if (s_connected && !wifi_ok_get()) wifi_ok_set(true);
    if (s_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
    }
    if (g_engine) {
        cn_net_status st = {0};
        st.connected = s_connected;
        strlcpy(st.ssid, s_connected ? s_ssid : "", sizeof(st.ssid));
        strlcpy(st.ip, s_ip, sizeof(st.ip));
        st.rssi = s_connected ? s_rssi : 0;
        st.ap_active = s_ap_on;
        strlcpy(st.ap_ssid, s_ap_ssid, sizeof(st.ap_ssid));
        cn_net_update(g_engine, &st);
    }
}

void net_stop(void)
{
    if (!s_started) return;
    if (s_mdns_started) {
        mdns_free();
        s_mdns_started = false;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_started = false;
    s_connected = false;
    s_mode = WIFI_MODE_NULL;
}

char *net_scan_json(void)
{
    cn_buf b;
    cn_buf_init(&b);
    cn_jw w;
    cn_jw_init(&w, &b);
    cn_jw_obj(&w);
    cn_jw_key(&w, "networks");
    cn_jw_arr(&w);
    uint16_t count = 24;
    wifi_ap_record_t *recs = calloc(count, sizeof(wifi_ap_record_t));
    wifi_scan_config_t sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE, .scan_time.active = {.min = 80, .max = 150}};
    if (recs && s_started && !s_connecting && esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
        for (int i = 0; i < count; i++) {
            if (!recs[i].ssid[0]) continue;
            bool dup = false;
            for (int k = 0; k < i; k++)
                if (!strcmp((const char *)recs[k].ssid, (const char *)recs[i].ssid)) dup = true; /* strongest first */
            if (dup) continue;
            cn_jw_obj(&w);
            cn_jw_kstr(&w, "ssid", (const char *)recs[i].ssid);
            cn_jw_kint(&w, "rssi", recs[i].rssi);
            cn_jw_kbool(&w, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
            cn_jw_end_obj(&w);
        }
    }
    free(recs);
    cn_jw_end_arr(&w);
    cn_jw_end_obj(&w);
    return cn_buf_take(&b);
}
