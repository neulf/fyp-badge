/**
 * @file main.c
 * @brief 应用启动入口，按依赖顺序装配 BLE、BSP、音频、语音和 LVGL 页面。
 */
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "app_music/lvgl_music.h"
#include "app_msg/sys_msg_ui.h"
#include "app_eaf/eaf_test.h"
#include "wake_word_det/wake_word_drv.h"
#include "driver_test/driver_test_ui.h"
#include "ble_control/ble_control.h"
#include "esp_log.h"

#define TAG   "main"

static generic_file_list_t mp3_files;
static lv_obj_t *main_screen; 

void app_main(void)
{
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    // Initialize BLE while internal RAM is still available to the controller.
    bool ble_ready = (ble_control_init() == ESP_OK);
    if (!ble_ready) {
        ESP_LOGE(TAG, "BLE is unavailable; continuing without phone control");
    }

    lv_display_t *disp = bsp_display_start();
    lv_indev_t *tp = bsp_display_get_input_dev();
    bsp_display_backlight_on();
    esp_err_t sd_err = bsp_sdcard_mount();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s); continuing without local music",
                 esp_err_to_name(sd_err));
    }
    
    Audio_Play_Init();
    msg_driver_init();
    wake_word_drv_init();

    esp_err_t err = sd_err == ESP_OK
                        ? get_file_list_by_ext("/sdcard/music", ".mp3", &mp3_files)
                        : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        for (int i = 0; i < mp3_files.count; i++) {
            printf("MP3[%d]: %s\n", i, mp3_files.list[i]);
        }
        if (mp3_files.count > 0) {
            ble_control_set_default_track(mp3_files.list[0]);
        }
    }

    bsp_display_lock(-1);

    main_screen = lv_obj_create(NULL);
    lv_obj_t * tv = lv_tileview_create(main_screen);

    lv_obj_t * tile1 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    msg_ui_screen_init(main_screen,tile1);
    

    lv_obj_t * tile2 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    init_music_ui_screen(tile2,&mp3_files);

    lv_obj_t * tile3 = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);
    eaf_ui_screen_init(tile3);

    driver_test_ui_init(main_screen);

    lv_screen_load(main_screen);

    bsp_display_unlock();

    if (ble_ready) {
        ESP_ERROR_CHECK(ble_control_set_advertising(true));
    }
}
