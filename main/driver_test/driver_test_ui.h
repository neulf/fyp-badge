#pragma once

#include "bsp/esp-bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

void driver_test_ui_init(lv_obj_t *return_screen);
void driver_test_ui_open_microphone(void);
void driver_test_ui_open_bluetooth(void);
void driver_test_ui_set_voice_state(const char *text, bool active);
void driver_test_ui_set_voice_result(const char *text);
void driver_test_ui_set_mic_levels(uint8_t mic1_percent, uint8_t mic2_percent);
void driver_test_ui_set_ble_state(const char *text, bool connected);
void driver_test_ui_set_ble_last_command(const char *text);
void driver_test_ui_set_ble_scan_result(const char *name, const char *address, int8_t rssi);

#ifdef __cplusplus
}
#endif
