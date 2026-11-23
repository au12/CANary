/* Device configuration <-> JSON. SPDX-License-Identifier: GPL-3.0-or-later */
#include "canary/core.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

void cn_config_defaults(cn_config *c)
{
    memset(c, 0, sizeof(*c));
    cn_strlcpy(c->device_name, "CANary", sizeof(c->device_name));
    cn_strlcpy(c->ap_pass, "canary-setup", sizeof(c->ap_pass));
    cn_strlcpy(c->odo_unit, "mi", sizeof(c->odo_unit));
    cn_strlcpy(c->profile, "auto", sizeof(c->profile));
    c->gb_upload_logs = true;
    c->sleep_enabled = true;
    c->sleep_voltage = 13.0;
    c->wake_voltage = 13.3;
    c->sleep_delay_s = 300;
    c->log_enabled = true;
    c->retention_days = 7;
    c->units_imperial = true;
}

static void get_str(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v)) cn_strlcpy(dst, v->valuestring, n);
}

/* Secrets arrive as "" from a UI that was shown the redacted form; keep the stored value then. */
static void get_secret(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && strcmp(v->valuestring, "\x01keep") != 0) cn_strlcpy(dst, v->valuestring, n);
}

static void get_bool(const cJSON *o, const char *k, bool *dst)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsBool(v)) *dst = cJSON_IsTrue(v);
}

static void get_int(const cJSON *o, const char *k, int *dst, int lo, int hi)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) {
        int x = (int)v->valuedouble;
        *dst = x < lo ? lo : x > hi ? hi : x;
    }
}

static void get_dbl(const cJSON *o, const char *k, double *dst, double lo, double hi)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) {
        double x = v->valuedouble;
        *dst = x < lo ? lo : x > hi ? hi : x;
    }
}

bool cn_config_from_json(cn_config *c, const char *json)
{
    cJSON *doc = cJSON_Parse(json);
    if (!cJSON_IsObject(doc)) {
        cJSON_Delete(doc);
        return false;
    }
    get_str(doc, "device_name", c->device_name, sizeof(c->device_name));
    get_bool(doc, "units_imperial", &c->units_imperial);
    get_int(doc, "tz_offset_min", &c->tz_offset_min, -900, 900);
    get_secret(doc, "ui_password", c->ui_password, sizeof(c->ui_password));

    const cJSON *wifi = cJSON_GetObjectItemCaseSensitive(doc, "wifi");
    if (cJSON_IsObject(wifi)) {
        get_secret(wifi, "ap_pass", c->ap_pass, sizeof(c->ap_pass));
        get_int(wifi, "ap_mode", &c->ap_mode, 0, 1);
        const cJSON *nets = cJSON_GetObjectItemCaseSensitive(wifi, "networks");
        if (cJSON_IsArray(nets)) {
            cn_wifi_net old[CN_MAX_WIFI];
            int nold = c->nwifi;
            memcpy(old, c->wifi, sizeof(old));
            c->nwifi = 0;
            const cJSON *n;
            cJSON_ArrayForEach(n, nets)
            {
                if (c->nwifi >= CN_MAX_WIFI) break;
                cn_wifi_net *w = &c->wifi[c->nwifi];
                memset(w, 0, sizeof(*w));
                get_str(n, "ssid", w->ssid, sizeof(w->ssid));
                if (!w->ssid[0]) continue;
                const cJSON *pw = cJSON_GetObjectItemCaseSensitive(n, "pass");
                if (cJSON_IsString(pw) && strcmp(pw->valuestring, "\x01keep") != 0) {
                    cn_strlcpy(w->pass, pw->valuestring, sizeof(w->pass));
                } else {
                    for (int i = 0; i < nold; i++)
                        if (!strcmp(old[i].ssid, w->ssid)) cn_strlcpy(w->pass, old[i].pass, sizeof(w->pass));
                }
                c->nwifi++;
            }
        }
    }
    const cJSON *gb = cJSON_GetObjectItemCaseSensitive(doc, "glovebox");
    if (cJSON_IsObject(gb)) {
        get_str(gb, "url", c->gb_url, sizeof(c->gb_url));
        size_t n = strlen(c->gb_url);
        while (n && c->gb_url[n - 1] == '/') c->gb_url[--n] = 0;
        get_secret(gb, "api_key", c->gb_api_key, sizeof(c->gb_api_key));
        get_int(gb, "vehicle_id", &c->gb_vehicle_id, 0, 1000000);
        get_bool(gb, "upload_logs", &c->gb_upload_logs);
        get_str(gb, "odometer_unit", c->odo_unit, sizeof(c->odo_unit));
        if (strcmp(c->odo_unit, "km") != 0) cn_strlcpy(c->odo_unit, "mi", sizeof(c->odo_unit));
        get_dbl(gb, "odometer_offset", &c->odo_offset, -100000, 100000);
    }
    const cJSON *veh = cJSON_GetObjectItemCaseSensitive(doc, "vehicle");
    if (cJSON_IsObject(veh)) {
        get_str(veh, "profile", c->profile, sizeof(c->profile));
        if (!c->profile[0]) cn_strlcpy(c->profile, "auto", sizeof(c->profile));
    }
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(doc, "power");
    if (cJSON_IsObject(pw)) {
        get_bool(pw, "sleep_enabled", &c->sleep_enabled);
        get_dbl(pw, "sleep_voltage", &c->sleep_voltage, 11.5, 14.5);
        get_dbl(pw, "wake_voltage", &c->wake_voltage, 12.0, 15.0);
        get_int(pw, "sleep_delay_s", &c->sleep_delay_s, 60, 3600);
        if (c->wake_voltage < c->sleep_voltage + 0.1) c->wake_voltage = c->sleep_voltage + 0.1;
    }
    const cJSON *lg = cJSON_GetObjectItemCaseSensitive(doc, "log");
    if (cJSON_IsObject(lg)) {
        get_bool(lg, "enabled", &c->log_enabled);
        get_int(lg, "retention_days", &c->retention_days, 1, 365);
    }
    cJSON_Delete(doc);
    return true;
}

char *cn_config_to_json(const cn_config *c, bool redact)
{
    cn_buf b;
    cn_buf_init(&b);
    cn_jw w;
    cn_jw_init(&w, &b);
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "device_name", c->device_name);
    cn_jw_kbool(&w, "units_imperial", c->units_imperial);
    cn_jw_kint(&w, "tz_offset_min", c->tz_offset_min);
    if (redact)
        cn_jw_kbool(&w, "ui_password_set", c->ui_password[0] != 0);
    else
        cn_jw_kstr(&w, "ui_password", c->ui_password);

    cn_jw_key(&w, "wifi");
    cn_jw_obj(&w);
    if (redact)
        cn_jw_kbool(&w, "ap_pass_set", c->ap_pass[0] != 0);
    else
        cn_jw_kstr(&w, "ap_pass", c->ap_pass);
    cn_jw_kint(&w, "ap_mode", c->ap_mode);
    cn_jw_key(&w, "networks");
    cn_jw_arr(&w);
    for (int i = 0; i < c->nwifi; i++) {
        cn_jw_obj(&w);
        cn_jw_kstr(&w, "ssid", c->wifi[i].ssid);
        if (redact)
            cn_jw_kbool(&w, "pass_set", c->wifi[i].pass[0] != 0);
        else
            cn_jw_kstr(&w, "pass", c->wifi[i].pass);
        cn_jw_end_obj(&w);
    }
    cn_jw_end_arr(&w);
    cn_jw_end_obj(&w);

    cn_jw_key(&w, "glovebox");
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "url", c->gb_url);
    if (redact)
        cn_jw_kbool(&w, "api_key_set", c->gb_api_key[0] != 0);
    else
        cn_jw_kstr(&w, "api_key", c->gb_api_key);
    cn_jw_kint(&w, "vehicle_id", c->gb_vehicle_id);
    cn_jw_kbool(&w, "upload_logs", c->gb_upload_logs);
    cn_jw_kstr(&w, "odometer_unit", c->odo_unit);
    cn_jw_knum(&w, "odometer_offset", c->odo_offset, 1);
    cn_jw_end_obj(&w);

    cn_jw_key(&w, "vehicle");
    cn_jw_obj(&w);
    cn_jw_kstr(&w, "profile", c->profile);
    cn_jw_end_obj(&w);

    cn_jw_key(&w, "power");
    cn_jw_obj(&w);
    cn_jw_kbool(&w, "sleep_enabled", c->sleep_enabled);
    cn_jw_knum(&w, "sleep_voltage", c->sleep_voltage, 1);
    cn_jw_knum(&w, "wake_voltage", c->wake_voltage, 1);
    cn_jw_kint(&w, "sleep_delay_s", c->sleep_delay_s);
    cn_jw_end_obj(&w);

    cn_jw_key(&w, "log");
    cn_jw_obj(&w);
    cn_jw_kbool(&w, "enabled", c->log_enabled);
    cn_jw_kint(&w, "retention_days", c->retention_days);
    cn_jw_end_obj(&w);

    cn_jw_end_obj(&w);
    return cn_buf_take(&b);
}
