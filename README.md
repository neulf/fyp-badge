# LVGL + Speaker + Drivers Template

This project is a combined ESP32-S3 application template for the Waveshare 1.85B board.
It is based on the comprehensive example and is configured for ESP-IDF v5.5.5.

完整的目录、模块边界和函数职责见 [docs/PROJECT_GUIDE.md](docs/PROJECT_GUIDE.md)。

## Included modules

- LVGL display and CST816S touch input through the board support package.
- ES8311 speaker output through the board I2S and codec driver.
- Optional microphone and wake-word pipeline under `main/wake_word_det`.
- QMI8658 accelerometer and gyroscope under `components/waveshare__qmi8658`.
- PCF85063A real-time clock under `components/waveshare__pcf85063a`.
- BQ27220 battery monitor through the managed ESP-IDF component.
- SD card storage and MP3 playback under `main/app_music`.
- Message and settings UI under `main/app_msg`.
- Speech-recognition test controls and results under `main/driver_test`.
- BLE peripheral phone-control service under `main/ble_control`.

## Module boundaries

`main/main.c` performs application startup and composes the feature modules.
Hardware-specific board initialization is provided by
`components/waveshare__esp32_s3_touch_lcd_1_85B`.
Speaker control and audio playback wrappers are in
`components/bsp_extra`.

Add new hardware drivers as separate components under `components/<vendor>__<device>`.
Keep UI code in a `main/app_*` module and expose a small public header for the
application layer.

## Build in VS Code

Open this directory as the workspace. The included `.vscode/settings.json` selects
ESP-IDF v5.5.5 and uses `build_v5_5_5` as the build directory.

```powershell
. 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'
idf.py build
```

The generated firmware is `build_v5_5_5/lvgl_speaker_drivers_template.bin`.

## Voice and BLE tests

Open Settings and select `Microphone test` or `Bluetooth test`.
The two microphone bars show logarithmic RMS sound levels independently of
speech recognition. Any speech or clapping should move them. These percentages
are a display scale, not a calibrated sound-pressure measurement.
Recognition requires the wake word `Hi ESP`, followed by a configured English
backlight command; Chinese dictation is not configured.
Use `Stop microphone test` / `Start microphone test` to pause/resume at safe
task boundaries. Audio callbacks publish snapshots for the LVGL timer.

For hardware regression, enable `Template diagnostics > Run microphone
stop/start regression on boot` in menuconfig. It exercises the actual UI button
for ten cycles and checks PCM frame progress, with `mic_selftest` serial results.
Keep this option disabled in normal firmware.

The BLE peripheral advertises as `LVGL-DRIVER-TEST`. Use a BLE scanner app such
as nRF Connect, connect to the device, open the custom service, and write ASCII
commands to its control characteristic. Supported commands are `PING`, `PLAY`,
`PAUSE`, `STOP`, and `VOL:0` through `VOL:100`. `PLAY` starts the first MP3 found
under `/sdcard/music`.

The BLE service also exposes image upload characteristic UUID 219a143e-701b-4d91-a481-9d5220100003. Uploads require a mounted SD card and are saved under /sdcard/images. Send binary packets in order: 0x01 plus UTF-8 metadata filename,size,crc32hex; 0x02 plus a 4-byte little-endian offset and image bytes; then 0x03 to finish. The device accepts JPG, JPEG, PNG, and BMP names up to 4 MiB, writes a temporary .part file, and renames it only after size and CRC32 checks pass. The control characteristic read value reports IMAGE:READY, IMAGE:DONE, or the failure reason. nRF Connect can send these packets as hexadecimal Write Without Response operations.

## Board-specific audio note

The BSP already maps the board's I2S pins and ES8311 codec. Use
`bsp_audio_codec_speaker_init()` for the speaker device, then configure the
sample format with `esp_codec_dev_open()` before writing PCM data. The existing
`bsp_board_extra.c` wrapper also provides playback, pause, resume, stop, and
volume functions for application modules.
