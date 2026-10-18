/**
 * @file wake_word_drv.c
 * @brief ES7210 双麦克风采集、ESP-SR AFE/唤醒词和 MultiNet 命令识别。
 *
 * feed_Task 只负责采样和送入 AFE；detect_Task 只负责识别。两个任务通过
 * 事件位协作暂停，避免 UI 线程强制挂起持有音频资源的任务。
 */
#include "wake_word_drv.h"

#include "bsp/esp-bsp.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "bsp_board_extra.h"
#include "app_music/lvgl_music.h"
#include "driver_test/driver_test_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <math.h>
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_process_sdkconfig.h"

#include "model_path.h"
#include "string.h"

int wakeup_flag = 0;
static const esp_afe_sr_iface_t *afe_handle = NULL;
static atomic_bool task_flag;
static esp_codec_dev_handle_t record_dev = NULL;
srmodel_list_t *models = NULL;
int s_backlight = 0;
bool play_status = 0;

static TaskHandle_t feed_task_handle = NULL;
static TaskHandle_t det_task_handle = NULL;
static EventGroupHandle_t s_first_feed_group;
static volatile bool s_first_feed_notified;
static atomic_bool s_enabled;
static atomic_bool s_ready;
static atomic_uint s_frame_count;
static wake_word_state_cb_t s_state_callback;
static wake_word_result_cb_t s_result_callback;
static wake_word_level_cb_t s_level_callback;

#define ADC_I2S_CHANNEL 4
#define VOICE_ENABLED BIT1

/* RMS uses a logarithmic scale so normal speech remains visible below full scale. */
static uint8_t microphone_level(uint64_t square_sum, int samples)
{
    if (!samples || !square_sum) return 0;
    float rms = sqrtf((float)square_sum / samples);
    float percent = (20.0f * log10f(rms / 32768.0f) + 60.0f) * 100.0f / 60.0f;
    return (uint8_t)fminf(100.0f, fmaxf(0.0f, percent));
}

static esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len)
{
    esp_err_t ret = ESP_OK;
    int audio_chunksize = buffer_len / (sizeof(int16_t) * ADC_I2S_CHANNEL);

    ret = esp_codec_dev_read(record_dev, (void *)buffer, buffer_len);
    if (!is_get_raw_channel) {
        for (int i = 0; i < audio_chunksize; i++) {
            int16_t ref = buffer[4 * i + 0];
            buffer[3 * i + 0] = buffer[4 * i + 1];
            buffer[3 * i + 1] = buffer[4 * i + 3];
            buffer[3 * i + 2] = ref;
        }
    }

    return ret;
}
static int bsp_get_feed_channel(void)
{
    return ADC_I2S_CHANNEL;
}

static char* bsp_get_input_format(void)
{
    return "RMNM";
}

void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int nch = afe_handle->get_feed_channel_num(afe_data);
    int feed_channel = bsp_get_feed_channel();
    assert(nch==feed_channel);
    int16_t *i2s_buff = malloc(audio_chunksize * sizeof(int16_t) * feed_channel);
    assert(i2s_buff);

    while (task_flag) {
        /* Pause only at a task boundary, after codec, AFE and UI locks are released. */
        if (!(xEventGroupWaitBits(s_first_feed_group, VOICE_ENABLED, pdFALSE,
                                 pdTRUE, pdMS_TO_TICKS(100)) & VOICE_ENABLED)) continue;
        esp_err_t read_err = bsp_get_feed_data(true, i2s_buff,
                                                audio_chunksize * sizeof(int16_t) * feed_channel);
        if (read_err != ESP_OK) {
            ESP_LOGW("voice_input", "Microphone read failed: %s", esp_err_to_name(read_err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        atomic_fetch_add(&s_frame_count, 1);

        static uint8_t level_update_divider;
        if ((++level_update_divider % 4) == 0) {
            uint64_t mic1_sum = 0;
            uint64_t mic2_sum = 0;
            uint32_t peak1 = 0, peak2 = 0;
            for (int i = 0; i < audio_chunksize; i++) {
                int32_t mic1 = i2s_buff[4 * i + 1];
                int32_t mic2 = i2s_buff[4 * i + 3];
                mic1_sum += (int64_t)mic1 * mic1;
                mic2_sum += (int64_t)mic2 * mic2;
                uint32_t abs1 = mic1 < 0 ? -mic1 : mic1;
                uint32_t abs2 = mic2 < 0 ? -mic2 : mic2;
                if (abs1 > peak1) peak1 = abs1;
                if (abs2 > peak2) peak2 = abs2;
            }
            uint8_t mic1_level = microphone_level(mic1_sum, audio_chunksize);
            uint8_t mic2_level = microphone_level(mic2_sum, audio_chunksize);
            if (s_level_callback && atomic_load(&s_enabled))
                s_level_callback(mic1_level, mic2_level);
            static TickType_t last_log;
            if (!last_log || xTaskGetTickCount() - last_log >= pdMS_TO_TICKS(10000)) {
                ESP_LOGI("voice_input", "PCM mic1=%u%% peak=%lu, mic2=%u%% peak=%lu; DMA largest=%u",
                         mic1_level, (unsigned long)peak1, mic2_level, (unsigned long)peak2,
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
                last_log = xTaskGetTickCount();
            }
        }

        afe_handle->feed(afe_data, i2s_buff);
        if (s_first_feed_group && !s_first_feed_notified) {
            s_first_feed_notified = true;
            xEventGroupSetBits(s_first_feed_group, BIT0);
        }
    }
    if (i2s_buff) {
        free(i2s_buff);
        i2s_buff = NULL;
    }
    feed_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

void detect_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    printf("multinet:%s\n", mn_name);
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 6000);
    int mu_chunksize = multinet->get_samp_chunksize(model_data);
    esp_mn_commands_update_from_sdkconfig(multinet, model_data); // Add speech commands from sdkconfig
    assert(mu_chunksize == afe_chunksize);
    //print active speech commands
    multinet->print_active_speech_commands(model_data);

    printf("------------detect start------------\n");
    atomic_store(&s_ready, true);
    if (s_state_callback) s_state_callback("Microphone ready", atomic_load(&s_enabled));
    while (task_flag) {
        if (!(xEventGroupWaitBits(s_first_feed_group, VOICE_ENABLED, pdFALSE,
                                 pdTRUE, pdMS_TO_TICKS(100)) & VOICE_ENABLED)) {
            if (wakeup_flag) {
                wakeup_flag = 0;
                multinet->clean(model_data);
                afe_handle->enable_wakenet(afe_data);
                bsp_display_brightness_set(s_backlight);
                if (play_status) {
                    Audio_Resume_Play();
                    bsp_display_lock(-1);
                    ui_set_playing(true);
                    bsp_display_unlock();
                    play_status = false;
                }
            }
            continue;
        }
        afe_fetch_result_t* res = afe_handle->fetch(afe_data); 
        if (!atomic_load(&s_enabled)) continue;
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGW("voice_input", "AFE input is not ready; waiting for microphone frames");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            printf("WAKEWORD DETECTED\n");
            if (s_result_callback) s_result_callback("Wake word detected; listening for a command");
	        multinet->clean(model_data);
        }

        if (res->raw_data_channels == 1 && res->wakeup_state == WAKENET_DETECTED) {
            wakeup_flag = 1;
        } else if (res->raw_data_channels > 1 && res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            // For a multi-channel AFE, it is necessary to wait for the channel to be verified.
            printf("AFE_FETCH_CHANNEL_VERIFIED, channel index: %d\n", res->trigger_channel_id);
            s_backlight = bsp_display_brightness_get();
            bsp_display_brightness_set(s_backlight-30);

            esp_asp_state_t status = Audio_Get_Current_State();
            if (status == ESP_ASP_STATE_RUNNING) {
                Audio_Pause_Play();
                bsp_display_lock(-1);
                ui_set_playing(false);
                bsp_display_unlock();

                play_status = true;
            }

            wakeup_flag = 1;
        }

        if (wakeup_flag == 1) {
            esp_mn_state_t mn_state = multinet->detect(model_data, res->data);

            if (mn_state == ESP_MN_STATE_DETECTING) {
                continue;
            }

            if (mn_state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *mn_result = multinet->get_results(model_data);
                for (int i = 0; i < mn_result->num; i++) {
                    printf("TOP %d, command_id: %d, phrase_id: %d, string:%s prob: %f\n", 
                    i+1, mn_result->command_id[i], mn_result->phrase_id[i], mn_result->string, mn_result->prob[i]);
                    if (s_result_callback) s_result_callback(mn_result->string);
                }
                printf("\n-----------listening-----------\n");
            }

            if (mn_state == ESP_MN_STATE_TIMEOUT) {
                esp_mn_results_t *mn_result = multinet->get_results(model_data);
                printf("timeout, string:%s\n", mn_result->string);
                afe_handle->enable_wakenet(afe_data);
                wakeup_flag = 0;
                printf("\n-----------awaits to be waken up-----------\n");

                esp_asp_state_t status = Audio_Get_Current_State();
                if (play_status == true) {
                    Audio_Resume_Play();
                    bsp_display_lock(-1);
                    ui_set_playing(true);
                    bsp_display_unlock();
                }

                bsp_display_brightness_set(s_backlight);
                play_status = false;

                continue;
            }
        }
    }
    if (model_data) {
        multinet->destroy(model_data);
        model_data = NULL;
    }
    printf("detect exit\n");
    atomic_store(&s_ready, false);
    det_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

void wake_word_drv_init(void)
{

    record_dev =  bsp_audio_codec_microphone_init();

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 32,
        .channel = 2,
        .sample_rate = 16000,
    };
    if (!record_dev || esp_codec_dev_open(record_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE("voice_input", "Microphone codec could not be opened");
        return;
    }
    esp_codec_dev_set_in_gain(record_dev, 30.0);

    models = esp_srmodel_init("model"); // partition label defined in partitions.csv
    afe_config_t *afe_config = afe_config_init(bsp_get_input_format(), models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    task_flag = 1;

    s_first_feed_group = xEventGroupCreate();
    if (!s_first_feed_group) {
        ESP_LOGE("voice_input", "Could not create microphone startup semaphore");
        task_flag = 0;
        return;
    }

    atomic_store(&s_enabled, true);
    xEventGroupSetBits(s_first_feed_group, VOICE_ENABLED);
    if (xTaskCreatePinnedToCoreWithCaps(&feed_Task, "feed", 8 * 1024, (void*)afe_data,
                                5, &feed_task_handle, 1,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE("voice_input", "Could not create microphone feed task");
        task_flag = 0;
        return;
    }

    if ((xEventGroupWaitBits(s_first_feed_group, BIT0, pdFALSE, pdTRUE,
                             pdMS_TO_TICKS(1000)) & BIT0) == 0) {
        ESP_LOGW("voice_input", "No microphone frame received during startup");
    }

    if (xTaskCreatePinnedToCoreWithCaps(&detect_Task, "detect", 8 * 1024,
                                       (void*)afe_data, 6, &det_task_handle, 1,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE("voice_input", "Could not create speech detection task");
        task_flag = 0;
        atomic_store(&s_enabled, false);
    }
}

void pause_wake_word_task()
{
    atomic_store(&s_enabled, false);
    if (s_first_feed_group) xEventGroupClearBits(s_first_feed_group, VOICE_ENABLED);
    ESP_LOGI("voice_input", "Microphone test stopped cooperatively");
}

void resume_wake_word_task()
{
    if (!atomic_load(&s_ready)) return;
    atomic_store(&s_enabled, true);
    xEventGroupSetBits(s_first_feed_group, VOICE_ENABLED);
    ESP_LOGI("voice_input", "Microphone test started");
}

bool wake_word_drv_is_ready(void) { return atomic_load(&s_ready); }
bool wake_word_drv_is_active(void) { return atomic_load(&s_ready) && atomic_load(&s_enabled); }
uint32_t wake_word_drv_frame_count(void) { return atomic_load(&s_frame_count); }

void wake_word_drv_bind_ui(wake_word_state_cb_t state_cb,
                           wake_word_result_cb_t result_cb,
                           wake_word_level_cb_t level_cb)
{
    s_state_callback = state_cb;
    s_result_callback = result_cb;
    s_level_callback = level_cb;
}
