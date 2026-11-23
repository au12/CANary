/* CANary firmware entry point: bring the board up, start the engine and the
 * tasks around it, and decide when to sleep. SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <math.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "canary";

cn_engine *g_engine;
bool g_recovery;
char g_device_id[20];

/* Survives software resets but not power loss: counts boots that never got as far as "running fine". */
static RTC_NOINIT_ATTR uint32_t s_boot_magic;
static RTC_NOINIT_ATTR uint32_t s_unstable_boots;

static void vehicle_task(void *arg)
{
    for (;;) cn_vehicle_step(g_engine);
}

static void sync_task(void *arg)
{
    for (;;) {
        net_poll();
        if (g_engine) cn_sync_step(g_engine);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void set_status_led(void)
{
    if (!g_engine) {
        bsp_led(10, 0, 10); /* recovery: purple */
        return;
    }
    int worst = cn_worst_severity(g_engine, NULL);
    bool awake = g_engine->vs == CN_VS_RUN || g_engine->vs == CN_VS_KOEO;
    if (worst >= CN_SEV_CRITICAL)
        bsp_led(22, 0, 0);
    else if (worst == CN_SEV_WARNING)
        bsp_led(18, 8, 0);
    else if (awake)
        bsp_led(0, 10, 0);
    else
        bsp_led(0, 0, 4); /* parked, waiting to sleep */
}

/* Car is off and everything is uploaded: stop the radios, put the board's parts to
 * sleep and doze, looking at the battery voltage every couple of seconds. The
 * device restarts cleanly when the engine starts (voltage rises) or the button is pressed. */
static void sleep_until_engine_start(const char *why)
{
    cn_config cfg = g_engine->cfg;
    cn_log(g_engine, 'i', "Going to sleep (%s), battery %.2f V", why, (double)g_engine->vbat);
    web_stop();
    net_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    plat_can_shutdown();
    sd_unmount();
    bsp_prepare_sleep();
    float vmin = bsp_vbat();
    gpio_wakeup_enable(BOARD_BUTTON, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    for (;;) {
        esp_sleep_enable_timer_wakeup(2 * 1000000ULL);
        esp_light_sleep_start();
        float v = bsp_vbat();
        if (!isnan(v) && (isnan(vmin) || v < vmin)) vmin = v;
        if (bsp_button_pressed() || cn_power_should_wake(&cfg, v, vmin)) break;
    }
    esp_restart();
}

static void housekeeping_task(void *arg)
{
    int sleepy = 0, held = 0;
    bool was_pressed = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        int64_t up = esp_timer_get_time() / 1000000;
        if (up > 90 && s_unstable_boots) s_unstable_boots = 0; /* this boot is fine */

        /* button: a press opens the access point for 15 minutes; holding it 10 s resets the settings */
        bool pressed = bsp_button_pressed();
        if (pressed) {
            held++;
            if (held == 20) {
                /* CANary's own settings only: the stock firmware keeps its data in the same flash area,
                   and someone who goes back to it should find it as they left it */
                ESP_LOGW(TAG, "button held: settings reset");
                nvs_handle_t h;
                if (nvs_open("canary", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_erase_all(h);
                    nvs_commit(h);
                    nvs_close(h);
                }
                esp_restart();
            }
        } else {
            if (was_pressed && held < 20) {
                net_ap_enable(15);
                if (g_engine) {
                    g_engine->last_ui_ms = esp_timer_get_time() / 1000;
                    cn_log(g_engine, 'i', "Button pressed: access point on for 15 minutes");
                }
            }
            held = 0;
        }
        was_pressed = pressed;
        set_status_led();
        if (!g_engine) continue;

        if (g_engine->reboot_requested) {
            vTaskDelay(pdMS_TO_TICKS(1200));
            if (g_engine->trip.active) cn_trip_end(g_engine, "restart");
            sd_unmount();
            esp_restart();
        }
        if (g_engine->format_requested) {
            g_engine->format_requested = false;
            bool ok = sd_format();
            cn_trips_scan(g_engine);
            cn_log(g_engine, ok ? 'i' : 'e', ok ? "SD card formatted" : "SD card format failed");
        }
        if (!sd_mounted() && (up % 30) == 0 && sd_mount()) {
            cn_trips_scan(g_engine);
            cn_log(g_engine, 'i', "SD card inserted");
        }
        char why[48];
        if (cn_power_can_sleep(g_engine, why, sizeof(why)) && !pressed) {
            if (++sleepy >= 4) sleep_until_engine_start(why);
        } else {
            sleepy = 0;
        }
    }
}

void app_main(void)
{
    if (s_boot_magic != 0xCA7A121E) {
        s_boot_magic = 0xCA7A121E;
        s_unstable_boots = 0;
    }
    s_unstable_boots++;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    bsp_init();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_device_id, sizeof(g_device_id), "canary-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "CANary %s on %s (%s)", CN_FW_VERSION, BOARD_NAME, g_device_id);
    bsp_led(6, 6, 6);

    /* Recovery: button held through power-up, or several crashes in a row. Only Wi-Fi and firmware update run. */
    if (bsp_button_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (bsp_button_pressed()) g_recovery = true;
    }
    if (s_unstable_boots >= 5) {
        ESP_LOGE(TAG, "%u unstable boots in a row: starting in recovery mode", (unsigned)s_unstable_boots);
        g_recovery = true;
        s_unstable_boots = 0;
    }

    /* Clock: the battery-backed RTC first; network time refines it later. */
    int64_t rtc;
    if (bsp_rtc_read(&rtc) && rtc > 1700000000) {
        struct timeval tv = {.tv_sec = (time_t)rtc};
        settimeofday(&tv, NULL);
    }

    net_init();
    if (g_recovery) {
        bsp_led(10, 0, 10);
        web_start();
        xTaskCreate(sync_task, "net", 6144, NULL, 4, NULL);
        xTaskCreate(housekeeping_task, "house", 4096, NULL, 3, NULL);
        return;
    }

    sd_mount();
    bsp_chip_init();

    cn_platform pf;
    plat_fill(&pf);
    g_engine = cn_engine_create(&pf, fw_profile_loader, NULL, g_device_id, BOARD_NAME);
    if (!g_engine) {
        ESP_LOGE(TAG, "engine failed to start; recovery mode");
        g_recovery = true;
        web_start();
        xTaskCreate(sync_task, "net", 6144, NULL, 4, NULL);
        xTaskCreate(housekeeping_task, "house", 4096, NULL, 3, NULL);
        return;
    }
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT)
        cn_log(g_engine, 'e', "Previous run ended abnormally (reset reason %d)", (int)rr);
    if (!sd_mounted()) cn_log(g_engine, 'w', "No SD card: trips are analysed but not recorded");
    if (bsp_chip_version()[0]) cn_log(g_engine, 'i', "OBD chip %s (idle; CANary uses the direct CAN connection)", bsp_chip_version());

    /* A freshly installed image is confirmed once it has got this far (matters with rollback-enabled bootloaders). */
    esp_ota_mark_app_valid_cancel_rollback();

    web_start();
    xTaskCreate(vehicle_task, "vehicle", 12288, NULL, 6, NULL);
    xTaskCreate(sync_task, "sync", 16384, NULL, 4, NULL);
    xTaskCreate(housekeeping_task, "house", 6144, NULL, 3, NULL);
}
