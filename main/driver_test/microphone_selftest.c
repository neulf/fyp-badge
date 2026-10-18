/**
 * @file microphone_selftest.c
 * @brief 可选的麦克风停止/启动回归测试，仅在 CONFIG_TEMPLATE_MIC_SELFTEST
 *        开启时编译有效。
 */
#include "microphone_selftest.h"

#ifdef CONFIG_TEMPLATE_MIC_SELFTEST
#include "driver_test_ui.h"
#include "wake_word_det/wake_word_drv.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <assert.h>

/* Runs on the LVGL thread so the probe exercises the same path as touch input. */
static void selftest_tick(lv_timer_t *timer)
{
    static unsigned phase, cycles;
    static uint32_t before;
    lv_obj_t *button = lv_timer_get_user_data(timer);
    if (!wake_word_drv_is_ready()) return;

    switch (phase) {
    case 0:
        driver_test_ui_open_microphone();
        assert(wake_word_drv_is_active());
        lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
        assert(!wake_word_drv_is_active());
        phase = 1;
        break;
    case 1:
        /* Allow one in-flight codec read to finish before checking quiescence. */
        before = wake_word_drv_frame_count();
        phase = 2;
        break;
    case 2:
        assert(wake_word_drv_frame_count() == before);
        lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
        assert(wake_word_drv_is_active());
        phase = 3;
        break;
    case 3:
        assert(wake_word_drv_frame_count() > before);
        ESP_LOGI("mic_selftest", "Cycle %u/10 PASS; frames=%lu DMA largest=%u",
                 ++cycles, (unsigned long)wake_word_drv_frame_count(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        phase = 0;
        if (cycles == 10) {
            ESP_LOGI("mic_selftest", "PASS: 10 UI stop/start cycles, capture resumed each time");
            lv_timer_delete(timer);
        }
        break;
    }
}

void microphone_selftest_start(lv_obj_t *button)
{
    lv_timer_create(selftest_tick, 1000, button);
}
#endif
