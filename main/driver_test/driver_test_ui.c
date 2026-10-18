/**
 * @file driver_test_ui.c
 * @brief 麦克风和 BLE 调试页面。
 *
 * 音频/BLE 工作线程只更新快照；voice_ui_timer_cb 在 LVGL 线程更新控件，
 * 这样可以避免从非 LVGL 线程直接操作对象。
 */
#include "driver_test_ui.h"

#include "ui_theme.h"
#include "microphone_selftest.h"
#include "wake_word_det/wake_word_drv.h"
#include "ble_control/ble_control.h"

#include <string.h>
#include <stdio.h>

static lv_obj_t *s_voice_state_label;
static lv_obj_t *s_voice_result_label;
static lv_obj_t *s_voice_button_label;
static lv_obj_t *s_mic1_label;
static lv_obj_t *s_mic2_label;
static lv_obj_t *s_mic1_bar;
static lv_obj_t *s_mic2_bar;
static lv_obj_t *s_ble_state_label;
static lv_obj_t *s_ble_command_label;
static lv_obj_t *s_ble_button_label;
static lv_obj_t *s_ble_scan_label;
static lv_obj_t *s_ble_scan_button_label;
static lv_obj_t *s_mic_screen;
static lv_obj_t *s_ble_screen;
static lv_obj_t *s_return_screen;
static bool s_voice_active;
static bool s_ble_advertising;
static bool s_ble_scanning;
static uint16_t s_ble_scan_count;

/* Audio callbacks publish snapshots; only the LVGL timer touches microphone widgets. */
static portMUX_TYPE s_voice_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint8_t mic1, mic2;
    bool levels_dirty, state_dirty, result_dirty, active;
    char state[96];
    char result[192];
} s_voice_snapshot;

static void voice_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    typeof(s_voice_snapshot) snapshot;
    portENTER_CRITICAL(&s_voice_snapshot_lock);
    snapshot = s_voice_snapshot;
    s_voice_snapshot.levels_dirty = false;
    s_voice_snapshot.state_dirty = false;
    s_voice_snapshot.result_dirty = false;
    portEXIT_CRITICAL(&s_voice_snapshot_lock);

    bool ready = wake_word_drv_is_ready();
    bool active = wake_word_drv_is_active();
    lv_obj_t *button = lv_obj_get_parent(s_voice_button_label);
    if (ready) lv_obj_remove_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
    if (active) lv_obj_add_state(button, LV_STATE_CHECKED);
    else lv_obj_remove_state(button, LV_STATE_CHECKED);
    if (s_voice_active != active || snapshot.state_dirty) {
        s_voice_active = active;
        lv_label_set_text(s_voice_button_label, active ? "Stop microphone test" : "Start microphone test");
        const char *state = !ready ? "Microphone driver unavailable" :
                           active ? "Listening / sound level" : "Stopped";
        if (ready && snapshot.state_dirty && snapshot.active == active) state = snapshot.state;
        lv_label_set_text(s_voice_state_label, state);
        lv_obj_set_style_text_color(s_voice_state_label,
                                   active ? TEST_UI_SUCCESS : TEST_UI_TEXT_DIM, LV_PART_MAIN);
    }
    if (snapshot.levels_dirty || !active) {
        uint8_t mic1 = active ? snapshot.mic1 : 0;
        uint8_t mic2 = active ? snapshot.mic2 : 0;
        lv_bar_set_value(s_mic1_bar, mic1, LV_ANIM_OFF);
        lv_bar_set_value(s_mic2_bar, mic2, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_mic1_label, "MIC 1  %u%%", (unsigned)mic1);
        lv_label_set_text_fmt(s_mic2_label, "MIC 2  %u%%", (unsigned)mic2);
    }
    if (snapshot.result_dirty && active) lv_label_set_text(s_voice_result_label, snapshot.result);
}

static void back_to_settings_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED && s_return_screen) {
        lv_screen_load(s_return_screen);
    }
}

static lv_obj_t *make_action_button(lv_obj_t *parent, const char *text,
                                    lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, lv_pct(100), TEST_UI_BUTTON_H);
    lv_obj_set_style_bg_color(button, TEST_UI_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(button, TEST_UI_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, TEST_UI_ACCENT_DARK, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, TEST_UI_ACCENT_DARK, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button, TEST_UI_BORDER, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(button, TEST_UI_TEXT_DIM, LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(button, 6);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *create_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *section = lv_obj_create(parent);
    lv_obj_set_width(section, lv_pct(100));
    lv_obj_set_height(section, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(section, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(section, TEST_UI_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(section, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(section, TEST_UI_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(section, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(section, TEST_UI_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_all(section, TEST_UI_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(section, TEST_UI_GAP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(section, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *heading = lv_label_create(section);
    lv_label_set_text(heading, title);
    lv_obj_set_style_text_color(heading, TEST_UI_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_18, LV_PART_MAIN);
    return section;
}

static lv_obj_t *create_status_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_color(label, TEST_UI_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    return label;
}

static void voice_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    if (wake_word_drv_is_active()) {
        pause_wake_word_task();
        s_voice_active = false;
        lv_label_set_text(s_voice_button_label, "Start microphone test");
        driver_test_ui_set_voice_state("Stopped", false);
    } else {
        resume_wake_word_task();
        s_voice_active = wake_word_drv_is_active();
        lv_label_set_text(s_voice_button_label, "Stop microphone test");
        driver_test_ui_set_voice_state(s_voice_active ? "Listening for wake word" :
                                                       "Microphone driver unavailable", s_voice_active);
        driver_test_ui_set_voice_result("Level bars test both microphones directly. Commands need 'Hi ESP' followed by an English command.");
    }
}

static void ble_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    if (s_ble_advertising) {
        ble_control_set_advertising(false);
        s_ble_advertising = false;
        lv_label_set_text(s_ble_button_label, "Start BLE advertising");
        driver_test_ui_set_ble_state("Advertising stopped", false);
    } else if (ble_control_set_advertising(true) == ESP_OK) {
        s_ble_advertising = true;
        lv_label_set_text(s_ble_button_label, "Stop BLE advertising");
        driver_test_ui_set_ble_state("Advertising: LVGL-DRIVER-TEST", false);
    } else {
        driver_test_ui_set_ble_state("BLE start failed; check serial log", false);
    }
}

static void ble_scan_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    if (s_ble_scanning) {
        if (ble_control_scan_stop() == ESP_OK) {
            lv_label_set_text(s_ble_scan_button_label, "Stopping scan...");
        }
        return;
    }

    s_ble_scan_count = 0;
    lv_label_set_text(s_ble_scan_label, "Searching nearby BLE devices...");
    if (ble_control_scan_start() == ESP_OK) {
        s_ble_scanning = true;
        lv_label_set_text(s_ble_scan_button_label, "Stop BLE scan");
    } else {
        lv_label_set_text(s_ble_scan_label, "Scan unavailable; check BLE status");
    }
}

void driver_test_ui_init(lv_obj_t *return_screen)
{
    s_return_screen = return_screen;
    s_mic_screen = lv_obj_create(NULL);
    s_ble_screen = lv_obj_create(NULL);

    lv_obj_t *mic_content = lv_obj_create(s_mic_screen);
    lv_obj_set_size(mic_content, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(mic_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mic_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(s_mic_screen, TEST_UI_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_mic_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mic_content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(mic_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mic_content, TEST_UI_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(mic_content, TEST_UI_GAP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(mic_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(mic_content, LV_SCROLLBAR_MODE_AUTO);

    make_action_button(mic_content, "Back to settings", back_to_settings_event_cb);
    lv_obj_t *mic_title = lv_label_create(mic_content);
    lv_label_set_text(mic_title, "Microphone test");
    lv_obj_set_style_text_color(mic_title, TEST_UI_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(mic_title, &lv_font_montserrat_22, LV_PART_MAIN);

    lv_obj_t *voice_section = create_section(mic_content, "Dual microphone input");
    s_voice_state_label = create_status_label(voice_section, "Voice recognition stopped");
    s_mic1_label = create_status_label(voice_section, "MIC 1  0%");
    s_mic1_bar = lv_bar_create(voice_section);
    lv_obj_set_width(s_mic1_bar, lv_pct(100));
    lv_obj_set_height(s_mic1_bar, 14);
    lv_bar_set_range(s_mic1_bar, 0, 100);
    lv_bar_set_value(s_mic1_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_mic1_bar, TEST_UI_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mic1_bar, TEST_UI_ACCENT, LV_PART_INDICATOR);
    s_mic2_label = create_status_label(voice_section, "MIC 2  0%");
    s_mic2_bar = lv_bar_create(voice_section);
    lv_obj_set_width(s_mic2_bar, lv_pct(100));
    lv_obj_set_height(s_mic2_bar, 14);
    lv_bar_set_range(s_mic2_bar, 0, 100);
    lv_bar_set_value(s_mic2_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_mic2_bar, TEST_UI_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mic2_bar, TEST_UI_SUCCESS, LV_PART_INDICATOR);
    s_voice_result_label = create_status_label(
        voice_section, "Speak to move the MIC bars. For commands, say 'Hi ESP', then an English command.");
    lv_label_set_long_mode(s_voice_result_label, LV_LABEL_LONG_WRAP);
    s_voice_active = wake_word_drv_is_active();
    driver_test_ui_set_voice_state(s_voice_active ? "Listening for wake word" :
                                                   "Waiting for microphone driver", s_voice_active);
    lv_obj_t *voice_button = make_action_button(voice_section,
                                                s_voice_active ? "Stop microphone test" :
                                                                 "Start microphone test",
                                                voice_button_event_cb);
    s_voice_button_label = lv_obj_get_child(voice_button, 0);

    lv_obj_t *ble_content = lv_obj_create(s_ble_screen);
    lv_obj_set_size(ble_content, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(ble_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ble_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_color(s_ble_screen, TEST_UI_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ble_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ble_content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ble_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ble_content, TEST_UI_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ble_content, TEST_UI_GAP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(ble_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ble_content, LV_SCROLLBAR_MODE_AUTO);

    make_action_button(ble_content, "Back to settings", back_to_settings_event_cb);
    lv_obj_t *ble_title = lv_label_create(ble_content);
    lv_label_set_text(ble_title, "Bluetooth debug");
    lv_obj_set_style_text_color(ble_title, TEST_UI_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(ble_title, &lv_font_montserrat_22, LV_PART_MAIN);

    lv_obj_t *ble_section = create_section(ble_content, "BLE phone control and scan");
    s_ble_state_label = create_status_label(ble_section,
                                            "Phone control + SD image upload: LVGL-DRIVER-TEST");
    s_ble_scan_label = create_status_label(ble_section, "Nearby devices: none");
    lv_obj_t *scan_button = make_action_button(ble_section, "Scan nearby BLE devices",
                                               ble_scan_button_event_cb);
    s_ble_scan_button_label = lv_obj_get_child(scan_button, 0);
    s_ble_command_label = create_status_label(ble_section, "Last command: none");
    s_ble_advertising = true;
    lv_obj_t *ble_button = make_action_button(ble_section, "Stop BLE advertising",
                                              ble_button_event_cb);
    s_ble_button_label = lv_obj_get_child(ble_button, 0);

    ble_control_bind_ui(driver_test_ui_set_ble_state,
                        driver_test_ui_set_ble_last_command);
    ble_control_bind_scan_ui(driver_test_ui_set_ble_scan_result);
    wake_word_drv_bind_ui(driver_test_ui_set_voice_state,
                          driver_test_ui_set_voice_result,
                          driver_test_ui_set_mic_levels);
    lv_timer_create(voice_ui_timer_cb, 100, NULL);
    microphone_selftest_start(voice_button);
}

void driver_test_ui_open_microphone(void)
{
    if (s_mic_screen) {
        lv_screen_load(s_mic_screen);
    }
}

void driver_test_ui_open_bluetooth(void)
{
    if (s_ble_screen) {
        lv_screen_load(s_ble_screen);
    }
}

void driver_test_ui_set_mic_levels(uint8_t mic1_percent, uint8_t mic2_percent)
{
    portENTER_CRITICAL(&s_voice_snapshot_lock);
    s_voice_snapshot.mic1 = mic1_percent;
    s_voice_snapshot.mic2 = mic2_percent;
    s_voice_snapshot.levels_dirty = true;
    portEXIT_CRITICAL(&s_voice_snapshot_lock);
}

void driver_test_ui_set_voice_state(const char *text, bool active)
{
    portENTER_CRITICAL(&s_voice_snapshot_lock);
    snprintf(s_voice_snapshot.state, sizeof(s_voice_snapshot.state), "%s", text);
    s_voice_snapshot.active = active;
    s_voice_snapshot.state_dirty = true;
    portEXIT_CRITICAL(&s_voice_snapshot_lock);
}

void driver_test_ui_set_voice_result(const char *text)
{
    portENTER_CRITICAL(&s_voice_snapshot_lock);
    snprintf(s_voice_snapshot.result, sizeof(s_voice_snapshot.result), "%s", text);
    s_voice_snapshot.result_dirty = true;
    portEXIT_CRITICAL(&s_voice_snapshot_lock);
}

void driver_test_ui_set_ble_state(const char *text, bool connected)
{
    if (!s_ble_state_label) {
        return;
    }
    bsp_display_lock(-1);
    lv_label_set_text(s_ble_state_label, text);
    if (strstr(text, "scan complete")) {
        s_ble_scanning = false;
        lv_label_set_text(s_ble_scan_button_label, "Scan nearby BLE devices");
    }
    lv_obj_set_style_text_color(s_ble_state_label,
                                connected ? TEST_UI_SUCCESS : TEST_UI_TEXT_DIM,
                                LV_PART_MAIN);
    bsp_display_unlock();
}

void driver_test_ui_set_ble_scan_result(const char *name, const char *address, int8_t rssi)
{
    if (!s_ble_scan_label) {
        return;
    }
    bsp_display_lock(-1);
    s_ble_scan_count++;
    lv_label_set_text_fmt(s_ble_scan_label, "%u found | %.24s | %s | %d dBm",
                          (unsigned)s_ble_scan_count, name, address, (int)rssi);
    bsp_display_unlock();
}

void driver_test_ui_set_ble_last_command(const char *text)
{
    if (s_ble_command_label) {
        bsp_display_lock(-1);
        lv_label_set_text(s_ble_command_label, text);
        bsp_display_unlock();
    }
}
