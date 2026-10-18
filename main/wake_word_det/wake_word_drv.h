#pragma once

#include "esp_log.h"
#include "bsp/esp-bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

void wake_word_drv_init(void);
void pause_wake_word_task();
void resume_wake_word_task();
bool wake_word_drv_is_ready(void);
bool wake_word_drv_is_active(void);
/* Counts successfully captured PCM frames, for runtime health diagnostics. */
uint32_t wake_word_drv_frame_count(void);
typedef void (*wake_word_state_cb_t)(const char *text, bool active);
typedef void (*wake_word_result_cb_t)(const char *text);
typedef void (*wake_word_level_cb_t)(uint8_t mic1_percent, uint8_t mic2_percent);
void wake_word_drv_bind_ui(wake_word_state_cb_t state_cb,
                           wake_word_result_cb_t result_cb,
                           wake_word_level_cb_t level_cb);
#ifdef __cplusplus
}
#endif
