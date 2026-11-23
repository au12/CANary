/* Shared declarations for the ESP32 firmware. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "board.h"
#include "canary/core.h"

#include <stdbool.h>
#include <stdint.h>

extern cn_engine *g_engine;
extern bool g_recovery;      /* started in recovery mode: Wi-Fi access point and firmware update only */
extern char g_device_id[20]; /* canary-xxxxxx, from the MAC address */

/* hal.c */
void bsp_init(void);
float bsp_vbat(void);
bool bsp_button_pressed(void);
void bsp_led(uint8_t r, uint8_t g, uint8_t b);
bool bsp_rtc_read(int64_t *unix_s);
void bsp_rtc_write(int64_t unix_s);
void bsp_chip_init(void);
void bsp_chip_sleep(void);
const char *bsp_chip_version(void);
void bsp_prepare_sleep(void);

/* sdcard.c */
bool sd_mount(void);
void sd_unmount(void);
bool sd_mounted(void);
int sd_info(uint64_t *total, uint64_t *free_bytes);
bool sd_format(void);
const char *sd_card_name(void);

/* plat.c */
void plat_fill(cn_platform *pf);
void plat_can_shutdown(void);

/* profiles (generated at build time from /profiles) */
char *fw_profile_loader(void *ctx, const char *id);

/* net.c */
void net_init(void);
void net_poll(void); /* once a second: keeps Wi-Fi in line with the settings and reports status */
void net_ap_enable(int minutes);
void net_stop(void);
char *net_scan_json(void);

/* web.c */
void web_start(void);
void web_stop(void);

/* generated web assets */
typedef struct {
    const char *path;
    const char *type;
    const uint8_t *data;
    uint32_t len;
    uint8_t gzip; /* 0 for files that are already compressed (fonts, PNG) */
} web_asset;
extern const web_asset g_web_assets[];
extern const int g_web_asset_count;
extern const char g_web_build[];
