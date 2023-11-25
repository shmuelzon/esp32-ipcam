#include "camera.h"
#include "rtp.h"
#include <esp_camera.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

static const char *TAG = "Camera";

static uint8_t is_capturing = 0;
static SemaphoreHandle_t capture_semaphore;

static void camera_release_fb(void *fb)
{
    esp_camera_fb_return((camera_fb_t *)fb);
}

static void camera_capture_task(void *pvParameter)
{
    int sleep_time = 1000 / (int)pvParameter;
    camera_fb_t *fb;

    while (1)
    {
        if (xSemaphoreTake(capture_semaphore, portMAX_DELAY) != pdTRUE)
            continue;

        if (!(fb = esp_camera_fb_get()))
        {
            ESP_LOGE(TAG, "Camera capture failed");
            xSemaphoreGive(capture_semaphore);
            continue;
        }

        /* XXX TODO Should go through ipcam.c */
        rtp_send_jpeg(fb->width, fb->height, fb->buf, fb->len,
            esp_timer_get_time(), camera_release_fb, fb);

        xSemaphoreGive(capture_semaphore);

        vTaskDelay(pdMS_TO_TICKS(sleep_time));
    };

    vTaskDelete(NULL);
}

static int resolution_to_frame_size(const char *resolution)
{
    struct {
        const char *name;
        int frame_size;
    } *p, resolutions[] = {
        { "96x96", FRAMESIZE_96X96 },
        { "160x120", FRAMESIZE_QQVGA },
        { "176x144", FRAMESIZE_QCIF },
        { "240x176", FRAMESIZE_HQVGA },
        { "240x240", FRAMESIZE_240X240 },
        { "320x240", FRAMESIZE_QVGA },
        { "400x296", FRAMESIZE_CIF },
        { "480x320", FRAMESIZE_HVGA },
        { "640x480", FRAMESIZE_VGA },
        { "800x600", FRAMESIZE_SVGA },
        { "1024x768", FRAMESIZE_XGA },
        { "1280x720", FRAMESIZE_HD },
        { "1280x1024", FRAMESIZE_SXGA },
        { "1600x1200", FRAMESIZE_UXGA },
        { "1920x1080", FRAMESIZE_FHD },
        { "720x1280", FRAMESIZE_P_HD },
        { "864x1536", FRAMESIZE_P_3MP },
        { "2048x1536", FRAMESIZE_QXGA },
        { "2560x1440", FRAMESIZE_QHD },
        { "2560x1600", FRAMESIZE_WQXGA },
        { "1080x1920", FRAMESIZE_P_FHD },
        { "2560x1920", FRAMESIZE_QSXGA },
        { NULL, FRAMESIZE_INVALID },
    };

    for (p = resolutions; p->name; p++)
    {
        if (!strcasecmp(p->name, resolution))
            break;
    }

    return p->frame_size;

}

void camera_start(void)
{
    if (is_capturing)
        return;

    if (xSemaphoreGive(capture_semaphore) != pdTRUE )
        ESP_LOGE(TAG, "Failed starting camera");

    is_capturing = 1;
    ESP_LOGI(TAG, "Started camera capture");
}

void camera_stop(void)
{
    if (!is_capturing)
        return;

    if (xSemaphoreTake(capture_semaphore, pdMS_TO_TICKS(1000)) != pdTRUE)
        ESP_LOGE(TAG, "Failed stopping camera");

    is_capturing = 0;
    ESP_LOGI(TAG, "Stopped camera capture");
}

int camera_initialize(int pwdn, int reset, int xclk, int siod, int sioc, int d7,
    int d6, int d5, int d4, int d3, int d2, int d1, int d0, int vsync, int href,
    int pclk, const char *resolution, int fps, uint8_t vflip, uint8_t hmirror,
    int quality)
{
    sensor_t *s;
    camera_config_t camera_config = {
        .xclk_freq_hz = 10000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .fb_count = 2,
    };

    ESP_LOGD(TAG, "Initializing camera");

    camera_config.pin_pwdn = pwdn;
    camera_config.pin_reset = reset;
    camera_config.pin_xclk = xclk;
    camera_config.pin_sccb_sda = siod;
    camera_config.pin_sccb_scl = sioc;

    camera_config.pin_d7 = d7;
    camera_config.pin_d6 = d6;
    camera_config.pin_d5 = d5;
    camera_config.pin_d4 = d4;
    camera_config.pin_d3 = d3;
    camera_config.pin_d2 = d2;
    camera_config.pin_d1 = d1;
    camera_config.pin_d0 = d0;
    camera_config.pin_vsync = vsync;
    camera_config.pin_href = href;
    camera_config.pin_pclk = pclk;

    camera_config.frame_size = resolution_to_frame_size(resolution);
    camera_config.jpeg_quality = quality;

    if (camera_config.frame_size == FRAMESIZE_INVALID)
    {
        ESP_LOGE(TAG, "Invalid frame size: %s", resolution);
        return -1;
    }

    ESP_ERROR_CHECK(esp_camera_init(&camera_config));

    s = esp_camera_sensor_get();
    s->set_vflip(s, vflip);
    s->set_hmirror(s, hmirror);

    if (!(capture_semaphore = xSemaphoreCreateBinary()))
    {
        ESP_LOGE(TAG, "Failed creating semaphore");
        return -1;
    }

    if (xTaskCreatePinnedToCore(camera_capture_task, "camer_capture_task", 4096,
        (void *)fps, 5, NULL, 1) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed creating capture task");
        return -1;
    }

    return 0;
}
