#pragma once

#include "esp_err.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_control_state_cb_t)(const char *text, bool connected);
typedef void (*ble_control_command_cb_t)(const char *text);
typedef void (*ble_control_scan_cb_t)(const char *name, const char *address, int8_t rssi);

esp_err_t ble_control_init(void);
esp_err_t ble_control_set_advertising(bool enabled);
esp_err_t ble_control_scan_start(void);
esp_err_t ble_control_scan_stop(void);
void ble_control_bind_ui(ble_control_state_cb_t state_cb,
                         ble_control_command_cb_t command_cb);
void ble_control_bind_scan_ui(ble_control_scan_cb_t scan_cb);
void ble_control_set_default_track(const char *path);

/* Uploads are accepted only while an SD card is mounted. */
#define BLE_IMAGE_UPLOAD_SERVICE_UUID "219a143e-701b-4d91-a481-9d5220100001"
#define BLE_IMAGE_UPLOAD_CHAR_UUID    "219a143e-701b-4d91-a481-9d5220100003"

#ifdef __cplusplus
}
#endif
