/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* MQTT over TLS Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "esp_crt_bundle.h"
#include "driver/rmt_tx.h"
#include "mqtt_client.h"

static const char *TAG = "satmon_mpu_mqtt";

#define MQTT_CONNECTED_BIT BIT0
#define SATMON_WIFI_CONNECTED_BIT BIT0
#define SATMON_WIFI_FAIL_BIT BIT1

#define MPU_I2C_TIMEOUT_MS 100
#define MPU_I2C_GLITCH_IGNORE_CNT 7
#define MPU_REG_SMPLRT_DIV 0x19
#define MPU_REG_CONFIG 0x1A
#define MPU_REG_GYRO_CONFIG 0x1B
#define MPU_REG_ACCEL_CONFIG 0x1C
#define MPU_REG_ACCEL_CONFIG2 0x1D
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define MPU_REG_PWR_MGMT_1 0x6B
#define MPU_REG_PWR_MGMT_2 0x6C
#define MPU_REG_WHO_AM_I 0x75

#define MPU_WHO_AM_I_MPU6050 0x68
#define MPU_WHO_AM_I_MPU6500 0x70
#define MPU_WHO_AM_I_MPU9250 0x71
#define MPU_WHO_AM_I_MPU9255 0x73

#define LSM6DS_REG_WHO_AM_I 0x0F
#define LSM6DS_REG_CTRL1_XL 0x10
#define LSM6DS_REG_CTRL2_G 0x11
#define LSM6DS_REG_CTRL3_C 0x12
#define LSM6DS_REG_CTRL9_XL 0x18
#define LSM6DS_REG_CTRL10_C 0x19
#define LSM6DS_REG_OUT_TEMP_L 0x20

#define LSM6DS_WHO_AM_I_LSM6DS3 0x69
#define LSM6DS_WHO_AM_I_LSM6DSL 0x6A
#define LSM6DS_WHO_AM_I_LSM6DSR 0x6B
#define LSM6DS_WHO_AM_I_LSM6DSO 0x6C

#define MPU_GYRO_UDPS_PER_LSB 7634
#define LSM6DS_GYRO_UDPS_PER_LSB 8750
#define IMU_ACCEL_UG_PER_LSB_DEFAULT 61
#define IMU_GYRO_BIAS_SAMPLES 64
#define IMU_GYRO_BIAS_SETTLE_MS 120
#define IMU_GYRO_BIAS_SAMPLE_DELAY_MS 5
#define SATMON_IMU_SDA_GPIO 6
#define SATMON_IMU_SCL_GPIO 7
#define SATMON_IMU_I2C_FREQ_HZ 400000
#define SATMON_SENSOR_ID "mpu"

#define SATMON_MIN_VALID_YEAR 2020
#define SATMON_SATELLITE_ID_JSON_LEN 64
#define SATMON_TIMESTAMP_JSON_LEN sizeof("\"2026-04-20T14:32:10Z\"")

#if CONFIG_SATMON_LED_ENABLE
#define SATMON_LED_COUNT 1
#define SATMON_LED_DEFAULT_RED 20
#define SATMON_LED_DEFAULT_GREEN 195
#define SATMON_LED_DEFAULT_BLUE 183
#define SATMON_LED_BOOT_BLINK_COUNT 8
#define SATMON_LED_BOOT_BLINK_ON_MS 180
#define SATMON_LED_BOOT_BLINK_OFF_MS 180
#define SATMON_LED_PAYLOAD_MAX_LEN 256
#define SATMON_LED_TOPIC_MAX_LEN 128
#define SATMON_LED_TOPIC_SUFFIX "/led"
#define SATMON_LED_RMT_RESOLUTION_HZ 10000000
#define SATMON_LED_WS2812_T0H_TICKS 4
#define SATMON_LED_WS2812_T0L_TICKS 8
#define SATMON_LED_WS2812_T1H_TICKS 8
#define SATMON_LED_WS2812_T1L_TICKS 4
#endif

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} axis_raw_t;

typedef struct {
    axis_raw_t accel_raw;
    axis_raw_t gyro_raw;
    int16_t temp_raw;
} mpu_sample_t;

typedef enum {
    IMU_MODEL_UNKNOWN,
    IMU_MODEL_MPU925X,
    IMU_MODEL_LSM6DS,
} imu_model_t;

static EventGroupHandle_t s_mqtt_event_group;
static EventGroupHandle_t s_wifi_event_group;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_mpu_dev;
static uint8_t s_mpu_i2c_addr;
static imu_model_t s_mpu_model = IMU_MODEL_UNKNOWN;
static uint8_t s_mpu_who_am_i;
static axis_raw_t s_gyro_bias;
static bool s_mpu_ready;
static int s_wifi_retry_count;

#if CONFIG_SATMON_LED_ENABLE
static rmt_channel_handle_t s_led_rmt_channel;
static rmt_encoder_handle_t s_led_rmt_encoder;
static bool s_led_enabled;
static uint8_t s_led_red = SATMON_LED_DEFAULT_RED;
static uint8_t s_led_green = SATMON_LED_DEFAULT_GREEN;
static uint8_t s_led_blue = SATMON_LED_DEFAULT_BLUE;
#endif

#if CONFIG_SATMON_CERT_VALIDATE_CUSTOM
static const char cert_override_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    CONFIG_SATMON_BROKER_CERTIFICATE_OVERRIDE "\n"
    "-----END CERTIFICATE-----";
#endif

#if CONFIG_SATMON_CERT_VALIDATE_MOSQUITTO_CA
/* Embedded Mosquitto CA certificate for test.mosquitto.org:8883 */
extern const uint8_t mosquitto_org_crt_start[] asm("_binary_mosquitto_org_crt_start");
extern const uint8_t mosquitto_org_crt_end[] asm("_binary_mosquitto_org_crt_end");
#endif

static int16_t bytes_to_i16(uint8_t high_byte, uint8_t low_byte)
{
    return (int16_t)(((uint16_t)high_byte << 8) | low_byte);
}

static int16_t little_endian_bytes_to_i16(uint8_t low_byte, uint8_t high_byte)
{
    return bytes_to_i16(high_byte, low_byte);
}

static int16_t clamp_i16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static int32_t rounded_average(int64_t sum, int samples)
{
    if (sum >= 0) {
        return (int32_t)((sum + samples / 2) / samples);
    }
    return (int32_t)((sum - samples / 2) / samples);
}

static bool lsm6ds_who_am_i_supported(uint8_t who_am_i)
{
    return who_am_i == LSM6DS_WHO_AM_I_LSM6DS3 || who_am_i == LSM6DS_WHO_AM_I_LSM6DSL ||
           who_am_i == LSM6DS_WHO_AM_I_LSM6DSR || who_am_i == LSM6DS_WHO_AM_I_LSM6DSO;
}

static bool mpu_who_am_i_supported(uint8_t who_am_i)
{
    return who_am_i == MPU_WHO_AM_I_MPU6050 || who_am_i == MPU_WHO_AM_I_MPU6500 ||
           who_am_i == MPU_WHO_AM_I_MPU9250 || who_am_i == MPU_WHO_AM_I_MPU9255;
}

static int16_t lsm6ds_temperature_deci_c(int16_t temp_raw)
{
    /* LSM6DS sensitivity is 256 LSB/C with a 25 C reference, in tenths of a degree. */
    return (int16_t)(250 + ((int32_t)temp_raw * 10) / 256);
}

static int16_t mpu_temperature_deci_c(int16_t temp_raw)
{
    /* Convert the raw IMU temperature register to tenths of a degree Celsius.
       MPU-6050:           T(C) = raw / 340    + 36.53
       MPU-6500/9250/9255: T(C) = raw / 333.87 + 21   (RoomTemp_Offset = 0) */
    if (s_mpu_who_am_i == MPU_WHO_AM_I_MPU6050) {
        return clamp_i16(365 + ((int32_t)temp_raw * 10) / 340);
    }
    return clamp_i16(210 + ((int32_t)temp_raw * 1000) / 33387);
}

static const char *imu_model_name(void)
{
    switch (s_mpu_model) {
    case IMU_MODEL_MPU925X:
        return "mpu-family";
    case IMU_MODEL_LSM6DS:
        return "lsm6ds-family";
    default:
        return "unknown";
    }
}

static int imu_gyro_udps_per_lsb(void)
{
    return s_mpu_model == IMU_MODEL_LSM6DS ? LSM6DS_GYRO_UDPS_PER_LSB : MPU_GYRO_UDPS_PER_LSB;
}

static int imu_accel_ug_per_lsb(void)
{
    return IMU_ACCEL_UG_PER_LSB_DEFAULT;
}

static const char *satmon_satellite_id(void)
{
#if CONFIG_SATMON_NODE_PROFILE_SAT01_MPU
    return "SAT-01";
#elif CONFIG_SATMON_NODE_PROFILE_SAT02_GROVE_0X65
    return "SAT-02";
#else
    return CONFIG_SATMON_SATELLITE_ID;
#endif
}

#if CONFIG_SATMON_LED_ENABLE
static esp_err_t satmon_led_init(void)
{
    if (s_led_rmt_channel != NULL && s_led_rmt_encoder != NULL) {
        return ESP_OK;
    }

    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = (gpio_num_t)CONFIG_SATMON_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = SATMON_LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .flags.invert_out = false,
        .flags.with_dma = false,
        .flags.init_level = 0,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config, &s_led_rmt_channel), TAG,
                        "Failed to create onboard RGB LED RMT channel on GPIO %d", CONFIG_SATMON_LED_GPIO);

    const rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = SATMON_LED_WS2812_T0H_TICKS,
            .level1 = 0,
            .duration1 = SATMON_LED_WS2812_T0L_TICKS,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = SATMON_LED_WS2812_T1H_TICKS,
            .level1 = 0,
            .duration1 = SATMON_LED_WS2812_T1L_TICKS,
        },
        .flags.msb_first = true,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&encoder_config, &s_led_rmt_encoder), TAG,
                        "Failed to create onboard RGB LED RMT encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_rmt_channel), TAG, "Failed to enable onboard RGB LED RMT channel");

    ESP_LOGI(TAG, "Onboard RGB LED RMT ready on GPIO %d", CONFIG_SATMON_LED_GPIO);

    return ESP_OK;
}

static esp_err_t satmon_led_write_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_ERROR(satmon_led_init(), TAG, "Onboard RGB LED is not ready");

    const uint8_t pixel_grb[SATMON_LED_COUNT * 3] = {green, red, blue};
    const rmt_transmit_config_t tx_config = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };

    ESP_RETURN_ON_ERROR(rmt_transmit(s_led_rmt_channel, s_led_rmt_encoder, pixel_grb, sizeof(pixel_grb), &tx_config),
                        TAG, "Failed to transmit onboard RGB LED data");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(s_led_rmt_channel, 100), TAG,
                        "Timed out updating onboard RGB LED");

    return ESP_OK;
}

static esp_err_t satmon_led_apply(bool enabled, uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_ERROR(satmon_led_write_rgb(enabled ? red : 0, enabled ? green : 0, enabled ? blue : 0), TAG,
                        "Failed to update onboard RGB LED");

    s_led_enabled = enabled;
    s_led_red = red;
    s_led_green = green;
    s_led_blue = blue;
    ESP_LOGI(TAG, "Onboard RGB LED %s color=#%02x%02x%02x", enabled ? "on" : "off", red, green, blue);

    return ESP_OK;
}

static esp_err_t satmon_led_boot_blink_test(void)
{
#if CONFIG_SATMON_LED_BOOT_BLINK_TEST
    ESP_LOGI(TAG, "Starting onboard RGB LED boot blink test on GPIO %d", CONFIG_SATMON_LED_GPIO);

    for (int blink = 0; blink < SATMON_LED_BOOT_BLINK_COUNT; ++blink) {
        ESP_RETURN_ON_ERROR(satmon_led_write_rgb(SATMON_LED_DEFAULT_RED, SATMON_LED_DEFAULT_GREEN,
                                                 SATMON_LED_DEFAULT_BLUE),
                            TAG, "Failed to turn onboard RGB LED on during boot blink test");
        vTaskDelay(pdMS_TO_TICKS(SATMON_LED_BOOT_BLINK_ON_MS));

        ESP_RETURN_ON_ERROR(satmon_led_write_rgb(0, 0, 0), TAG,
                            "Failed to turn onboard RGB LED off during boot blink test");
        vTaskDelay(pdMS_TO_TICKS(SATMON_LED_BOOT_BLINK_OFF_MS));
    }

    s_led_enabled = false;
    ESP_LOGI(TAG, "Finished onboard RGB LED boot blink test");
#endif

    return ESP_OK;
}

static const char *json_value_start(const char *json, const char *key)
{
    char pattern[32];
    const int pattern_len = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (pattern_len < 0 || pattern_len >= (int)sizeof(pattern)) {
        return NULL;
    }

    const char *key_start = strstr(json, pattern);
    if (key_start == NULL) {
        return NULL;
    }

    const char *value = strchr(key_start + pattern_len, ':');
    if (value == NULL) {
        return NULL;
    }

    value++;
    while (*value != '\0' && isspace((unsigned char)*value)) {
        value++;
    }

    return value;
}

static bool json_bool_value(const char *json, const char *key, bool *value)
{
    const char *start = json_value_start(json, key);
    if (start == NULL) {
        return false;
    }

    if (strncmp(start, "true", 4) == 0 || *start == '1') {
        *value = true;
        return true;
    }

    if (strncmp(start, "false", 5) == 0 || *start == '0') {
        *value = false;
        return true;
    }

    return false;
}

static bool json_u8_value(const char *json, const char *key, uint8_t *value)
{
    const char *start = json_value_start(json, key);
    if (start == NULL) {
        return false;
    }

    char *end = NULL;
    const long parsed = strtol(start, &end, 10);
    if (end == start || parsed < 0 || parsed > 255) {
        return false;
    }

    *value = (uint8_t)parsed;
    return true;
}

static int hex_digit_value(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

static bool parse_hex_color(const char *color, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (color[0] == '#') {
        color++;
    }

    for (int index = 0; index < 6; ++index) {
        if (hex_digit_value(color[index]) < 0) {
            return false;
        }
    }

    if (color[6] != '\0') {
        return false;
    }

    *red = (uint8_t)((hex_digit_value(color[0]) << 4) | hex_digit_value(color[1]));
    *green = (uint8_t)((hex_digit_value(color[2]) << 4) | hex_digit_value(color[3]));
    *blue = (uint8_t)((hex_digit_value(color[4]) << 4) | hex_digit_value(color[5]));
    return true;
}

static bool json_color_value(const char *json, uint8_t *red, uint8_t *green, uint8_t *blue)
{
    const char *start = json_value_start(json, "color");
    if (start == NULL || *start != '"') {
        return false;
    }

    start++;
    char color[8] = {0};
    size_t color_len = 0;
    while (*start != '\0' && *start != '"' && color_len + 1 < sizeof(color)) {
        color[color_len++] = *start++;
    }

    return parse_hex_color(color, red, green, blue);
}

static bool build_led_command_topic(char *topic, size_t topic_len, const char *target)
{
    const int written = snprintf(topic, topic_len, "%s/%s%s", CONFIG_SATMON_LED_COMMAND_TOPIC_PREFIX, target,
                                 SATMON_LED_TOPIC_SUFFIX);
    return written > 0 && written < (int)topic_len;
}

static bool mqtt_topic_equals(const char *topic, int topic_len, const char *expected)
{
    const size_t expected_len = strlen(expected);
    return topic_len == (int)expected_len && memcmp(topic, expected, expected_len) == 0;
}

static bool mqtt_topic_is_led_command(const char *topic, int topic_len)
{
    char selected_topic[SATMON_LED_TOPIC_MAX_LEN];
    char broadcast_topic[SATMON_LED_TOPIC_MAX_LEN];

    if (!build_led_command_topic(selected_topic, sizeof(selected_topic), satmon_satellite_id()) ||
        !build_led_command_topic(broadcast_topic, sizeof(broadcast_topic), "all")) {
        ESP_LOGW(TAG, "LED command topic is too long");
        return false;
    }

    return mqtt_topic_equals(topic, topic_len, selected_topic) || mqtt_topic_equals(topic, topic_len, broadcast_topic);
}

static void subscribe_led_command_topics(esp_mqtt_client_handle_t client)
{
    char selected_topic[SATMON_LED_TOPIC_MAX_LEN];
    char broadcast_topic[SATMON_LED_TOPIC_MAX_LEN];

    if (!build_led_command_topic(selected_topic, sizeof(selected_topic), satmon_satellite_id()) ||
        !build_led_command_topic(broadcast_topic, sizeof(broadcast_topic), "all")) {
        ESP_LOGW(TAG, "Cannot subscribe to LED command topic because it is too long");
        return;
    }

    const int selected_msg_id = esp_mqtt_client_subscribe(client, selected_topic, 1);
    const int broadcast_msg_id = esp_mqtt_client_subscribe(client, broadcast_topic, 1);
    ESP_LOGI(TAG, "Subscribed to LED commands: %s msg_id=%d, %s msg_id=%d", selected_topic, selected_msg_id,
             broadcast_topic, broadcast_msg_id);
}

static bool handle_led_mqtt_event(const esp_mqtt_event_handle_t event)
{
    if (!mqtt_topic_is_led_command(event->topic, event->topic_len)) {
        return false;
    }

    if (event->data_len <= 0 || event->data_len >= SATMON_LED_PAYLOAD_MAX_LEN) {
        ESP_LOGW(TAG, "Ignoring LED command with invalid payload length %d", event->data_len);
        return true;
    }

    char payload[SATMON_LED_PAYLOAD_MAX_LEN];
    memcpy(payload, event->data, event->data_len);
    payload[event->data_len] = '\0';

    bool enabled = s_led_enabled;
    uint8_t red = s_led_red;
    uint8_t green = s_led_green;
    uint8_t blue = s_led_blue;

    const bool has_enabled = json_bool_value(payload, "enabled", &enabled);
    const bool has_color = json_color_value(payload, &red, &green, &blue);
    const bool has_rgb = json_u8_value(payload, "red", &red) | json_u8_value(payload, "green", &green) |
                         json_u8_value(payload, "blue", &blue);

    if (!has_enabled && !has_color && !has_rgb) {
        ESP_LOGW(TAG, "Ignoring LED command without enabled/color fields: %s", payload);
        return true;
    }

    if (satmon_led_apply(enabled, red, green, blue) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply LED command: %s", payload);
    }

    return true;
}
#endif

static esp_err_t mpu_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_mpu_dev, data, sizeof(data), MPU_I2C_TIMEOUT_MS);
}

static esp_err_t mpu_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_mpu_dev, &reg, sizeof(reg), data, len, MPU_I2C_TIMEOUT_MS);
}

static esp_err_t mpu925x_read_sample(mpu_sample_t *sample)
{
    uint8_t data[14];
    ESP_RETURN_ON_ERROR(mpu_read_regs(MPU_REG_ACCEL_XOUT_H, data, sizeof(data)), TAG, "Failed to read MPU sample");

    sample->accel_raw.x = bytes_to_i16(data[0], data[1]);
    sample->accel_raw.y = bytes_to_i16(data[2], data[3]);
    sample->accel_raw.z = bytes_to_i16(data[4], data[5]);
    sample->temp_raw = mpu_temperature_deci_c(bytes_to_i16(data[6], data[7]));
    sample->gyro_raw.x = bytes_to_i16(data[8], data[9]);
    sample->gyro_raw.y = bytes_to_i16(data[10], data[11]);
    sample->gyro_raw.z = bytes_to_i16(data[12], data[13]);

    return ESP_OK;
}

static esp_err_t lsm6ds_read_sample(mpu_sample_t *sample)
{
    uint8_t data[14];
    ESP_RETURN_ON_ERROR(mpu_read_regs(LSM6DS_REG_OUT_TEMP_L, data, sizeof(data)), TAG, "Failed to read LSM6DS sample");

    sample->temp_raw = lsm6ds_temperature_deci_c(little_endian_bytes_to_i16(data[0], data[1]));
    sample->gyro_raw.x = little_endian_bytes_to_i16(data[2], data[3]);
    sample->gyro_raw.y = little_endian_bytes_to_i16(data[4], data[5]);
    sample->gyro_raw.z = little_endian_bytes_to_i16(data[6], data[7]);
    sample->accel_raw.x = little_endian_bytes_to_i16(data[8], data[9]);
    sample->accel_raw.y = little_endian_bytes_to_i16(data[10], data[11]);
    sample->accel_raw.z = little_endian_bytes_to_i16(data[12], data[13]);

    return ESP_OK;
}

static esp_err_t mpu_read_sample(mpu_sample_t *sample)
{
    if (!s_mpu_ready || s_mpu_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;

    switch (s_mpu_model) {
    case IMU_MODEL_MPU925X:
        err = mpu925x_read_sample(sample);
        break;
    case IMU_MODEL_LSM6DS:
        err = lsm6ds_read_sample(sample);
        break;
    default:
        return ESP_ERR_INVALID_STATE;
    }

    if (err != ESP_OK) {
        return err;
    }

    sample->gyro_raw.x = clamp_i16((int32_t)sample->gyro_raw.x - s_gyro_bias.x);
    sample->gyro_raw.y = clamp_i16((int32_t)sample->gyro_raw.y - s_gyro_bias.y);
    sample->gyro_raw.z = clamp_i16((int32_t)sample->gyro_raw.z - s_gyro_bias.z);

    return ESP_OK;
}

static esp_err_t mpu_bus_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)SATMON_IMU_SDA_GPIO,
        .scl_io_num = (gpio_num_t)SATMON_IMU_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = MPU_I2C_GLITCH_IGNORE_CNT,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG, "Failed to create I2C bus");
    ESP_LOGI(TAG, "I2C bus ready for IMU: SDA GPIO %d, SCL GPIO %d, %d Hz", SATMON_IMU_SDA_GPIO,
             SATMON_IMU_SCL_GPIO, SATMON_IMU_I2C_FREQ_HZ);

    return ESP_OK;
}

static void i2c_log_scan(void)
{
    char found[192] = {0};
    size_t offset = 0;
    int found_count = 0;

    for (uint8_t address = 0x08; address <= 0x77; ++address) {
        if (i2c_master_probe(s_i2c_bus, address, MPU_I2C_TIMEOUT_MS) != ESP_OK) {
            continue;
        }

        const int written = snprintf(&found[offset], sizeof(found) - offset, "%s0x%02x", found_count ? ", " : "",
                                     address);
        if (written > 0) {
            const size_t next_offset = offset + (size_t)written;
            offset = next_offset < sizeof(found) ? next_offset : sizeof(found) - 1;
        }
        found_count++;
    }

    if (found_count == 0) {
        ESP_LOGW(TAG, "I2C scan found no devices on SDA GPIO %d / SCL GPIO %d", SATMON_IMU_SDA_GPIO,
                 SATMON_IMU_SCL_GPIO);
        return;
    }

    ESP_LOGI(TAG, "I2C scan found %d device(s): %s", found_count, found);
}

static esp_err_t mpu_detect_address(void)
{
    i2c_log_scan();

#if CONFIG_SATMON_NODE_PROFILE_SAT01_MPU
    const uint8_t candidates[] = {0x68, 0x69};
#elif CONFIG_SATMON_NODE_PROFILE_SAT02_GROVE_0X65
    const uint8_t candidates[] = {0x65, 0x6A, 0x6B};
#else
    const uint8_t candidates[] = {CONFIG_SATMON_MPU_I2C_ADDR, 0x65, 0x68, 0x69, 0x6A, 0x6B};
#endif
    esp_err_t first_err = ESP_ERR_NOT_FOUND;

    const size_t candidate_count = sizeof(candidates) / sizeof(candidates[0]);
    for (size_t i = 0; i < candidate_count; ++i) {
        bool already_checked = false;
        for (size_t previous = 0; previous < i; ++previous) {
            if (candidates[previous] == candidates[i]) {
                already_checked = true;
                break;
            }
        }
        if (already_checked) {
            continue;
        }

        const esp_err_t err = i2c_master_probe(s_i2c_bus, candidates[i], MPU_I2C_TIMEOUT_MS);
        if (err == ESP_OK) {
            s_mpu_i2c_addr = candidates[i];
            ESP_LOGI(TAG, "Using IMU at I2C address 0x%02x", s_mpu_i2c_addr);
            return ESP_OK;
        }
        if (i == 0) {
            first_err = err;
        }
    }

    ESP_LOGW(TAG,
             "IMU not found for selected node profile on SDA GPIO %d / SCL GPIO %d; check power, GND, SDA/SCL, and address select pins",
             SATMON_IMU_SDA_GPIO, SATMON_IMU_SCL_GPIO);
    return first_err;
}

static esp_err_t mpu_detect_model(void)
{
#if CONFIG_SATMON_NODE_PROFILE_SAT01_MPU
    {
        uint8_t who_am_i = 0;
        const esp_err_t err = mpu_read_regs(MPU_REG_WHO_AM_I, &who_am_i, sizeof(who_am_i));
        s_mpu_model = IMU_MODEL_MPU925X;
        s_mpu_who_am_i = err == ESP_OK ? who_am_i : 0;
        ESP_LOGW(TAG, "SAT-01 profile selected: forcing MPU-family register map at 0x%02x, WHO_AM_I=%s/0x%02x",
                 s_mpu_i2c_addr, esp_err_to_name(err), s_mpu_who_am_i);
        return ESP_OK;
    }
#elif CONFIG_SATMON_NODE_PROFILE_SAT02_GROVE_0X65
    ESP_LOGI(TAG, "SAT-02 profile selected: Grove IMU address 0x65/0x6a/0x6b, auto-detecting register map");
#else
#if CONFIG_SATMON_IMU_MODEL_MPU_FAMILY
    {
        uint8_t who_am_i = 0;
        const esp_err_t err = mpu_read_regs(MPU_REG_WHO_AM_I, &who_am_i, sizeof(who_am_i));
        s_mpu_model = IMU_MODEL_MPU925X;
        s_mpu_who_am_i = err == ESP_OK ? who_am_i : 0;
        ESP_LOGW(TAG, "IMU model forced by config: MPU-family at 0x%02x, WHO_AM_I=%s/0x%02x", s_mpu_i2c_addr,
                 esp_err_to_name(err), s_mpu_who_am_i);
        return ESP_OK;
    }
#elif CONFIG_SATMON_IMU_MODEL_LSM6DS_FAMILY
    {
        uint8_t who_am_i = 0;
        const esp_err_t err = mpu_read_regs(LSM6DS_REG_WHO_AM_I, &who_am_i, sizeof(who_am_i));
        s_mpu_model = IMU_MODEL_LSM6DS;
        s_mpu_who_am_i = err == ESP_OK ? who_am_i : 0;
        ESP_LOGW(TAG, "IMU model forced by config: LSM6DS-family at 0x%02x, WHO_AM_I=%s/0x%02x", s_mpu_i2c_addr,
                 esp_err_to_name(err), s_mpu_who_am_i);
        return ESP_OK;
    }
#endif
#endif

    uint8_t mpu_who_am_i = 0;
    const esp_err_t mpu_err = mpu_read_regs(MPU_REG_WHO_AM_I, &mpu_who_am_i, sizeof(mpu_who_am_i));
    if (mpu_err == ESP_OK && mpu_who_am_i_supported(mpu_who_am_i)) {
        s_mpu_model = IMU_MODEL_MPU925X;
        s_mpu_who_am_i = mpu_who_am_i;
        ESP_LOGI(TAG, "MPU-family IMU detected at 0x%02x, WHO_AM_I=0x%02x", s_mpu_i2c_addr, mpu_who_am_i);
        return ESP_OK;
    }

    uint8_t lsm6ds_who_am_i = 0;
    const esp_err_t lsm6ds_err = mpu_read_regs(LSM6DS_REG_WHO_AM_I, &lsm6ds_who_am_i, sizeof(lsm6ds_who_am_i));
    if (lsm6ds_err == ESP_OK && lsm6ds_who_am_i_supported(lsm6ds_who_am_i)) {
        s_mpu_model = IMU_MODEL_LSM6DS;
        s_mpu_who_am_i = lsm6ds_who_am_i;
        ESP_LOGI(TAG, "LSM6DS-family IMU detected at 0x%02x, WHO_AM_I=0x%02x", s_mpu_i2c_addr,
                 lsm6ds_who_am_i);
        return ESP_OK;
    }

    ESP_LOGE(TAG,
             "Unsupported IMU identity at 0x%02x (MPU WHO_AM_I %s/0x%02x, LSM6DS WHO_AM_I %s/0x%02x)",
             s_mpu_i2c_addr, esp_err_to_name(mpu_err), mpu_who_am_i, esp_err_to_name(lsm6ds_err),
             lsm6ds_who_am_i);
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t mpu925x_configure(void)
{
    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU_REG_PWR_MGMT_1, 0x80), TAG, "Failed to reset MPU");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU_REG_PWR_MGMT_1, 0x01), TAG, "Failed to wake MPU");
    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU_REG_PWR_MGMT_2, 0x00), TAG, "Failed to enable MPU axes");
    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU_REG_CONFIG, 0x03), TAG, "Failed to configure MPU DLPF");
    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU_REG_SMPLRT_DIV, 0x04), TAG, "Failed to configure MPU sample rate");
    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU_REG_GYRO_CONFIG, 0x00), TAG, "Failed to configure MPU gyro range");
    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU_REG_ACCEL_CONFIG, 0x00), TAG, "Failed to configure MPU accel range");
    ESP_RETURN_ON_ERROR(mpu_write_reg(MPU_REG_ACCEL_CONFIG2, 0x03), TAG, "Failed to configure MPU accel DLPF");

    return ESP_OK;
}

static esp_err_t lsm6ds_configure(void)
{
    ESP_RETURN_ON_ERROR(mpu_write_reg(LSM6DS_REG_CTRL3_C, 0x01), TAG, "Failed to reset LSM6DS");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(mpu_write_reg(LSM6DS_REG_CTRL3_C, 0x44), TAG,
                        "Failed to enable LSM6DS block-data-update and register auto-increment");
    ESP_RETURN_ON_ERROR(mpu_write_reg(LSM6DS_REG_CTRL1_XL, 0x40), TAG,
                        "Failed to configure LSM6DS accelerometer");
    ESP_RETURN_ON_ERROR(mpu_write_reg(LSM6DS_REG_CTRL2_G, 0x40), TAG, "Failed to configure LSM6DS gyroscope");
    ESP_RETURN_ON_ERROR(mpu_write_reg(LSM6DS_REG_CTRL9_XL, 0x38), TAG, "Failed to enable LSM6DS accel axes");
    ESP_RETURN_ON_ERROR(mpu_write_reg(LSM6DS_REG_CTRL10_C, 0x38), TAG, "Failed to enable LSM6DS gyro axes");

    return ESP_OK;
}

static esp_err_t imu_read_uncalibrated_sample(mpu_sample_t *sample)
{
    switch (s_mpu_model) {
    case IMU_MODEL_MPU925X:
        return mpu925x_read_sample(sample);
    case IMU_MODEL_LSM6DS:
        return lsm6ds_read_sample(sample);
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

static esp_err_t imu_calibrate_gyro_bias(void)
{
    int64_t sum_x = 0;
    int64_t sum_y = 0;
    int64_t sum_z = 0;

    vTaskDelay(pdMS_TO_TICKS(IMU_GYRO_BIAS_SETTLE_MS));

    for (int sample_index = 0; sample_index < IMU_GYRO_BIAS_SAMPLES; ++sample_index) {
        mpu_sample_t sample = {0};
        ESP_RETURN_ON_ERROR(imu_read_uncalibrated_sample(&sample), TAG, "Failed to read IMU during gyro calibration");

        sum_x += sample.gyro_raw.x;
        sum_y += sample.gyro_raw.y;
        sum_z += sample.gyro_raw.z;
        vTaskDelay(pdMS_TO_TICKS(IMU_GYRO_BIAS_SAMPLE_DELAY_MS));
    }

    s_gyro_bias.x = clamp_i16(rounded_average(sum_x, IMU_GYRO_BIAS_SAMPLES));
    s_gyro_bias.y = clamp_i16(rounded_average(sum_y, IMU_GYRO_BIAS_SAMPLES));
    s_gyro_bias.z = clamp_i16(rounded_average(sum_z, IMU_GYRO_BIAS_SAMPLES));

    ESP_LOGI(TAG, "Gyro zero bias calibrated: x=%d y=%d z=%d raw counts", s_gyro_bias.x, s_gyro_bias.y,
             s_gyro_bias.z);

    return ESP_OK;
}

static esp_err_t mpu_init(void)
{
    if (s_mpu_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(mpu_bus_init(), TAG, "Failed to initialize MPU I2C bus");

    if (s_mpu_dev == NULL) {
        ESP_RETURN_ON_ERROR(mpu_detect_address(), TAG, "Failed to detect MPU I2C address");

        const i2c_device_config_t mpu_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = s_mpu_i2c_addr,
            .scl_speed_hz = SATMON_IMU_I2C_FREQ_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &mpu_config, &s_mpu_dev), TAG,
                            "Failed to add MPU device");
    }

    if (s_mpu_model == IMU_MODEL_UNKNOWN) {
        ESP_RETURN_ON_ERROR(mpu_detect_model(), TAG, "Failed to detect IMU model");
    }

    switch (s_mpu_model) {
    case IMU_MODEL_MPU925X:
        ESP_RETURN_ON_ERROR(mpu925x_configure(), TAG, "Failed to configure MPU-925x");
        break;
    case IMU_MODEL_LSM6DS:
        ESP_RETURN_ON_ERROR(lsm6ds_configure(), TAG, "Failed to configure LSM6DS-family IMU");
        break;
    default:
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(imu_calibrate_gyro_bias(), TAG, "Failed to calibrate IMU gyro bias");

    s_mpu_ready = true;
    return ESP_OK;
}

static bool json_escape_string(char *dest, size_t dest_len, const char *src)
{
    size_t output_index = 0;

    if (dest_len == 0) {
        return false;
    }

    for (size_t input_index = 0; src[input_index] != '\0'; ++input_index) {
        const unsigned char character = (unsigned char)src[input_index];
        const char *escape = NULL;

        switch (character) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }

        if (escape != NULL) {
            const size_t escape_len = strlen(escape);
            if (output_index + escape_len >= dest_len) {
                return false;
            }
            memcpy(&dest[output_index], escape, escape_len);
            output_index += escape_len;
            continue;
        }

        if (character < 0x20) {
            if (output_index + 6 >= dest_len) {
                return false;
            }
            snprintf(&dest[output_index], dest_len - output_index, "\\u%04x", character);
            output_index += 6;
            continue;
        }

        if (output_index + 1 >= dest_len) {
            return false;
        }
        dest[output_index++] = (char)character;
    }

    dest[output_index] = '\0';
    return true;
}

static bool format_timestamp_json(char *buffer, size_t buffer_len)
{
    time_t now = 0;
    struct tm utc_time = {0};
    char iso_timestamp[sizeof("2026-04-20T14:32:10Z")];

    time(&now);
    if (gmtime_r(&now, &utc_time) == NULL || utc_time.tm_year < (SATMON_MIN_VALID_YEAR - 1900) ||
        strftime(iso_timestamp, sizeof(iso_timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0) {
        return snprintf(buffer, buffer_len, "null") > 0;
    }

    return snprintf(buffer, buffer_len, "\"%s\"", iso_timestamp) > 0;
}

static bool build_payload_metadata(char *satellite_id, size_t satellite_id_len, char *timestamp_json,
                                   size_t timestamp_json_len)
{
    if (!json_escape_string(satellite_id, satellite_id_len, satmon_satellite_id())) {
        ESP_LOGW(TAG, "Satellite ID is too long for MQTT payload metadata");
        return false;
    }

    if (!format_timestamp_json(timestamp_json, timestamp_json_len)) {
        ESP_LOGW(TAG, "Timestamp is too long for MQTT payload metadata");
        return false;
    }

    return true;
}

static void publish_payload(esp_mqtt_client_handle_t client, const char *payload, const char *kind)
{
    const int msg_id = esp_mqtt_client_publish(client, CONFIG_SATMON_MPU_MQTT_TOPIC, payload, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to enqueue MPU MQTT %s publish", kind);
    } else {
        ESP_LOGI(TAG, "Published MPU %s to %s, msg_id=%d", kind, CONFIG_SATMON_MPU_MQTT_TOPIC, msg_id);
    }
}

static void publish_mpu_sample(esp_mqtt_client_handle_t client, const mpu_sample_t *sample)
{
    char payload[768];
    char satellite_id[SATMON_SATELLITE_ID_JSON_LEN];
    char timestamp_json[SATMON_TIMESTAMP_JSON_LEN];

    if (!build_payload_metadata(satellite_id, sizeof(satellite_id), timestamp_json, sizeof(timestamp_json))) {
        return;
    }

    const int payload_len = snprintf(
        payload, sizeof(payload),
        "{\"satellite_id\":\"%s\",\"timestamp\":%s,"
        "\"sensors\":{\"%s\":{"
        "\"model\":\"%s\",\"i2c_address\":\"0x%02x\",\"who_am_i\":\"0x%02x\","
        "\"gyroscope_udps_per_lsb\":%d,\"accelerometer_ug_per_lsb\":%d,"
        "\"accelerometer\":{\"x\":%d,\"y\":%d,\"z\":%d},"
        "\"gyroscope\":{\"x\":%d,\"y\":%d,\"z\":%d}"
        "},\"temperature\":%d"
        "}}",
        satellite_id, timestamp_json, SATMON_SENSOR_ID, imu_model_name(), s_mpu_i2c_addr, s_mpu_who_am_i,
        imu_gyro_udps_per_lsb(), imu_accel_ug_per_lsb(), sample->accel_raw.x, sample->accel_raw.y,
        sample->accel_raw.z, sample->gyro_raw.x, sample->gyro_raw.y, sample->gyro_raw.z, sample->temp_raw);

    if (payload_len < 0 || payload_len >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "MPU MQTT sample payload did not fit in buffer");
        return;
    }

    publish_payload(client, payload, "sample");
}

static void initialize_time_sync(void)
{
    if (CONFIG_SATMON_SNTP_SERVER[0] == '\0') {
        ESP_LOGW(TAG, "SNTP time sync disabled because no server is configured");
        return;
    }

    ESP_LOGI(TAG, "Starting SNTP time sync with %s", CONFIG_SATMON_SNTP_SERVER);
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_SATMON_SNTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize SNTP: %s", esp_err_to_name(err));
        return;
    }

    if (CONFIG_SATMON_SNTP_SYNC_TIMEOUT_MS == 0) {
        ESP_LOGI(TAG, "SNTP initial sync wait skipped");
        return;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(CONFIG_SATMON_SNTP_SYNC_TIMEOUT_MS));
    if (err == ESP_OK) {
        char timestamp_json[SATMON_TIMESTAMP_JSON_LEN];
        if (format_timestamp_json(timestamp_json, sizeof(timestamp_json))) {
            ESP_LOGI(TAG, "System time synced: %s", timestamp_json);
        } else {
            ESP_LOGI(TAG, "System time synced");
        }
    } else {
        ESP_LOGW(TAG, "SNTP sync not available within %d ms: %s", CONFIG_SATMON_SNTP_SYNC_TIMEOUT_MS,
                 esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, SATMON_WIFI_CONNECTED_BIT);

        if (CONFIG_SATMON_WIFI_CONN_MAX_RETRY < 0 || s_wifi_retry_count < CONFIG_SATMON_WIFI_CONN_MAX_RETRY) {
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying connection to %s (%d)", CONFIG_SATMON_WIFI_SSID,
                     s_wifi_retry_count);
            ESP_ERROR_CHECK(esp_wifi_connect());
        } else {
            ESP_LOGE(TAG, "Wi-Fi connection failed after %d retries", CONFIG_SATMON_WIFI_CONN_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, SATMON_WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi connected, IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, SATMON_WIFI_CONNECTED_BIT);
    }
}

static esp_err_t satmon_wifi_connect(void)
{
    if (CONFIG_SATMON_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty. Set it under SatMon Configuration -> Wi-Fi connection");
        return ESP_ERR_INVALID_ARG;
    }

    s_wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create Wi-Fi event group");

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(sta_netif != NULL, ESP_FAIL, TAG, "Failed to create default Wi-Fi station");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Failed to initialize Wi-Fi");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL),
                        TAG, "Failed to register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL),
                        TAG, "Failed to register IP event handler");

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", CONFIG_SATMON_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", CONFIG_SATMON_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID %s", CONFIG_SATMON_WIFI_SSID);
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set Wi-Fi station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "Failed to set Wi-Fi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start Wi-Fi");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, SATMON_WIFI_CONNECTED_BIT | SATMON_WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if ((bits & SATMON_WIFI_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

static void mpu_publish_task(void *arg)
{
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)arg;
    TickType_t publish_interval_ticks = pdMS_TO_TICKS(CONFIG_SATMON_MPU_PUBLISH_INTERVAL_MS);
    if (publish_interval_ticks == 0) {
        publish_interval_ticks = 1;
    }

    while (true) {
        xEventGroupWaitBits(s_mqtt_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        esp_err_t err = mpu_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MPU init failed: %s; skipping publish", esp_err_to_name(err));
            vTaskDelay(publish_interval_ticks);
            continue;
        }

        mpu_sample_t sample = {0};
        err = mpu_read_sample(&sample);
        if (err == ESP_OK) {
            publish_mpu_sample(client, &sample);
        } else {
            s_mpu_ready = false;
            ESP_LOGW(TAG, "MPU read failed: %s; skipping publish", esp_err_to_name(err));
        }

        vTaskDelay(publish_interval_ticks);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
#if CONFIG_SATMON_LED_ENABLE
        subscribe_led_command_topics(event->client);
#endif
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
#if CONFIG_SATMON_LED_ENABLE
        if (handle_led_mqtt_event(event)) {
            break;
        }
#endif
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGI(TAG, "Last captured errno : %d (%s)", event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = CONFIG_SATMON_MQTT_BROKER_URI,
#if CONFIG_SATMON_CERT_VALIDATE_CUSTOM
            .verification.certificate = cert_override_pem,
#elif CONFIG_SATMON_CERT_VALIDATE_MOSQUITTO_CA
            .verification.certificate = (const char *)mosquitto_org_crt_start,
#elif CONFIG_SATMON_CERT_VALIDATE_BUNDLE
            .verification.crt_bundle_attach = esp_crt_bundle_attach, /* Use built-in certificate bundle */
#endif
        },
    };

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    const BaseType_t task_created = xTaskCreate(mpu_publish_task, "mpu_publish", 4096, client, 5, NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MPU publish task");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("satmon_mpu_mqtt", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    s_mqtt_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(s_mqtt_event_group == NULL ? ESP_ERR_NO_MEM : ESP_OK);

#if CONFIG_SATMON_LED_ENABLE
    if (satmon_led_apply(false, s_led_red, s_led_green, s_led_blue) != ESP_OK) {
        ESP_LOGW(TAG, "Onboard RGB LED control is disabled until the LED driver can initialize");
    } else if (satmon_led_boot_blink_test() != ESP_OK) {
        ESP_LOGW(TAG, "Onboard RGB LED boot blink test failed");
    }
#endif

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "MPU publish settings: topic=%s interval=%d ms", CONFIG_SATMON_MPU_MQTT_TOPIC,
             CONFIG_SATMON_MPU_PUBLISH_INTERVAL_MS);

    ESP_ERROR_CHECK(satmon_wifi_connect());

    initialize_time_sync();

    mqtt_app_start();
}
