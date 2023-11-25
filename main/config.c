#include "config.h"
#include <esp_err.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_spiffs.h>
#include <nvs.h>
#include <cJSON.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Types */
typedef struct config_update_handle_t {
    const esp_partition_t *partition;
    uint8_t partition_id;
    size_t bytes_written;
} config_update_handle_t;

/* Constants */
static const char *TAG = "Config";
static const char *config_file_name = "/spiffs/config.json";
static const char *nvs_namespace = "ipcam_config";
static const char *nvs_active_partition = "active_part";
static cJSON *config;

/* Internal variables */
static char config_version[65];
static nvs_handle nvs;

/* Common utilities */
static char *read_file(const char *path)
{
    int fd, len;
    struct stat st;
    char *buf, *p;

    if (stat(path, &st))
        return NULL;

    if ((fd = open(path, O_RDONLY)) < 0)
        return NULL;

    if (!(buf = p = malloc(st.st_size + 1)))
        return NULL;

    while ((len = read(fd, p, 1024)) > 0)
        p += len;
    close(fd);

    if (len < 0)
    {
        free(buf);
        return NULL;
    }

    *p = '\0';
    return buf;
}

/* RTP Configuraton */
const char *config_rtp_host_get(void)
{
    cJSON *rtp = cJSON_GetObjectItemCaseSensitive(config, "rtp");
    cJSON *ip = cJSON_GetObjectItemCaseSensitive(rtp, "host");

    if (cJSON_IsString(ip))
        return ip->valuestring;

    return "225.0.0.1";
}

static uint16_t config_rtp_port_get(const char *name, uint16_t default_value)
{
    cJSON *rtp = cJSON_GetObjectItemCaseSensitive(config, "rtp");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(rtp, name);

    if (cJSON_IsNumber(port))
        return port->valuedouble;

    return default_value;
}

uint16_t config_rtp_video_port_get(void)
{
    return config_rtp_port_get("video_port", 5000);
}

uint16_t config_rtp_audio_port_get(void)
{
    /* XXX TODO Return 0 if microphone isn't configured */
    return config_rtp_port_get("audio_port", 5002);
}

uint8_t config_rtp_ttl_get(void)
{
    cJSON *rtp = cJSON_GetObjectItemCaseSensitive(config, "rtp");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(rtp, "ttl");

    if (cJSON_IsNumber(port))
        return port->valuedouble;

    return 1;
}

/* Camera Configuraton */
static int config_camera_pin_get(const char *name)
{
    cJSON *camera = cJSON_GetObjectItemCaseSensitive(config, "camera");
    cJSON *pins = cJSON_GetObjectItemCaseSensitive(camera, "pins");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(pins, name);

    if (cJSON_IsNumber(port))
        return port->valuedouble;

    return -1;
}

#define CAMERA_PIN(name) \
  int config_camera_pin_##name##_get(void) \
  { \
    return config_camera_pin_get(#name); \
  }
CAMERA_PIN(pwdn);
CAMERA_PIN(reset);
CAMERA_PIN(xclk);
CAMERA_PIN(siod);
CAMERA_PIN(sioc);
CAMERA_PIN(d7);
CAMERA_PIN(d6);
CAMERA_PIN(d5);
CAMERA_PIN(d4);
CAMERA_PIN(d3);
CAMERA_PIN(d2);
CAMERA_PIN(d1);
CAMERA_PIN(d0);
CAMERA_PIN(vsync);
CAMERA_PIN(href);
CAMERA_PIN(pclk);
#undef CAMERA_PIN

const char *config_camera_resolution_get(void)
{
    cJSON *camera = cJSON_GetObjectItemCaseSensitive(config, "camera");
    cJSON *resolution = cJSON_GetObjectItemCaseSensitive(camera, "resolution");

    if (cJSON_IsString(resolution))
        return resolution->valuestring;

    return "800x600";
}

int config_camera_fps_get(void)
{
    cJSON *camera = cJSON_GetObjectItemCaseSensitive(config, "camera");
    cJSON *fps = cJSON_GetObjectItemCaseSensitive(camera, "fps");

    if (cJSON_IsNumber(fps))
        return fps->valuedouble;

    return 5;
}

uint8_t config_camera_vertical_flip_get(void)
{
    cJSON *camera = cJSON_GetObjectItemCaseSensitive(config, "camera");
    cJSON *vflip = cJSON_GetObjectItemCaseSensitive(camera, "vertical_flip");

    return cJSON_IsTrue(vflip);
}

uint8_t config_camera_horizontal_mirror_get(void)
{
    cJSON *camera = cJSON_GetObjectItemCaseSensitive(config, "camera");
    cJSON *hmirror = cJSON_GetObjectItemCaseSensitive(camera,
        "horizontal_mirror");

    return cJSON_IsTrue(hmirror);
}

int config_camera_quality_get(void)
{
    cJSON *camera = cJSON_GetObjectItemCaseSensitive(config, "camera");
    cJSON *quality = cJSON_GetObjectItemCaseSensitive(camera, "quality");

    if (cJSON_IsNumber(quality))
        return quality->valuedouble;

    return 12;
}

/* Microphone Configuration */
int config_microphone_din_get(void)
{
    cJSON *microphone = cJSON_GetObjectItemCaseSensitive(config, "microphone");
    cJSON *din = cJSON_GetObjectItemCaseSensitive(microphone, "din");

    if (cJSON_IsNumber(din))
        return din->valuedouble;

    return -1;
}

int config_microphone_clk_get(void)
{
    cJSON *microphone = cJSON_GetObjectItemCaseSensitive(config, "microphone");
    cJSON *clk = cJSON_GetObjectItemCaseSensitive(microphone, "clk");

    if (cJSON_IsNumber(clk))
        return clk->valuedouble;

    return -1;
}

uint32_t config_microphone_sample_rate_get(void)
{
    cJSON *microphone = cJSON_GetObjectItemCaseSensitive(config, "microphone");
    cJSON *sample_rate = cJSON_GetObjectItemCaseSensitive(microphone,
        "sample_rate");

    if (cJSON_IsNumber(sample_rate))
        return sample_rate->valuedouble;

    return 16000;
}

/* Motion Sensor Configuraton */
int config_motion_sensor_pin_get(void)
{
    cJSON *motion_sensor = cJSON_GetObjectItemCaseSensitive(config,
        "motion_sensor");
    cJSON *pin = cJSON_GetObjectItemCaseSensitive(motion_sensor, "pin");

    if (cJSON_IsNumber(pin))
        return pin->valuedouble;

    return -1;
}

/* Ethernet Configuration */
const char *config_network_eth_phy_get(void)
{
    cJSON *network = cJSON_GetObjectItemCaseSensitive(config, "network");
    cJSON *eth = cJSON_GetObjectItemCaseSensitive(network, "eth");
    cJSON *phy = cJSON_GetObjectItemCaseSensitive(eth, "phy");

    if (cJSON_IsString(phy))
        return phy->valuestring;

    return NULL;
}

int8_t config_network_eth_phy_power_pin_get(void)
{
    cJSON *network = cJSON_GetObjectItemCaseSensitive(config, "network");
    cJSON *eth = cJSON_GetObjectItemCaseSensitive(network, "eth");
    cJSON *phy_power_pin = cJSON_GetObjectItemCaseSensitive(eth,
        "phy_power_pin");

    if (cJSON_IsNumber(phy_power_pin))
        return phy_power_pin->valuedouble;

    return -1;
}

/* MQTT Configuration*/
const char *config_mqtt_server_get(const char *param_name)
{
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(config, "mqtt");
    cJSON *server = cJSON_GetObjectItemCaseSensitive(mqtt, "server");
    cJSON *param = cJSON_GetObjectItemCaseSensitive(server, param_name);

    if (cJSON_IsString(param))
        return param->valuestring;

    return NULL;
}

const char *config_mqtt_host_get(void)
{
    return config_mqtt_server_get("host");
}

uint16_t config_mqtt_port_get(void)
{
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(config, "mqtt");
    cJSON *server = cJSON_GetObjectItemCaseSensitive(mqtt, "server");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(server, "port");

    if (cJSON_IsNumber(port))
        return port->valuedouble;

    return 0;
}

uint8_t config_mqtt_ssl_get(void)
{
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(config, "mqtt");
    cJSON *server = cJSON_GetObjectItemCaseSensitive(mqtt, "server");
    cJSON *ssl = cJSON_GetObjectItemCaseSensitive(server, "ssl");

    return cJSON_IsTrue(ssl);
}

const char *config_mqtt_file_get(const char *field)
{
    const char *file = config_mqtt_server_get(field);
    char buf[128];

    if (!file)
        return NULL;

    snprintf(buf, sizeof(buf), "/spiffs%s", file);
    return read_file(buf);
}

const char *config_mqtt_server_cert_get(void)
{
    static const char *cert;

    if (!cert)
        cert = config_mqtt_file_get("server_cert");

    return cert;
}

const char *config_mqtt_client_cert_get(void)
{
    static const char *cert;

    if (!cert)
        cert = config_mqtt_file_get("client_cert");

    return cert;
}

const char *config_mqtt_client_key_get(void)
{
    static const char *key;

    if (!key)
        key = config_mqtt_file_get("client_key");

    return key;
}

const char *config_mqtt_client_id_get(void)
{
    return config_mqtt_server_get("client_id");
}

const char *config_mqtt_username_get(void)
{
    return config_mqtt_server_get("username");
}

const char *config_mqtt_password_get(void)
{
    return config_mqtt_server_get("password");
}

uint8_t config_mqtt_qos_get(void)
{
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(config, "mqtt");
    cJSON *publish = cJSON_GetObjectItemCaseSensitive(mqtt, "publish");
    cJSON *qos = cJSON_GetObjectItemCaseSensitive(publish, "qos");

    if (cJSON_IsNumber(qos))
        return qos->valuedouble;

    return 0;
}

uint8_t config_mqtt_retained_get(void)
{
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(config, "mqtt");
    cJSON *publish = cJSON_GetObjectItemCaseSensitive(mqtt, "publish");
    cJSON *retain = cJSON_GetObjectItemCaseSensitive(publish, "retain");

    return cJSON_IsTrue(retain);
}

/* Network Configuration */
config_network_type_t config_network_type_get(void)
{
    cJSON *network = cJSON_GetObjectItemCaseSensitive(config, "network");
    cJSON *eth = cJSON_GetObjectItemCaseSensitive(network, "eth");

    return eth ? NETWORK_TYPE_ETH : NETWORK_TYPE_WIFI;
}

/* WiFi Configuration */
const char *config_network_hostname_get(void)
{
    cJSON *network = cJSON_GetObjectItemCaseSensitive(config, "network");
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(network, "hostname");

    if (cJSON_IsString(ssid))
        return ssid->valuestring;

    return NULL;
}

const char *config_network_wifi_ssid_get(void)
{
    cJSON *network = cJSON_GetObjectItemCaseSensitive(config, "network");
    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(network, "wifi");
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(wifi, "ssid");

    if (cJSON_IsString(ssid))
        return ssid->valuestring;

    return "MY_SSID";
}

const char *config_network_wifi_password_get(void)
{
    cJSON *network = cJSON_GetObjectItemCaseSensitive(config, "network");
    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(network, "wifi");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(wifi, "password");

    if (cJSON_IsString(password))
        return password->valuestring;

    return NULL;
}

const char *config_network_wifi_eap_get(const char *param_name)
{
    cJSON *network = cJSON_GetObjectItemCaseSensitive(config, "network");
    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(network, "wifi");
    cJSON *eap = cJSON_GetObjectItemCaseSensitive(wifi, "eap");
    cJSON *param = cJSON_GetObjectItemCaseSensitive(eap, param_name);

    if (cJSON_IsString(param))
        return param->valuestring;

    return NULL;
}

const char *config_eap_file_get(const char *field)
{
    const char *file = config_network_wifi_eap_get(field);
    char buf[128];

    if (!file)
        return NULL;

    snprintf(buf, sizeof(buf), "/spiffs%s", file);
    return read_file(buf);
}

const char *config_eap_ca_cert_get(void)
{
    static const char *cert;

    if (!cert)
        cert = config_eap_file_get("ca_cert");

    return cert;
}

const char *config_eap_client_cert_get(void)
{
    static const char *cert;

    if (!cert)
        cert = config_eap_file_get("client_cert");

    return cert;
}

const char *config_eap_client_key_get(void)
{
    static const char *key;

    if (!key)
        key = config_eap_file_get("client_key");

    return key;
}

const char *config_eap_method_get(void)
{
    return config_network_wifi_eap_get("method");
}

const char *config_eap_identity_get(void)
{
    return config_network_wifi_eap_get("identity");
}

const char *config_eap_username_get(void)
{
    return config_network_wifi_eap_get("username");
}

const char *config_eap_password_get(void)
{
    return config_network_wifi_eap_get("password");
}

/* Remote Logging Configuration */
const char *config_log_host_get(void)
{
    cJSON *log = cJSON_GetObjectItemCaseSensitive(config, "log");
    cJSON *ip = cJSON_GetObjectItemCaseSensitive(log, "host");

    if (cJSON_IsString(ip))
        return ip->valuestring;

    return NULL;
}

uint16_t config_log_port_get(void)
{
    cJSON *log = cJSON_GetObjectItemCaseSensitive(config, "log");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(log, "port");

    if (cJSON_IsNumber(port))
        return port->valuedouble;

    return 0;
}

/* Configuration Update */
static int config_active_partition_get(void)
{
    uint8_t partition = 0;

    nvs_get_u8(nvs, nvs_active_partition, &partition);
    return partition;
}

static int config_active_partition_set(uint8_t partition)
{
    ESP_LOGD(TAG, "Setting active partition to %u", partition);

    if (nvs_set_u8(nvs, nvs_active_partition, partition) != ESP_OK ||
        nvs_commit(nvs) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed setting active partition to: %u", partition);
        return -1;
    }

    return 0;
}

int config_update_begin(config_update_handle_t **handle)
{
    const esp_partition_t *partition;
    char partition_name[5];
    uint8_t partition_id = !config_active_partition_get();

    sprintf(partition_name, "fs_%u", partition_id);
    ESP_LOGI(TAG, "Writing to partition %s", partition_name);
    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS, partition_name);

    if (!partition)
    {
        ESP_LOGE(TAG, "Failed finding SPIFFS partition");
        return -1;
    }

    ESP_LOGI(TAG, "Writing partition type 0x%0x subtype 0x%0x (offset 0x%08"
        PRIx32 ")", partition->type, partition->subtype, partition->address);

    /* Erase partition, needed before writing is allowed */
    if (esp_partition_erase_range(partition, 0, partition->size))
        return -1;

    *handle = malloc(sizeof(**handle));
    (*handle)->partition = partition;
    (*handle)->partition_id = partition_id;
    (*handle)->bytes_written = 0;

    return 0;
}

int config_update_write(config_update_handle_t *handle, uint8_t *data,
    size_t len)
{
    if (esp_partition_write(handle->partition, handle->bytes_written, data,
        len))
    {
        ESP_LOGE(TAG, "Failed writing to SPIFFS partition!");
        free(handle);
        return -1;
    }

    handle->bytes_written += len;
    return 0;
}

int config_update_end(config_update_handle_t *handle)
{
    int ret = -1;

    /* We succeeded only if the entire partition was written */
    if (handle->bytes_written == handle->partition->size)
        ret = config_active_partition_set(handle->partition_id);

    free(handle);
    return ret;
}

static cJSON *load_json(const char *path)
{
    char *str = read_file(path);
    cJSON *json;

    if (!str)
        return NULL;

    json = cJSON_Parse(str);

    free(str);
    return json;
}

char *config_version_get(void)
{
    return config_version;
}

int config_load(uint8_t partition_id)
{
    char *p, partition_name[] = { 'f', 's', '_', 'x', '\0' };
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = partition_name,
        .max_files = 8,
        .format_if_mount_failed = true
    };
    uint8_t i, sha[32];

    partition_name[3] = partition_id + '0';
    ESP_LOGD(TAG, "Loading config from partition %s", partition_name);

    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    /* Load config.json from SPIFFS */
    if (!(config = load_json(config_file_name)))
    {
        esp_vfs_spiffs_unregister(partition_name);
        return -1;
    }

    /* Calulate hash of active partition */
    esp_partition_get_sha256(esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS, partition_name), sha);
    for (p = config_version, i = 0; i < sizeof(sha); i++)
        p += sprintf(p, "%02x", sha[i]);

    return 0;
}

int config_initialize(void)
{
    uint8_t partition;

    ESP_LOGI(TAG, "Initializing configuration");
    ESP_ERROR_CHECK(nvs_open(nvs_namespace, NVS_READWRITE, &nvs));

    partition = config_active_partition_get();

    /* Attempt to load configuration from active partition with fall-back */
    if (config_load(partition))
    {
        ESP_LOGE(TAG, "Failed loading partition %d, falling back to %d",
            partition, !partition);
        if (config_load(!partition))
        {
            ESP_LOGE(TAG, "Failed loading partition %d as well", !partition);
            return -1;
        }
        /* Fall-back partition is OK, mark it as active */
        config_active_partition_set(!partition);
    }

    ESP_LOGI(TAG, "version: %s", config_version_get());
    return 0;
}
