/**
 * @file ble_control.c
 * @brief NimBLE 控制服务、附近设备扫描和 SD 卡图片分包上传实现。
 *
 * 控制特征值处理短文本命令；图片特征值处理 BEGIN、CHUNK、END 二进制包。
 * 上传先写入 .part 文件，长度和 CRC32 校验通过后才改名为正式文件。
 */
#include "ble_control.h"

#include "bsp_board_extra.h"
#include "bsp/esp32_s3_touch_lcd_1_85B.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "ble_control";
static const char *DEVICE_NAME = "LVGL-DRIVER-TEST";
static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0x21, 0x9a, 0x14, 0x3e, 0x70, 0x1b, 0x4d, 0x91,
    0xa4, 0x81, 0x9d, 0x52, 0x20, 0x10, 0x00, 0x01);
static const ble_uuid128_t s_control_uuid = BLE_UUID128_INIT(
    0x21, 0x9a, 0x14, 0x3e, 0x70, 0x1b, 0x4d, 0x91,
    0xa4, 0x81, 0x9d, 0x52, 0x20, 0x10, 0x00, 0x02);
static const ble_uuid128_t s_image_upload_uuid = BLE_UUID128_INIT(
    0x21, 0x9a, 0x14, 0x3e, 0x70, 0x1b, 0x4d, 0x91,
    0xa4, 0x81, 0x9d, 0x52, 0x20, 0x10, 0x00, 0x03);

static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_control_value_handle;
static uint16_t s_image_upload_value_handle;
static bool s_stack_synced;
static bool s_initialized;
static bool s_advertising_requested;
static bool s_scan_active;
static char s_control_value[64] = "READY";
static char s_default_track[160];
static ble_control_state_cb_t s_state_callback;
static ble_control_command_cb_t s_command_callback;
static ble_control_scan_cb_t s_scan_callback;

#define IMAGE_DIR "/sdcard/images"
#define IMAGE_MAX_SIZE (4U * 1024U * 1024U)
#define IMAGE_PACKET_DATA 1
#define IMAGE_PACKET_CHUNK 2
#define IMAGE_PACKET_END 3

static FILE *s_image_file;
static char s_image_name[65];
static char s_image_part_path[96];
static char s_image_final_path[96];
static uint32_t s_image_expected_size;
static uint32_t s_image_received_size;
static uint32_t s_image_expected_crc;
static uint32_t s_image_crc;

static void image_upload_abort(void)
{
    if (s_image_file) {
        fclose(s_image_file);
        s_image_file = NULL;
    }
    if (s_image_part_path[0]) unlink(s_image_part_path);
    s_image_name[0] = '\0';
    s_image_part_path[0] = '\0';
    s_image_final_path[0] = '\0';
    s_image_expected_size = 0;
    s_image_received_size = 0;
    s_image_expected_crc = 0;
    s_image_crc = 0;
}

static bool image_name_is_safe(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length > 64 || name[0] == '.' ||
        (strstr(name, ".jpg") == NULL && strstr(name, ".JPG") == NULL &&
         strstr(name, ".jpeg") == NULL && strstr(name, ".JPEG") == NULL &&
         strstr(name, ".png") == NULL && strstr(name, ".PNG") == NULL &&
         strstr(name, ".bmp") == NULL && strstr(name, ".BMP") == NULL)) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '.' ||
              name[i] == '_' || name[i] == '-')) return false;
    }
    return strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static void image_upload_status(const char *status)
{
    snprintf(s_control_value, sizeof(s_control_value), "IMAGE:%s", status);
    ESP_LOGI(TAG, "Image upload: %s", status);
}

static int image_upload_begin(const uint8_t *payload, size_t length)
{
    if (bsp_sdcard_get_handle() == NULL) {
        image_upload_status("SD_NOT_MOUNTED");
        return BLE_ATT_ERR_UNLIKELY;
    }

    char metadata[160];
    if (length == 0 || length >= sizeof(metadata)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    memcpy(metadata, payload, length);
    metadata[length] = '\0';

    char name[65];
    unsigned long size = 0, crc = 0;
    if (sscanf(metadata, "%64[^,],%lu,%lx", name, &size, &crc) != 3 ||
        !image_name_is_safe(name) || size == 0 || size > IMAGE_MAX_SIZE) {
        image_upload_status("BAD_BEGIN");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    image_upload_abort();
    if (mkdir(IMAGE_DIR, 0775) != 0 && errno != EEXIST) {
        image_upload_status("MKDIR_FAILED");
        return BLE_ATT_ERR_UNLIKELY;
    }
    snprintf(s_image_name, sizeof(s_image_name), "%s", name);
    snprintf(s_image_part_path, sizeof(s_image_part_path), "%s/.%s.part", IMAGE_DIR, name);
    snprintf(s_image_final_path, sizeof(s_image_final_path), "%s/%s", IMAGE_DIR, name);
    s_image_file = fopen(s_image_part_path, "wb");
    if (!s_image_file) {
        image_upload_abort();
        image_upload_status("OPEN_FAILED");
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_image_expected_size = (uint32_t)size;
    s_image_expected_crc = (uint32_t)crc;
    s_image_crc = 0;
    image_upload_status("READY");
    return 0;
}

static int image_upload_chunk(const uint8_t *payload, size_t length)
{
    if (!s_image_file || length < sizeof(uint32_t)) return BLE_ATT_ERR_UNLIKELY;
    uint32_t offset = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
                      ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
    const uint8_t *data = payload + sizeof(uint32_t);
    size_t data_length = length - sizeof(uint32_t);
    if (offset != s_image_received_size || data_length == 0 ||
        data_length > s_image_expected_size - s_image_received_size) {
        image_upload_status("BAD_OFFSET");
        return BLE_ATT_ERR_INVALID_OFFSET;
    }
    if (fwrite(data, 1, data_length, s_image_file) != data_length) {
        image_upload_abort();
        image_upload_status("WRITE_FAILED");
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_image_crc = esp_rom_crc32_le(s_image_crc, data, data_length);
    s_image_received_size += (uint32_t)data_length;
    return 0;
}

static int image_upload_end(void)
{
    if (!s_image_file || s_image_received_size != s_image_expected_size) {
        image_upload_status("SIZE_MISMATCH");
        image_upload_abort();
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    fflush(s_image_file);
    fclose(s_image_file);
    s_image_file = NULL;
    if (s_image_crc != s_image_expected_crc) {
        image_upload_status("CRC_MISMATCH");
        image_upload_abort();
        return BLE_ATT_ERR_UNLIKELY;
    }
    unlink(s_image_final_path);
    if (rename(s_image_part_path, s_image_final_path) != 0) {
        image_upload_status("RENAME_FAILED");
        image_upload_abort();
        return BLE_ATT_ERR_UNLIKELY;
    }
    image_upload_status("DONE");
    s_image_part_path[0] = '\0';
    return 0;
}

static int image_upload_access_cb(struct ble_gatt_access_ctxt *context)
{
    uint16_t length = OS_MBUF_PKTLEN(context->om);
    uint8_t packet[512];
    uint16_t copied = 0;
    if (length == 0 || length > sizeof(packet) ||
        ble_hs_mbuf_to_flat(context->om, packet, sizeof(packet), &copied) != 0 ||
        copied != length) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (packet[0] == IMAGE_PACKET_DATA) return image_upload_begin(packet + 1, length - 1);
    if (packet[0] == IMAGE_PACKET_CHUNK) return image_upload_chunk(packet + 1, length - 1);
    if (packet[0] == IMAGE_PACKET_END && length == 1) return image_upload_end();
    return BLE_ATT_ERR_INVALID_PDU;
}

static int gatt_access_cb(uint16_t connection_handle, uint16_t attribute_handle,
                          struct ble_gatt_access_ctxt *context, void *arg);
static int gap_event_cb(struct ble_gap_event *event, void *arg);
static int scan_event_cb(struct ble_gap_event *event, void *arg);

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_control_uuid.u,
                .access_cb = gatt_access_cb,
                .val_handle = &s_control_value_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_image_upload_uuid.u,
                .access_cb = gatt_access_cb,
                .val_handle = &s_image_upload_value_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

static void publish_state(const char *text, bool connected)
{
    ESP_LOGI(TAG, "%s", text);
    if (s_state_callback) {
        s_state_callback(text, connected);
    }
}

static void start_advertising(void)
{
    if (!s_stack_synced || !s_advertising_requested || s_scan_active ||
        s_connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    uint8_t address_type;
    int rc = ble_hs_id_infer_auto(0, &address_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not determine BLE address type: %d", rc);
        publish_state("BLE address setup failed", false);
        return;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not set BLE advertisement fields: %d", rc);
        publish_state("BLE advertisement setup failed", false);
        return;
    }

    struct ble_hs_adv_fields response_fields = {0};
    response_fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    response_fields.num_uuids128 = 1;
    response_fields.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not set BLE scan response: %d", rc);
        publish_state("BLE scan response setup failed", false);
        return;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER,
                           &parameters, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Could not start BLE advertisement: %d", rc);
        publish_state("BLE advertising failed", false);
        return;
    }

    publish_state("Advertising: LVGL-DRIVER-TEST", false);
}

static void host_sync_cb(void)
{
    s_stack_synced = true;
    start_advertising();
}

static void host_reset_cb(int reason)
{
    s_stack_synced = false;
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
    publish_state("BLE stack reset", false);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t execute_command(const char *command)
{
    if (strcmp(command, "PING") == 0) {
        return ESP_OK;
    }
    if (strcmp(command, "PLAY") == 0) {
        esp_asp_state_t state = Audio_Get_Current_State();
        if (state == ESP_ASP_STATE_PAUSED) {
            return Audio_Resume_Play() == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
        }
        if (s_default_track[0] == '\0') {
            return ESP_ERR_NOT_FOUND;
        }
        return Audio_Play_Music(s_default_track) == ESP_GMF_ERR_OK
                   ? ESP_OK : ESP_FAIL;
    }
    if (strcmp(command, "PAUSE") == 0) {
        return Audio_Pause_Play() == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
    }
    if (strcmp(command, "STOP") == 0) {
        return Audio_Stop_Play() == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
    }
    if (strncmp(command, "VOL:", 4) == 0) {
        char *end = NULL;
        long volume = strtol(command + 4, &end, 10);
        if (end == command + 4 || *end != '\0' || volume < 0 || volume > 100) {
            return ESP_ERR_INVALID_ARG;
        }
        Volume_Adjustment((uint8_t)volume);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static int gatt_access_cb(uint16_t connection_handle, uint16_t attribute_handle,
                          struct ble_gatt_access_ctxt *context, void *arg)
{
    (void)connection_handle;
    (void)attribute_handle;
    (void)arg;

    if (attribute_handle == s_image_upload_value_handle) {
        if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
        return image_upload_access_cb(context);
    }

    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(context->om, s_control_value,
                              strlen(s_control_value)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t length = OS_MBUF_PKTLEN(context->om);
    if (length == 0 || length >= sizeof(s_control_value)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    char command[sizeof(s_control_value)];
    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(context->om, command, sizeof(command) - 1, &copied);
    if (rc != 0 || copied != length) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    command[copied] = '\0';

    esp_err_t command_result = execute_command(command);
    snprintf(s_control_value, sizeof(s_control_value), "%.30s:%.20s", command,
             esp_err_to_name(command_result));
    ESP_LOGI(TAG, "Phone command: %s (%s)", command,
             esp_err_to_name(command_result));
    if (s_command_callback) {
        s_command_callback(s_control_value);
    }

    return command_result == ESP_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connection_handle = event->connect.conn_handle;
            publish_state("Phone connected", true);
        } else {
            publish_state("Connection failed; advertising", false);
            start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
        publish_state("Phone disconnected", false);
        start_advertising();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        publish_state("Advertising ended", false);
        start_advertising();
        return 0;
    default:
        return 0;
    }
}

static int scan_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_hs_adv_fields fields = {0};
        char name[32] = "(unnamed)";
        char address[18];
        if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                    event->disc.length_data) == 0 &&
            fields.name && fields.name_len > 0) {
            size_t name_len = fields.name_len;
            if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
            memcpy(name, fields.name, name_len);
            name[name_len] = '\0';
        }
        snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X",
                 event->disc.addr.val[5], event->disc.addr.val[4],
                 event->disc.addr.val[3], event->disc.addr.val[2],
                 event->disc.addr.val[1], event->disc.addr.val[0]);
        if (s_scan_callback) {
            s_scan_callback(name, address, event->disc.rssi);
        }
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_scan_active = false;
        publish_state(s_advertising_requested
                          ? "BLE scan complete; phone control advertising resumed"
                          : "BLE scan complete; advertising is off",
                      false);
        start_advertising();
    }
    return 0;
}

esp_err_t ble_control_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_hs_cfg.reset_cb = host_reset_cb;
    ble_hs_cfg.sync_cb = host_sync_cb;

    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT service registration failed: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);
    s_initialized = true;
    return ESP_OK;
}

esp_err_t ble_control_set_advertising(bool enabled)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    s_advertising_requested = enabled;
    if (!enabled) {
        int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "Could not stop BLE advertising: %d", rc);
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (s_stack_synced && !s_scan_active &&
        s_connection_handle == BLE_HS_CONN_HANDLE_NONE) {
        start_advertising();
    }
    return ESP_OK;
}

esp_err_t ble_control_scan_start(void)
{
    if (!s_initialized || !s_stack_synced) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scan_active || s_connection_handle != BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t address_type;
    int rc = ble_hs_id_infer_auto(0, &address_type);
    if (rc != 0) {
        return ESP_FAIL;
    }
    if (s_advertising_requested) {
        rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            return ESP_FAIL;
        }
    }

    const struct ble_gap_disc_params parameters = {
        .itvl = 0x50,
        .window = 0x30,
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,
        .filter_duplicates = 1,
    };
    rc = ble_gap_disc(address_type, 8000, &parameters, scan_event_cb, NULL);
    if (rc != 0) {
        start_advertising();
        return ESP_FAIL;
    }
    s_scan_active = true;
    publish_state("Scanning for nearby BLE devices", false);
    return ESP_OK;
}

esp_err_t ble_control_scan_stop(void)
{
    if (!s_initialized || !s_scan_active) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = ble_gap_disc_cancel();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

void ble_control_bind_ui(ble_control_state_cb_t state_cb,
                         ble_control_command_cb_t command_cb)
{
    s_state_callback = state_cb;
    s_command_callback = command_cb;
}

void ble_control_bind_scan_ui(ble_control_scan_cb_t scan_cb)
{
    s_scan_callback = scan_cb;
}

void ble_control_set_default_track(const char *path)
{
    if (!path || path[0] == '\0') {
        s_default_track[0] = '\0';
        return;
    }
    snprintf(s_default_track, sizeof(s_default_track), "file:/%s", path);
}
