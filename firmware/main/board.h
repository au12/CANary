/* Board definitions. A board is a pin map plus a few capability flags; the
 * rest of the firmware does not care which one it runs on.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WiCAN Pro pin assignments follow MeatPi's open-source firmware and
 * schematic (https://github.com/meatpiHQ/wican-fw, GPL-3.0).
 */
#pragma once
#include "sdkconfig.h"

#if CONFIG_CANARY_BOARD_WICAN_PRO

#define BOARD_NAME "wican-pro"
/* CAN transceiver (TJA1044) on the OBD connector's HS-CAN pair, pins 6 and 14 */
#define BOARD_CAN_TX 2
#define BOARD_CAN_RX 1
#define BOARD_CAN_STBY 38 /* high = standby */
/* supply voltage: 620k/62k divider and a buffer into ADC1 channel 3 */
#define BOARD_HAS_VBAT 1
#define BOARD_VBAT_GPIO 4
#define BOARD_VBAT_ADC_CHANNEL ADC_CHANNEL_3
#define BOARD_VBAT_SCALE 11.0f
#define BOARD_VBAT_OFFSET 0.1f
/* SD card, 4-bit SDMMC */
#define BOARD_HAS_SD 1
#define BOARD_SD_CLK 21
#define BOARD_SD_CMD 47
#define BOARD_SD_D0 14
#define BOARD_SD_D1 13
#define BOARD_SD_D2 12
#define BOARD_SD_D3 48
#define BOARD_SD_DETECT 40
/* I2C: AW2023 RGB LED driver, RX8130-compatible RTC, IMU, USB-C controller */
#define BOARD_HAS_I2C 1
#define BOARD_I2C_SDA 5
#define BOARD_I2C_SCL 6
#define BOARD_HAS_AW2023 1
#define BOARD_HAS_RTC 1
#define BOARD_BUTTON 8 /* active low */
/* OBD interpreter chip (not used for data; kept quiet and put to sleep with the rest) */
#define BOARD_HAS_OBD_CHIP 1
#define BOARD_CHIP_UART_TX 16
#define BOARD_CHIP_UART_RX 15
#define BOARD_CHIP_RESET 41
#define BOARD_CHIP_READY 7 /* high = awake */
#define BOARD_CHIP_SLEEP 9 /* low = may sleep */
#define BOARD_CHIP_LED_EN 42

#elif CONFIG_CANARY_BOARD_GENERIC_S3

/* Any ESP32-S3 dev board with a 3.3 V CAN transceiver (SN65HVD230, TJA1051…).
 * No battery sensing, so the device never sleeps: power it from a switched source. */
#define BOARD_NAME "generic-esp32s3"
#define BOARD_CAN_TX CONFIG_CANARY_GENERIC_CAN_TX
#define BOARD_CAN_RX CONFIG_CANARY_GENERIC_CAN_RX
#define BOARD_CAN_STBY (-1)
#define BOARD_HAS_VBAT 0
#define BOARD_HAS_SD 0
#define BOARD_HAS_I2C 0
#define BOARD_HAS_AW2023 0
#define BOARD_HAS_RTC 0
#define BOARD_BUTTON 0 /* BOOT button */
#define BOARD_HAS_OBD_CHIP 0

#else
#error "Select a board in menuconfig (CANary -> Board)"
#endif
