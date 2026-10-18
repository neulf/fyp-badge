file(GLOB_RECURSE MAIN_SRCS ./*.cpp ./*.c)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "."
    REQUIRES waveshare__esp32_s3_touch_lcd_1_85B bsp_extra espressif__bq27220
             waveshare__pcf85063a espressif__i2c_bus espressif__esp-sr
             espressif__esp_lv_eaf_player lvgl__lvgl nvs_flash esp_wifi
             esp_event esp_netif esp_system
    PRIV_REQUIRES bt
    EMBED_FILES "eaf_test.eaf" 
)
