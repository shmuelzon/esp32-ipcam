#include "microphone.h"
#include "audio_encoder.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <driver/i2s_pdm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sys/socket.h>
#include <string.h>
#include <endian.h>

static const char *TAG = "Microphone";

/* Internal state */
static uint8_t is_capturing = 0;
static SemaphoreHandle_t capture_semaphore;
static i2s_chan_handle_t chan_handle;

static void microphone_capture_task(void *pvParameter)
{
    uint32_t sample_rate = (uint32_t)pvParameter;
    size_t buffer_size = audio_encoder_frame_size(AUDIO_CODEC_OPUS, sample_rate);

    while (1)
    {
        if (xSemaphoreTake(capture_semaphore, portMAX_DELAY) != pdTRUE)
            continue;

        int16_t *pcm_buffer = (int16_t *)malloc(buffer_size * sizeof(int16_t));
        size_t pcm_length;

        if (i2s_channel_read(chan_handle, pcm_buffer, buffer_size * sizeof(int16_t), &pcm_length, 1000) != ESP_OK)
        {
            ESP_LOGE(TAG, "Microphone capture failed");
            xSemaphoreGive(capture_semaphore);
            continue;
        }

        /* XXX TODO go through ipcam.c */
        audio_encoder_encode(pcm_buffer, pcm_length / sizeof(int16_t),
            esp_timer_get_time(), free, pcm_buffer);

        xSemaphoreGive(capture_semaphore);
    }

    i2s_channel_disable(chan_handle);
    i2s_del_channel(chan_handle);
    vTaskDelete(NULL);
}

void microphone_start(void)
{
    if (is_capturing)
        return;

    if (xSemaphoreGive(capture_semaphore) != pdTRUE )
        ESP_LOGE(TAG, "Failed starting microphone");

    is_capturing = 1;
    ESP_LOGI(TAG, "Started microphone capture");
}

void microphone_stop(void)
{
    if (!is_capturing)
        return;

    if (xSemaphoreTake(capture_semaphore, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGE(TAG, "Failed stopping microphone");

    is_capturing = 0;
    ESP_LOGI(TAG, "Stopped microphone capture");
}

int microphone_initialize(int clk, int din, uint32_t sample_rate)
{
    ESP_LOGD(TAG, "Initializing microphone");

    if (!(capture_semaphore = xSemaphoreCreateBinary()))
    {
        ESP_LOGE(TAG, "Failed creating semaphore");
        return -1;
    }

    if (clk == -1 || din == -1)
    {
        ESP_LOGI(TAG, "Microphone capture disabled");
        return 0;
    }

    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_config, NULL, &chan_handle));

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = clk,
            .din = din,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(chan_handle, &pdm_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(chan_handle));

    if (xTaskCreatePinnedToCore(microphone_capture_task,
        "microphone_capture_task", 4096, (void *)sample_rate, 5, NULL, 1) !=
        pdPASS)
    {
        ESP_LOGI(TAG, "Failed starting capture task");
        return -1;
    }
    return 0;
}
