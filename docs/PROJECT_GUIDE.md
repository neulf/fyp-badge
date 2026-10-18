# 项目模块与函数说明

## 1. 项目定位

这是 Waveshare ESP32-S3 Touch LCD 1.85B 的 ESP-IDF 5.5.5 模板。

模块分层：

- 启动层：main/main.c，负责初始化顺序和模块装配。
- 业务层：main/app_msg、main/app_music、main/app_eaf、main/driver_test、main/wake_word_det、main/ble_control。
- 板级层：components/waveshare__esp32_s3_touch_lcd_1_85B。
- 驱动层：components/bsp_extra、components/waveshare__qmi8658、components/waveshare__pcf85063a。

依赖方向保持为：启动层调用业务层，业务层调用板级和驱动层；硬件组件不反向调用业务 UI。

## 2. 启动流程

main/main.c 的 app_main 依次执行：

1. 初始化 NVS。
2. 初始化 NimBLE，先占用蓝牙控制器所需的内部 RAM。
3. 启动 LVGL 显示、触摸和背光。
4. 挂载 SD 卡；失败时保留界面和 BLE，音乐及图片功能报告不可用。
5. 初始化扬声器、设置页和 ESP-SR 麦克风。
6. 扫描 /sdcard/music 并设置默认曲目。
7. 创建设置、音乐、EAF 和驱动测试页面。
8. 启动 BLE 广播。

## 3. 文件和函数索引

### 3.1 启动与配置

- main/main.c：app_main 完成 NVS、BLE、显示、SD 卡、音频、语音和 LVGL 页面初始化。
- main/CMakeLists.txt：收集 main 下源文件，声明组件依赖并嵌入 EAF 文件。
- main/idf_component.yml：主组件依赖元数据。
- sdkconfig.defaults：ESP32-S3、PSRAM、BLE、LVGL、FreeRTOS 默认配置。
- partitions.csv：NVS、OTA、factory 应用和 ESP-SR 模型分区定义。

### 3.2 音频播放

文件：components/bsp_extra/src/bsp_board_extra.c

- Audio_Play_Init：创建扬声器 codec 和播放器。
- Volume_Adjustment：设置音量。
- get_audio_volume：读取音量。
- Audio_Play_Music：停止旧曲目并播放指定 URL。
- Audio_Stop_Play：停止播放并关闭功放。
- Audio_Resume_Play：恢复播放并打开功放。
- Audio_Pause_Play：暂停播放并关闭功放。
- Audio_Get_Current_State：读取播放器状态。
- Audio_Play_Deinit：关闭功放并销毁播放器。
- create_simple_player：配置 Simple Player 的输入、输出和事件回调。
- out_data_callback：把解码 PCM 写入扬声器 codec。
- mock_event_callback：处理音乐信息和播放状态事件。
- Audio_PA_EN / Audio_PA_DIS：功放控制预留接口。

公共声明：components/bsp_extra/include/bsp_board_extra.h。

### 3.3 板级支持

文件：components/waveshare__esp32_s3_touch_lcd_1_85B/esp32_s3_touch_lcd_1_85B.c

该文件聚合 LCD、CST816S 触摸、I2C、I2S、ES8311、ES7210、SD 卡和 QMI8658。

- bsp_audio_init：初始化 I2S。
- bsp_audio_get_codec_itf：返回 codec 数据接口。
- bsp_audio_codec_speaker_init：创建 ES8311 扬声器设备。
- bsp_audio_codec_microphone_init：创建 ES7210 麦克风设备。
- bsp_i2c_init / bsp_i2c_deinit：初始化和释放 I2C 总线。
- bsp_i2c_get_handle / bsp_i2c_bus_get_handle：返回 I2C 句柄。
- bsp_spiffs_mount / bsp_spiffs_unmount：挂载和卸载 SPIFFS。
- bsp_sdcard_mount / bsp_sdcard_unmount：挂载和卸载 SD 卡。
- bsp_sdcard_get_handle：返回当前 SD 卡句柄；NULL 表示未挂载。
- bsp_sdcard_get_sdmmc_host：生成 SDMMC 主机配置。
- bsp_sdcard_get_sdspi_host：生成 SDSPI 主机配置；当前硬件不支持。
- bsp_sdcard_sdmmc_get_slot：生成 SDMMC 引脚配置。
- bsp_sdcard_sdspi_get_slot：生成 SDSPI 引脚配置；当前硬件不支持。
- bsp_sdcard_sdmmc_mount：执行实际 SDMMC 挂载。
- bsp_sdcard_sdspi_mount：SDSPI 挂载占位接口。
- get_file_list_by_ext：扫描目录并返回指定扩展名文件列表。
- bsp_display_start / bsp_display_start_with_config：创建 LVGL 显示和触摸。
- bsp_display_get_disp_dev / bsp_display_get_input_dev：返回 LVGL 设备。
- bsp_display_lock / bsp_display_unlock：保护跨线程 LVGL 访问。
- bsp_qmi8658_drv_init：创建板载 QMI8658 实例。

相关头文件：

- include/bsp/esp-bsp.h：聚合 BSP 头文件。
- include/bsp/esp32_s3_touch_lcd_1_85B.h：引脚、结构体和公共 API。
- include/bsp/display.h：显示、背光和亮度 API。
- include/bsp/touch.h：bsp_touch_new 创建 CST816S 触摸设备。
- include/bsp/config.h：板级编译配置。
- priv_include/bsp_err_check.h：BSP 内部错误检查宏。

### 3.4 QMI8658 六轴传感器

文件：components/waveshare__qmi8658/qmi8658.c

- qmi8658_init：注册 I2C 设备并配置默认参数。
- qmi8658_set_accel_range / qmi8658_set_gyro_range：设置量程。
- qmi8658_set_accel_odr / qmi8658_set_gyro_odr：设置输出数据率。
- qmi8658_enable_accel / qmi8658_enable_gyro / qmi8658_enable_sensors：启停通道。
- qmi8658_read_accel / qmi8658_read_gyro / qmi8658_read_temp：读取单项数据。
- qmi8658_read_sensor_data：读取完整数据帧。
- qmi8658_read_accel_mg / qmi8658_read_accel_mps2：读取并转换加速度单位。
- qmi8658_read_gyro_dps / qmi8658_read_gyro_rads：读取并转换角速度单位。
- qmi8658_is_data_ready：检查新数据。
- qmi8658_get_who_am_i：读取芯片身份。
- qmi8658_reset：复位传感器。
- qmi8658_set_accel_unit_mps2 / qmi8658_set_accel_unit_mg：设置加速度单位。
- qmi8658_set_gyro_unit_rads / qmi8658_set_gyro_unit_dps：设置角速度单位。
- qmi8658_set_display_precision / qmi8658_set_display_precision_enum / qmi8658_get_display_precision：设置或读取显示精度。
- qmi8658_is_accel_unit_mps2、qmi8658_is_accel_unit_mg、qmi8658_is_gyro_unit_rads、qmi8658_is_gyro_unit_dps：查询单位。
- qmi8658_enable_wake_on_motion / qmi8658_disable_wake_on_motion：配置运动唤醒。
- qmi8658_write_register / qmi8658_read_register：底层寄存器访问。

测试文件 components/waveshare__qmi8658/test_app/main/test_qmi8658.c：

- i2c_master_init：初始化测试 I2C。
- qmi8658_test_task：周期读取传感器并打印。

### 3.5 PCF85063A RTC

文件：components/waveshare__pcf85063a/pcf85063a.c

- pcf85063a_init：注册 RTC I2C 设备并配置控制寄存器。
- pcf85063a_reset：复位 RTC。
- pcf85063a_set_time / pcf85063a_set_date：写入时间或日期。
- pcf85063a_set_time_date：写入完整日期时间。
- pcf85063a_get_time_date：读取完整日期时间。
- pcf85063a_enable_alarm：启用闹钟。
- pcf85063a_get_alarm_flag：读取闹钟标志。
- pcf85063a_set_alarm / pcf85063a_get_alarm：设置或读取闹钟时间。
- pcf85063a_datetime_to_str：格式化日期时间。
- pcf85063a_write_register / pcf85063a_read_register：底层寄存器访问。
- decToBcd / bcdToDec：十进制和 BCD 编码转换。

测试文件 components/waveshare__pcf85063a/test_app/main/test_pcf85063a.c：

- i2c_master_init：初始化测试 I2C。
- pcf85063a_test_task：周期读取并打印 RTC 时间。

### 3.6 设置和系统信息 UI

文件：main/app_msg/sys_msg_ui.c

- msg_driver_init：创建 RTC、电池驱动并初始化设置页依赖。
- msg_ui_screen_init：创建设置页和状态信息。
- wifi_btn_click_cb / bat_msg_btn_click_cb / about_btn_click_cb：打开对应子页面。
- microphone_test_btn_click_cb / bluetooth_test_btn_click_cb：打开驱动测试页。
- slider_value_change_sound_vol_cb：把音量滑块值写入音频模块。
- slider_value_change_brightness_cb：把亮度滑块值写入 BSP。
- update_timer_cb：周期刷新时间、电量和系统状态。
- add_button_with_arrow：创建带箭头的设置项。
- add_button_with_slider：创建带滑块的设置项。
- add_button_with_label：创建只读文本设置项。

子目录 main/app_msg/submenu_ui：

- set_about.c：init_about_settings_screen 创建关于页；back_btn_click_event_cb 返回；get_mac_address 获取 MAC；init_about_lv_style 初始化样式。
- set_bat_msg.c：bq27220_drv_init 创建电池驱动；init_bat_settings_screen 创建电池页；定时器回调刷新电量。
- set_lv_style.c：get_set_screen_styles 返回设置页样式。
- set_wifi_service.c：wifi_init_sta 初始化 STA；init_wifi_service_screen 创建 Wi-Fi 页；wifi_event_handler 处理 Wi-Fi 事件；wifi_scan_ui_task 更新扫描结果。

main/app_msg/assets 下的 arrow.c、backlights.c、battery.c、info.c、sound.c、WIFI.c 只保存 LVGL 图片数据。

### 3.7 音乐 UI

文件：main/app_music/lvgl_music.c

- init_music_ui_screen：创建音乐页、曲目列表和播放控制。
- ui_set_playing：同步播放/暂停按钮状态。
- play_btn_event_cb：开始或暂停曲目。
- last_btn_event_cb / next_btn_event_cb：切换曲目。
- vol_btn_event_cb：打开音量控制。
- menu_btn_event_cb：处理菜单入口。

main/app_music/assets 下的 backgroud.c、spectrum.c、last.c、menu.c、next.c、play.c、stop.c、vol.c 只保存图片和控制图标。

### 3.8 EAF 动画

文件：main/app_eaf/eaf_test.c

- eaf_ui_screen_init：把嵌入的 eaf_test.eaf 建成 LVGL 图片描述，创建并居中显示 EAF 动画。

EAF 由 main/CMakeLists.txt 的 EMBED_FILES 嵌入固件，不依赖 SD 卡。

### 3.9 BLE 和 SD 卡图片上传

文件：main/ble_control/ble_control.c

BLE 函数：

- ble_control_init：初始化 NimBLE、GAP/GATT 和 BLE 主机任务。
- ble_control_set_advertising：打开或关闭广播。
- ble_control_scan_start / ble_control_scan_stop：启动或取消扫描。
- ble_control_bind_ui / ble_control_bind_scan_ui：注册 UI 回调。
- ble_control_set_default_track：设置 PLAY 命令的默认曲目。
- start_advertising：配置设备名、UUID 和广播参数。
- host_sync_cb / host_reset_cb：处理 NimBLE 同步和重置。
- host_task：运行 NimBLE 主循环。
- execute_command：解析 PING、PLAY、PAUSE、STOP、VOL:n。
- gatt_access_cb：分发控制特征值读写和图片特征值写入。
- gap_event_cb：处理连接、断开和广播完成。
- scan_event_cb：解析设备名、地址和 RSSI。

图片上传函数：

- image_upload_begin：检查 SD 卡、文件名、大小和 CRC，创建 .part 文件。
- image_upload_chunk：检查连续偏移，写入分包并累计 CRC32。
- image_upload_end：检查长度和 CRC，成功后重命名正式文件。
- image_upload_abort：关闭文件并删除未完成文件。
- image_name_is_safe：阻止路径穿越和不支持的扩展名。
- image_upload_access_cb：解析 BEGIN、CHUNK、END 数据包。
- image_upload_status：更新状态值并打印日志。

协议：0x01 加 UTF-8 元数据 filename,size,crc32hex；0x02 加 little-endian offset 和数据；0x03 结束。目录为 /sdcard/images，支持 JPG、JPEG、PNG、BMP，最大 4 MiB。

### 3.10 语音输入和唤醒词

文件：main/wake_word_det/wake_word_drv.c

- wake_word_drv_init：打开 ES7210、加载 ESP-SR 模型、创建任务。
- feed_Task：读取 I2S 四路帧，计算两个麦克风 RMS 并送入 AFE。
- detect_Task：处理 AFE、唤醒词、MultiNet 英文命令和播放恢复。
- pause_wake_word_task / resume_wake_word_task：用事件位协作暂停和恢复。
- wake_word_drv_is_ready / wake_word_drv_is_active：查询状态。
- wake_word_drv_frame_count：返回成功读取的 PCM 帧数。
- wake_word_drv_bind_ui：注册状态、结果和音量回调。
- microphone_level：把 RMS 转成 0-100 显示级别。
- bsp_get_feed_data：读取 codec 数据并处理 RMNM 通道布局。
- bsp_get_feed_channel / bsp_get_input_format：描述 ESP-SR 输入格式。

### 3.11 驱动测试 UI

文件：main/driver_test/driver_test_ui.c

- driver_test_ui_init：创建麦克风页和 BLE 调试页并绑定回调。
- driver_test_ui_open_microphone / driver_test_ui_open_bluetooth：切换页面。
- voice_button_event_cb：启动或协作停止麦克风任务。
- ble_button_event_cb：启动或停止 BLE 广播。
- ble_scan_button_event_cb：启动或停止扫描。
- voice_ui_timer_cb：在 LVGL 线程消费音频快照并更新控件。
- driver_test_ui_set_voice_state / driver_test_ui_set_voice_result：缓存语音状态和结果。
- driver_test_ui_set_mic_levels：缓存两路音量。
- driver_test_ui_set_ble_state / driver_test_ui_set_ble_last_command：更新 BLE 状态和命令结果。
- driver_test_ui_set_ble_scan_result：更新扫描设备摘要。
- make_action_button：创建统一按钮和 pressed、checked、disabled 状态。
- create_section / create_status_label：创建页面分区和状态文字。
- back_to_settings_event_cb：返回设置页。

文件 main/driver_test/microphone_selftest.c：

- microphone_selftest_start：创建可选回归测试定时器。
- selftest_tick：连续执行十轮停止和启动，检查 PCM 是否停止并恢复。

main/driver_test/ui_theme.h：集中定义颜色、间距、圆角和按钮尺寸。

## 4. 资源文件

main/app_msg/assets/*.c 和 main/app_music/assets/*.c 都是编译期 LVGL 图片数据，不包含运行时业务函数。

## 5. 线程和资源规则

- LVGL 对象只能在 LVGL 线程或持有 bsp_display_lock 时访问。
- 语音和 BLE 回调只写状态快照，LVGL 定时器负责更新对象。
- 采音和识别任务使用 PSRAM 栈；停止语音使用事件位，不强制挂起任务。
- 图片上传只在 SD 卡句柄有效时运行，未完成上传使用 .part 文件。
- SD 卡挂载失败不应导致整机重启。
- 新硬件驱动放到 components/<vendor>__<device>。
- 业务 UI 放到 main/app_* 或独立业务目录。
- 公共接口写入对应 .h，跨模块不要直接引用静态变量。

## 6. 构建

环境要求：ESP-IDF 5.5.5、Python v5.5.5、ESP32-S3 工具链。

在模板目录执行：idf.py -B build_v5_5_5 build

刷写前确认设备管理器中的串口号，再执行：idf.py -B build_v5_5_5 -p COM21 app-flash
