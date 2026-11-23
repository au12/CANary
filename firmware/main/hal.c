/* Board hardware: battery voltage, button, status LED, real-time clock and the
 * WiCAN Pro's OBD interpreter chip. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Register sequences for the WiCAN Pro peripherals follow MeatPi's GPL-3.0
 * firmware (https://github.com/meatpiHQ/wican-fw).
 */
#include "app.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>
#include <time.h>

#if BOARD_HAS_VBAT
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#endif
#if BOARD_HAS_I2C
#include "driver/i2c_master.h"
#endif

static const char *TAG __attribute__((unused)) = "hal"; /* a board without the extras logs nothing here */

#if BOARD_HAS_VBAT
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
#endif
#if BOARD_HAS_I2C
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_led, s_rtc;
#endif
static char s_chip_version[48] = "";

/* ───────────── battery voltage ───────────── */

float bsp_vbat(void)
{
#if BOARD_HAS_VBAT
    if (!s_adc) return NAN;
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, BOARD_VBAT_ADC_CHANNEL, &raw) != ESP_OK) continue;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK)
            sum += mv;
        else
            sum += raw * 1750 / 4095; /* uncalibrated estimate for 6 dB attenuation */
        n++;
    }
    if (!n) return NAN;
    float v = (float)sum / n * BOARD_VBAT_SCALE / 1000.0f;
    /* below a couple of volts the pin is just noise (bench, USB power): report it as-is, without the offset */
    return v > 2.0f ? v + BOARD_VBAT_OFFSET : v;
#else
    return NAN;
#endif
}

/* ───────────── button / LED ───────────── */

bool bsp_button_pressed(void) { return gpio_get_level(BOARD_BUTTON) == 0; }

#if BOARD_HAS_I2C && BOARD_HAS_AW2023
static void led_reg(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    if (s_led) i2c_master_transmit(s_led, b, 2, 50);
}
#endif

void bsp_led(uint8_t r, uint8_t g, uint8_t b)
{
#if BOARD_HAS_I2C && BOARD_HAS_AW2023
    led_reg(0x34, r);
    led_reg(0x35, g);
    led_reg(0x36, b);
#else
    (void)r;
    (void)g;
    (void)b;
#endif
}

/* ───────────── real-time clock (RX8130 / RX8111 family) ───────────── */

#if BOARD_HAS_I2C && BOARD_HAS_RTC
static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
#endif

bool bsp_rtc_read(int64_t *unix_s)
{
#if BOARD_HAS_I2C && BOARD_HAS_RTC
    uint8_t reg = 0x10, d[7];
    if (!s_rtc || i2c_master_transmit_receive(s_rtc, &reg, 1, d, 7, 100) != ESP_OK) return false;
    struct tm tm = {.tm_sec = bcd2dec(d[0] & 0x7F), .tm_min = bcd2dec(d[1] & 0x7F), .tm_hour = bcd2dec(d[2] & 0x3F),
                    .tm_mday = bcd2dec(d[4] & 0x3F), .tm_mon = bcd2dec(d[5] & 0x1F) - 1, .tm_year = bcd2dec(d[6]) + 100};
    if (tm.tm_year < 125 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 || tm.tm_hour > 23) return false;
    /* days since 1970 without relying on the C library's time zone state */
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int y = tm.tm_year + 1900;
    int64_t days = (int64_t)(y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400 + cum[tm.tm_mon] + tm.tm_mday - 1;
    if (tm.tm_mon > 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
    *unix_s = days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    return true;
#else
    (void)unix_s;
    return false;
#endif
}

void bsp_rtc_write(int64_t unix_s)
{
#if BOARD_HAS_I2C && BOARD_HAS_RTC
    if (!s_rtc) return;
    time_t t = (time_t)unix_s;
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t b[8] = {0x10, dec2bcd(tm.tm_sec), dec2bcd(tm.tm_min), dec2bcd(tm.tm_hour), (uint8_t)(1u << tm.tm_wday),
                    dec2bcd(tm.tm_mday), dec2bcd(tm.tm_mon + 1), dec2bcd(tm.tm_year % 100)};
    if (i2c_master_transmit(s_rtc, b, 8, 100) != ESP_OK) ESP_LOGW(TAG, "RTC write failed");
#else
    (void)unix_s;
#endif
}

/* ───────────── OBD interpreter chip ─────────────
 * CANary talks to the car through the ESP32's own CAN controller, so the
 * interpreter chip only needs to be brought up cleanly, identified, left
 * idle, and put to sleep with everything else. */

#if BOARD_HAS_OBD_CHIP
static int chip_cmd(const char *cmd, char *out, size_t n, int timeout_ms)
{
    uart_flush_input(UART_NUM_1);
    if (cmd) uart_write_bytes(UART_NUM_1, cmd, strlen(cmd));
    size_t len = 0;
    int64_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline && len + 1 < n) {
        int r = uart_read_bytes(UART_NUM_1, (uint8_t *)out + len, n - 1 - len, pdMS_TO_TICKS(20));
        if (r > 0) {
            len += (size_t)r;
            out[len] = 0;
            if (strchr(out, '>')) break;
        }
    }
    out[len] = 0;
    return (int)len;
}
#endif

void bsp_chip_init(void)
{
#if BOARD_HAS_OBD_CHIP
    uart_config_t cfg = {.baud_rate = 2000000, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
                         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
    if (uart_driver_install(UART_NUM_1, 1024, 0, 0, NULL, 0) != ESP_OK) return;
    uart_param_config(UART_NUM_1, &cfg);
    uart_set_pin(UART_NUM_1, BOARD_CHIP_UART_TX, BOARD_CHIP_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    vTaskDelay(pdMS_TO_TICKS(50));
    char buf[128];
    if (gpio_get_level(BOARD_CHIP_READY) == 1) {
        gpio_set_level(BOARD_CHIP_RESET, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(BOARD_CHIP_RESET, 1);
        chip_cmd(NULL, buf, sizeof(buf), 1500);
    } else {
        chip_cmd("ATZ\r", buf, sizeof(buf), 1500);
    }
    int n = chip_cmd("VTVERS\r", buf, sizeof(buf), 600);
    if (n < 3 || !strchr(buf, '>')) {
        uart_set_baudrate(UART_NUM_1, 115200);
        n = chip_cmd("VTVERS\r", buf, sizeof(buf), 600);
    }
    if (n >= 3 && strchr(buf, '>')) {
        size_t o = 0;
        for (const char *p = buf; *p && o + 1 < sizeof(s_chip_version); p++) {
            if (*p == '>' || *p == '\r' || *p == '\n') {
                if (o && s_chip_version[o - 1] != ' ') s_chip_version[o++] = ' ';
                continue;
            }
            if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7F) s_chip_version[o++] = *p;
        }
        while (o && s_chip_version[o - 1] == ' ') o--;
        s_chip_version[o] = 0;
        if (!strncmp(s_chip_version, "VTVERS", 6)) memmove(s_chip_version, s_chip_version + 6, strlen(s_chip_version + 6) + 1);
        ESP_LOGI(TAG, "OBD chip: %s", s_chip_version);
    } else {
        ESP_LOGW(TAG, "OBD chip did not answer (not needed for native CAN)");
    }
#endif
}

const char *bsp_chip_version(void) { return s_chip_version; }

void bsp_chip_sleep(void)
{
#if BOARD_HAS_OBD_CHIP
    char buf[64];
    chip_cmd("STSLEEP0\r", buf, sizeof(buf), 1500);
    gpio_set_level(BOARD_CHIP_SLEEP, 0);
    gpio_pulldown_en(BOARD_CHIP_SLEEP);
    gpio_hold_en(BOARD_CHIP_SLEEP);
    gpio_pulldown_en(BOARD_CHIP_READY);
    ESP_LOGI(TAG, "OBD chip asked to sleep (%s)", strstr(buf, "OK") ? "ok" : "no reply");
#endif
}

/* Last thing before the processor sleeps. */
void bsp_prepare_sleep(void)
{
    bsp_led(0, 0, 0);
#if BOARD_CAN_STBY >= 0
    gpio_set_level(BOARD_CAN_STBY, 1);
    gpio_hold_en(BOARD_CAN_STBY);
#endif
    bsp_chip_sleep();
}

void bsp_init(void)
{
    gpio_reset_pin(BOARD_BUTTON);
    gpio_set_direction(BOARD_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_BUTTON, GPIO_PULLUP_ONLY);

#if BOARD_CAN_STBY >= 0
    gpio_hold_dis(BOARD_CAN_STBY);
    gpio_reset_pin(BOARD_CAN_STBY);
    gpio_set_direction(BOARD_CAN_STBY, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_CAN_STBY, 1); /* transceiver stays in standby until the CAN driver starts */
#endif

#if BOARD_HAS_OBD_CHIP
    gpio_reset_pin(BOARD_CHIP_LED_EN);
    gpio_set_direction(BOARD_CHIP_LED_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_CHIP_LED_EN, 0);
    gpio_reset_pin(BOARD_CHIP_RESET);
    gpio_set_direction(BOARD_CHIP_RESET, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(BOARD_CHIP_RESET, 1);
    gpio_reset_pin(BOARD_CHIP_READY);
    gpio_set_direction(BOARD_CHIP_READY, GPIO_MODE_INPUT);
    gpio_pulldown_en(BOARD_CHIP_READY);
    gpio_hold_dis(BOARD_CHIP_SLEEP);
    gpio_reset_pin(BOARD_CHIP_SLEEP);
    gpio_set_direction(BOARD_CHIP_SLEEP, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_CHIP_SLEEP, 1);
#endif

#if BOARD_HAS_VBAT
    gpio_reset_pin(BOARD_VBAT_GPIO);
    gpio_set_direction(BOARD_VBAT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_VBAT_GPIO, GPIO_FLOATING);
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&ucfg, &s_adc) == ESP_OK) {
        adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_6, .bitwidth = ADC_BITWIDTH_DEFAULT};
        adc_oneshot_config_channel(s_adc, BOARD_VBAT_ADC_CHANNEL, &ccfg);
        adc_cali_curve_fitting_config_t cal = {.unit_id = ADC_UNIT_1, .chan = BOARD_VBAT_ADC_CHANNEL, .atten = ADC_ATTEN_DB_6,
                                               .bitwidth = ADC_BITWIDTH_DEFAULT};
        if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
    } else {
        s_adc = NULL;
    }
#endif

#if BOARD_HAS_I2C
    i2c_master_bus_config_t bcfg = {.i2c_port = I2C_NUM_0, .sda_io_num = BOARD_I2C_SDA, .scl_io_num = BOARD_I2C_SCL,
                                    .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    if (i2c_new_master_bus(&bcfg, &s_i2c) == ESP_OK) {
#if BOARD_HAS_AW2023
        i2c_device_config_t led = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x45, .scl_speed_hz = 200000};
        if (i2c_master_bus_add_device(s_i2c, &led, &s_led) == ESP_OK) {
            led_reg(0x00, 0x55); /* software reset */
            vTaskDelay(pdMS_TO_TICKS(5));
            led_reg(0x01, 0x01); /* chip enable */
            led_reg(0x30, 0x07); /* all three channels on */
            led_reg(0x31, 0x03);
            led_reg(0x32, 0x03);
            led_reg(0x33, 0x03);
        }
#endif
#if BOARD_HAS_RTC
        i2c_device_config_t rtc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x32, .scl_speed_hz = 200000};
        if (i2c_master_bus_add_device(s_i2c, &rtc, &s_rtc) == ESP_OK) {
            /* same register setup as the stock firmware: backup supply switching and event input */
            static const uint8_t init[][2] = {{0x30, 0x00}, {0x32, 0xC7}, {0x1C, 0x04}, {0x1D, 0x00}, {0x1E, 0x40}, {0x1F, 0x10}};
            for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); i++) i2c_master_transmit(s_rtc, init[i], 2, 100);
        }
#endif
    } else {
        ESP_LOGW(TAG, "I2C bus failed to start");
    }
#endif
}
