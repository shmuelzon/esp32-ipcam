#include "audio_encoder.h"
#include "camera.h"
#include "config.h"
#include "eth.h"
#include "httpd.h"
#include "log.h"
#include "microphone.h"
#include "motion_sensor.h"
#include "mqtt.h"
#include "ota.h"
#include "resolve.h"
#include "rtp.h"
#include "wifi.h"
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <mdns.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <string.h>

#define MAX_TOPIC_LEN 256
static const char *TAG = "IPCAM";

static const char *device_name_get(void)
{
    static const char *name = NULL;
    uint8_t *mac = NULL;

    if (name)
        return name;

    if ((name = config_network_hostname_get()))
        return name;

    switch (config_network_type_get())
    {
    case NETWORK_TYPE_ETH:
        mac = eth_mac_get();
        break;
    case NETWORK_TYPE_WIFI:
        mac = wifi_mac_get();
        break;
    }
    name = malloc(14);
    sprintf((char *)name, "IPCAM-%02X%02X", mac[4], mac[5]);

    return name;
}

/* Bookkeeping functions */
static void heartbeat_publish(void)
{
    char topic[MAX_TOPIC_LEN];
    char buf[16];

    /* Only publish uptime when connected, we don't want it to be queued */
    if (!mqtt_is_connected())
        return;

    /* Uptime (in seconds) */
    sprintf(buf, "%" PRId64, esp_timer_get_time() / 1000 / 1000);
    snprintf(topic, MAX_TOPIC_LEN, "%s/Uptime", device_name_get());
    mqtt_publish(topic, (uint8_t *)buf, strlen(buf), config_mqtt_qos_get(),
        config_mqtt_retained_get());

    /* Free memory (in bytes) */
    sprintf(buf, "%" PRIu32, esp_get_free_heap_size());
    snprintf(topic, MAX_TOPIC_LEN, "%s/FreeMemory", device_name_get());
    mqtt_publish(topic, (uint8_t *)buf, strlen(buf), config_mqtt_qos_get(),
        config_mqtt_retained_get());
}

static void self_publish(void)
{
    char topic[MAX_TOPIC_LEN];
    char *payload;

    /* Current status */
    payload = "Online";
    snprintf(topic, MAX_TOPIC_LEN, "%s/Status", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());

    /* App version */
    payload = IPCAM_VER;
    snprintf(topic, MAX_TOPIC_LEN, "%s/Version", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());

    /* Config version */
    payload = config_version_get();
    snprintf(topic, MAX_TOPIC_LEN, "%s/ConfigVersion", device_name_get());
    mqtt_publish(topic, (uint8_t *)payload, strlen(payload),
        config_mqtt_qos_get(), config_mqtt_retained_get());

    heartbeat_publish();
}

/* OTA functions */
static void ota_on_completed(ota_type_t type, ota_err_t err)
{
    ESP_LOGI(TAG, "Update completed: %s", ota_err_to_str(err));

    /* All done, restart */
    if (err == OTA_ERR_SUCCESS)
        abort();

    /* If upgrade failed, start stream again */
    camera_start();
    microphone_start();
}

static void _ota_on_completed(ota_type_t type, ota_err_t err);

static void ota_on_mqtt(const char *topic, const uint8_t *payload, size_t len,
    void *ctx)
{
    char *url = malloc(len + 1);
    ota_type_t type = (ota_type_t)ctx;
    ota_err_t err;

    memcpy(url, payload, len);
    url[len] = '\0';
    ESP_LOGI(TAG, "Starting %s update from %s",
        type == OTA_TYPE_FIRMWARE ? "firmware" : "configuration", url);

    if ((err = ota_download(type, url, _ota_on_completed)) != OTA_ERR_SUCCESS)
        ESP_LOGE(TAG, "Failed updating: %s", ota_err_to_str(err));

    camera_stop();
    microphone_stop();
    free(url);
}

static void _ota_on_mqtt(const char *topic, const uint8_t *payload, size_t len,
    void *ctx);

static void ota_subscribe(void)
{
    char topic[MAX_TOPIC_LEN];

    /* Register for both a specific topic for this device and a general one */
    snprintf(topic, MAX_TOPIC_LEN, "%s/OTA/Firmware", device_name_get());
    mqtt_subscribe(topic, 0, _ota_on_mqtt, (void *)OTA_TYPE_FIRMWARE, NULL);
    mqtt_subscribe("IPCAM/OTA/Firmware", 0, _ota_on_mqtt,
        (void *)OTA_TYPE_FIRMWARE, NULL);

    snprintf(topic, MAX_TOPIC_LEN, "%s/OTA/Config", device_name_get());
    mqtt_subscribe(topic, 0, _ota_on_mqtt, (void *)OTA_TYPE_CONFIG, NULL);
    mqtt_subscribe("IPCAM/OTA/Config", 0, _ota_on_mqtt,
        (void *)OTA_TYPE_CONFIG, NULL);
}

static void ota_unsubscribe(void)
{
    char topic[27];

    sprintf(topic, "%s/OTA/Firmware", device_name_get());
    mqtt_unsubscribe(topic);
    mqtt_unsubscribe("IPCAM/OTA/Firmware");

    sprintf(topic, "%s/OTA/Config", device_name_get());
    mqtt_unsubscribe(topic);
    mqtt_unsubscribe("IPCAM/OTA/Config");
}

/* Management functions */
static void management_on_restart_mqtt(const char *topic,
    const uint8_t *payload, size_t len, void *ctx)
{
    if (len != 4 || strncmp((char *)payload, "true", len))
        return;

    abort();
}

static void management_on_capture_mqtt(const char *topic,
    const uint8_t *payload, size_t len, void *ctx)
{
    if (len == 4 && !strncmp((char *)payload, "true", len))
    {
        camera_start();
        microphone_start();
        return;
    }
    if (len == 5 && !strncmp((char *)payload, "false", len))
    {
        camera_stop();
        microphone_stop();
        return;
    }
}

static void _management_on_restart_mqtt(const char *topic,
    const uint8_t *payload, size_t len, void *ctx);
static void _management_on_capture_mqtt(const char *topic,
    const uint8_t *payload, size_t len, void *ctx);

static void management_subscribe(void)
{
    char topic[MAX_TOPIC_LEN];

    snprintf(topic, MAX_TOPIC_LEN, "%s/Restart", device_name_get());
    mqtt_subscribe(topic, 0, _management_on_restart_mqtt, NULL, NULL);
    mqtt_subscribe("IPCAM/Restart", 0, _management_on_restart_mqtt, NULL,
        NULL);

    snprintf(topic, MAX_TOPIC_LEN, "%s/Capture", device_name_get());
    mqtt_subscribe(topic, 0, _management_on_capture_mqtt, NULL, NULL);
}

static void management_unsubscribe(void)
{
    char topic[MAX_TOPIC_LEN];

    snprintf(topic, MAX_TOPIC_LEN, "%s/Capture", device_name_get());
    mqtt_unsubscribe(topic);

    snprintf(topic, MAX_TOPIC_LEN, "%s/Restart", device_name_get());
    mqtt_unsubscribe(topic);
    mqtt_unsubscribe("IPCAM/Restart");
}

/* Camera functions */
static void cleanup(void)
{
    ota_unsubscribe();
    management_unsubscribe();
}

/* Network callback functions */
static void network_on_connected(void)
{
    char status_topic[MAX_TOPIC_LEN];

    log_start(config_log_host_get(), config_log_port_get());
    ESP_LOGI(TAG, "Connected to the network, connecting to MQTT");
    snprintf(status_topic, MAX_TOPIC_LEN, "%s/Status", device_name_get());

    mqtt_connect(config_mqtt_host_get(), config_mqtt_port_get(),
        config_mqtt_client_id_get(), config_mqtt_username_get(),
        config_mqtt_password_get(), config_mqtt_ssl_get(),
        config_mqtt_server_cert_get(), config_mqtt_client_cert_get(),
        config_mqtt_client_key_get(), status_topic, "Offline",
        config_mqtt_qos_get(), config_mqtt_retained_get());
    camera_start();
    microphone_start();
}

static void network_on_disconnected(void)
{
    log_stop();
    ESP_LOGI(TAG, "Disconnected from the network, stopping MQTT");
    mqtt_disconnect();
    /* We don't get notified when manually stopping MQTT */
    cleanup();
    camera_stop();
    microphone_stop();
}

/* MQTT callback functions */
static void mqtt_on_connected(void)
{
    ESP_LOGI(TAG, "Connected to MQTT");
    self_publish();
    ota_subscribe();
    management_subscribe();
}

static void mqtt_on_disconnected(void)
{
    static uint8_t num_disconnections = 0;

    ESP_LOGI(TAG, "Disconnected from MQTT");
    cleanup();

    if (++num_disconnections % 3 == 0)
    {
        ESP_LOGI(TAG,
            "Failed connecting to MQTT 3 times, reconnecting to the network");
        wifi_reconnect();
    }
}

/* Motion sensor callback functions */
static void motion_sensor_on_trigger(int pin, int level)
{
    char topic[MAX_TOPIC_LEN];
    char *payload = "false";
    size_t len = 5;

    snprintf(topic, MAX_TOPIC_LEN, "%s/MotionDetected", device_name_get());
    if (level)
    {
        payload = "true";
        len = 4;
    }
    ESP_LOGI(TAG, "Motion detected: %s", payload);
    mqtt_publish(topic, (uint8_t *)payload, len, config_mqtt_qos_get(),
        config_mqtt_retained_get());
}

/* IPCAM task and event callbacks */
typedef enum {
    EVENT_TYPE_HEARTBEAT_TIMER,
    EVENT_TYPE_NETWORK_CONNECTED,
    EVENT_TYPE_NETWORK_DISCONNECTED,
    EVENT_TYPE_OTA_MQTT,
    EVENT_TYPE_OTA_COMPLETED,
    EVENT_TYPE_MANAGEMENT_RESTART_MQTT,
    EVENT_TYPE_MANAGEMENT_CAPTURE_MQTT,
    EVENT_TYPE_MQTT_CONNECTED,
    EVENT_TYPE_MQTT_DISCONNECTED,
    EVENT_TYPE_MOTION_SENSOR_TRIGGERED,
} event_type_t;

typedef struct {
    event_type_t type;
    union {
        struct {
            ota_type_t type;
            ota_err_t err;
        } ota_completed;
        struct {
            char *topic;
            uint8_t *payload;
            size_t len;
            void *ctx;
        } mqtt_message;
        struct {
            int pin;
            int level;
        } motion_sensor_triggered;
    };
} event_t;

static QueueHandle_t event_queue;

static void ipcam_handle_event(event_t *event)
{
    switch (event->type)
    {
    case EVENT_TYPE_HEARTBEAT_TIMER:
        heartbeat_publish();
        break;
    case EVENT_TYPE_NETWORK_CONNECTED:
        network_on_connected();
        break;
    case EVENT_TYPE_NETWORK_DISCONNECTED:
        network_on_disconnected();
        break;
    case EVENT_TYPE_OTA_MQTT:
        ota_on_mqtt(event->mqtt_message.topic, event->mqtt_message.payload,
            event->mqtt_message.len, event->mqtt_message.ctx);
        free(event->mqtt_message.topic);
        free(event->mqtt_message.payload);
        break;
    case EVENT_TYPE_OTA_COMPLETED:
        ota_on_completed(event->ota_completed.type, event->ota_completed.err);
        break;
    case EVENT_TYPE_MANAGEMENT_RESTART_MQTT:
        management_on_restart_mqtt(event->mqtt_message.topic,
            event->mqtt_message.payload, event->mqtt_message.len,
            event->mqtt_message.ctx);
        free(event->mqtt_message.topic);
        free(event->mqtt_message.payload);
        break;
    case EVENT_TYPE_MANAGEMENT_CAPTURE_MQTT:
        management_on_capture_mqtt(event->mqtt_message.topic,
            event->mqtt_message.payload, event->mqtt_message.len,
            event->mqtt_message.ctx);
        free(event->mqtt_message.topic);
        free(event->mqtt_message.payload);
        break;
    case EVENT_TYPE_MQTT_CONNECTED:
        mqtt_on_connected();
        break;
    case EVENT_TYPE_MQTT_DISCONNECTED:
        mqtt_on_disconnected();
        break;
    case EVENT_TYPE_MOTION_SENSOR_TRIGGERED:
        motion_sensor_on_trigger(event->motion_sensor_triggered.pin,
            event->motion_sensor_triggered.level);
        break;
    }

    free(event);
}

static void ipcam_task(void *pvParameter)
{
    event_t *event;

    while (1)
    {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) != pdTRUE)
            continue;

        ipcam_handle_event(event);
    }

    vTaskDelete(NULL);
}

static void heartbeat_timer_cb(TimerHandle_t xTimer)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_HEARTBEAT_TIMER;

    ESP_LOGD(TAG, "Queuing event HEARTBEAT_TIMER");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static int start_ipcam_task(void)
{
    TimerHandle_t hb_timer;

    if (!(event_queue = xQueueCreate(10, sizeof(event_t *))))
        return -1;

    if (xTaskCreatePinnedToCore(ipcam_task, "ipcam_task", 4096, NULL, 5, NULL,
        1) != pdPASS)
    {
        return -1;
    }

    hb_timer = xTimerCreate("heartbeat", pdMS_TO_TICKS(60 * 1000), pdTRUE,
        NULL, heartbeat_timer_cb);
    xTimerStart(hb_timer, 0);

    return 0;
}

static void _mqtt_on_message(event_type_t type, const char *topic,
    const uint8_t *payload, size_t len, void *ctx)
{
    event_t *event = malloc(sizeof(*event));

    event->type = type;
    event->mqtt_message.topic = strdup(topic);
    event->mqtt_message.payload = malloc(len);
    memcpy(event->mqtt_message.payload, payload, len);
    event->mqtt_message.len = len;
    event->mqtt_message.ctx = ctx;

    ESP_LOGD(TAG, "Queuing event MQTT message %d (%s, %p, %u, %p)", type, topic,
        payload, len, ctx);
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _network_on_connected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_NETWORK_CONNECTED;

    ESP_LOGD(TAG, "Queuing event NETWORK_CONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _network_on_disconnected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_NETWORK_DISCONNECTED;

    ESP_LOGD(TAG, "Queuing event NETWORK_DISCONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _ota_on_mqtt(const char *topic, const uint8_t *payload, size_t len,
    void *ctx)
{
    _mqtt_on_message(EVENT_TYPE_OTA_MQTT, topic, payload, len, ctx);
}

static void _ota_on_completed(ota_type_t type, ota_err_t err)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_OTA_COMPLETED;
    event->ota_completed.type = type;
    event->ota_completed.err = err;

    ESP_LOGD(TAG, "Queuing event HEARTBEAT_TIMER (%d, %d)", type, err);
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _management_on_restart_mqtt(const char *topic,
    const uint8_t *payload, size_t len, void *ctx)
{
    _mqtt_on_message(EVENT_TYPE_MANAGEMENT_RESTART_MQTT, topic, payload, len,
        ctx);
}

static void _management_on_capture_mqtt(const char *topic,
    const uint8_t *payload, size_t len, void *ctx)
{
    _mqtt_on_message(EVENT_TYPE_MANAGEMENT_CAPTURE_MQTT, topic, payload, len,
        ctx);
}

static void _mqtt_on_connected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_MQTT_CONNECTED;

    ESP_LOGD(TAG, "Queuing event MQTT_CONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _mqtt_on_disconnected(void)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_MQTT_DISCONNECTED;

    ESP_LOGD(TAG, "Queuing event MQTT_DISCONNECTED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

static void _motion_sensor_triggered(int pin, int level)
{
    event_t *event = malloc(sizeof(*event));

    event->type = EVENT_TYPE_MOTION_SENSOR_TRIGGERED;
    event->motion_sensor_triggered.pin = pin;
    event->motion_sensor_triggered.level = level;

    ESP_LOGD(TAG, "Queuing event MOTION_SENSOR_TRIGGERED");
    xQueueSend(event_queue, &event, portMAX_DELAY);
}

void app_main()
{
    int config_failed;

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Version: %s", IPCAM_VER);

    /* Init configuration */
    config_failed = config_initialize();

    /* Init remote logging */
    ESP_ERROR_CHECK(log_initialize());

    /* Init OTA */
    ESP_ERROR_CHECK(ota_initialize());

    /* Init Network */
    switch (config_network_type_get())
    {
    case NETWORK_TYPE_ETH:
        /* Init Ethernet */
        ESP_ERROR_CHECK(eth_initialize());
        eth_hostname_set(device_name_get());
        eth_set_on_connected_cb(_network_on_connected);
        eth_set_on_disconnected_cb(_network_on_disconnected);
        break;
    case NETWORK_TYPE_WIFI:
        /* Init Wi-Fi */
        ESP_ERROR_CHECK(wifi_initialize());
        wifi_hostname_set(device_name_get());
        wifi_set_on_connected_cb(_network_on_connected);
        wifi_set_on_disconnected_cb(_network_on_disconnected);
        break;
    }

    /* Init mDNS */
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(device_name_get());

    /* Init name resolver */
    ESP_ERROR_CHECK(resolve_initialize());

    /* Init MQTT */
    ESP_ERROR_CHECK(mqtt_initialize());
    mqtt_set_on_connected_cb(_mqtt_on_connected);
    mqtt_set_on_disconnected_cb(_mqtt_on_disconnected);

    /* Init web server */
    ESP_ERROR_CHECK(httpd_initialize(config_rtp_host_get(),
        config_rtp_video_port_get(), config_rtp_audio_port_get()));
    httpd_set_on_ota_completed_cb(_ota_on_completed);

    /* Init motion sensor */
    ESP_ERROR_CHECK(motion_sensor_initialize(config_motion_sensor_pin_get()));
    motion_sensor_set_on_trigger(_motion_sensor_triggered);

    /* Init camera */
    ESP_ERROR_CHECK(camera_initialize(config_camera_pin_pwdn_get(),
        config_camera_pin_reset_get(), config_camera_pin_xclk_get(),
        config_camera_pin_siod_get(), config_camera_pin_sioc_get(),
        config_camera_pin_d7_get(), config_camera_pin_d6_get(),
        config_camera_pin_d5_get(), config_camera_pin_d4_get(),
        config_camera_pin_d3_get(), config_camera_pin_d2_get(),
        config_camera_pin_d1_get(), config_camera_pin_d0_get(),
        config_camera_pin_vsync_get(), config_camera_pin_href_get(),
        config_camera_pin_pclk_get(), config_camera_resolution_get(),
        config_camera_fps_get(), config_camera_vertical_flip_get(),
        config_camera_horizontal_mirror_get(), config_camera_quality_get()));

    /* Init microphone */
    ESP_ERROR_CHECK(microphone_initialize(config_microphone_clk_get(),
        config_microphone_din_get(), config_microphone_sample_rate_get()));

    /* Init audio encoder, if needed */
    if (config_microphone_clk_get() != -1 &&
        config_microphone_din_get() != -1)
    {
        ESP_ERROR_CHECK(audio_encoder_initialize(AUDIO_CODEC_OPUS,
            config_microphone_sample_rate_get()));
    }

    /* Init RTP */
    ESP_ERROR_CHECK(rtp_initialize(config_rtp_host_get(),
        config_rtp_video_port_get(), config_rtp_audio_port_get()));
    rtp_ttl_set(config_rtp_ttl_get());

    /* Start IPCAM task */
    ESP_ERROR_CHECK(start_ipcam_task());

    /* Failed to load configuration or it wasn't set, create access point */
    if (config_failed || !strcmp(config_network_wifi_ssid_get() ? : "", "MY_SSID"))
    {
        wifi_start_ap(device_name_get(), NULL);
        return;
    }

    switch (config_network_type_get())
    {
    case NETWORK_TYPE_ETH:
        eth_connect(eth_phy_atophy(config_network_eth_phy_get()),
            config_network_eth_phy_power_pin_get());
        break;
    case NETWORK_TYPE_WIFI:
        /* Start by connecting to network */
        wifi_connect(config_network_wifi_ssid_get(), config_network_wifi_password_get(),
            wifi_eap_atomethod(config_eap_method_get()),
            config_eap_identity_get(),
            config_eap_username_get(), config_eap_password_get(),
            config_eap_ca_cert_get(), config_eap_client_cert_get(),
            config_eap_client_key_get());
        break;
    }
}
