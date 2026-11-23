/* SD card: FAT over 4-bit SDMMC. Trip logs and the device log live in /sdcard/canary.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if BOARD_HAS_SD
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#define MOUNT "/sdcard"
static const char *TAG = "sd";
static sdmmc_card_t *s_card;
static char s_name[24];

bool sd_mount(void)
{
    if (s_card) return true;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BOARD_SD_CLK;
    slot.cmd = BOARD_SD_CMD;
    slot.d0 = BOARD_SD_D0;
    slot.d1 = BOARD_SD_D1;
    slot.d2 = BOARD_SD_D2;
    slot.d3 = BOARD_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    /* Never format behind the owner's back: an unreadable card is reported and can be formatted from the web UI. */
    esp_vfs_fat_sdmmc_mount_config_t cfg = {.format_if_mount_failed = false, .max_files = 6, .allocation_unit_size = 32 * 1024};
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no usable SD card (%s)", esp_err_to_name(err));
        s_card = NULL;
        return false;
    }
    snprintf(s_name, sizeof(s_name), "%.8s", s_card->cid.name);
    mkdir(MOUNT "/canary", 0775);
    ESP_LOGI(TAG, "SD card %s mounted, %llu MB", s_name, (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    return true;
}

void sd_unmount(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(MOUNT, s_card);
    s_card = NULL;
}

bool sd_mounted(void) { return s_card != NULL; }

int sd_info(uint64_t *total, uint64_t *free_bytes)
{
    if (!s_card) return -1;
    return esp_vfs_fat_info(MOUNT, total, free_bytes) == ESP_OK ? 0 : -1;
}

bool sd_format(void)
{
    if (!s_card) {
        /* a card that would not mount (exFAT, blank): mount with formatting allowed */
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
        slot.clk = BOARD_SD_CLK;
        slot.cmd = BOARD_SD_CMD;
        slot.d0 = BOARD_SD_D0;
        slot.d1 = BOARD_SD_D1;
        slot.d2 = BOARD_SD_D2;
        slot.d3 = BOARD_SD_D3;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        esp_vfs_fat_sdmmc_mount_config_t cfg = {.format_if_mount_failed = true, .max_files = 6, .allocation_unit_size = 32 * 1024};
        if (esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &cfg, &s_card) != ESP_OK) {
            s_card = NULL;
            return false;
        }
    } else if (esp_vfs_fat_sdcard_format(MOUNT, s_card) != ESP_OK) {
        return false;
    }
    mkdir(MOUNT "/canary", 0775);
    return true;
}

const char *sd_card_name(void) { return s_card ? s_name : ""; }

#else
bool sd_mount(void) { return false; }
void sd_unmount(void) {}
bool sd_mounted(void) { return false; }
int sd_info(uint64_t *total, uint64_t *free_bytes)
{
    (void)total;
    (void)free_bytes;
    return -1;
}
bool sd_format(void) { return false; }
const char *sd_card_name(void) { return ""; }
#endif
