#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stddef.h>

/* Types */
typedef struct config_update_handle_t config_update_handle_t;
typedef enum config_network_type_t {
    NETWORK_TYPE_WIFI,
    NETWORK_TYPE_ETH,
} config_network_type_t;

/* RTP Configuraton */
const char *config_rtp_host_get(void);
uint16_t config_rtp_video_port_get(void);
uint16_t config_rtp_audio_port_get(void);
uint8_t config_rtp_ttl_get(void);

/* Camera Configuraton */
int config_camera_pin_pwdn_get(void);
int config_camera_pin_reset_get(void);
int config_camera_pin_xclk_get(void);
int config_camera_pin_siod_get(void);
int config_camera_pin_sioc_get(void);
int config_camera_pin_d7_get(void);
int config_camera_pin_d6_get(void);
int config_camera_pin_d5_get(void);
int config_camera_pin_d4_get(void);
int config_camera_pin_d3_get(void);
int config_camera_pin_d2_get(void);
int config_camera_pin_d1_get(void);
int config_camera_pin_d0_get(void);
int config_camera_pin_vsync_get(void);
int config_camera_pin_href_get(void);
int config_camera_pin_pclk_get(void);
const char *config_camera_resolution_get(void);
int config_camera_fps_get(void);
uint8_t config_camera_vertical_flip_get(void);
uint8_t config_camera_horizontal_mirror_get(void);
int config_camera_quality_get(void);

/* Microphone Configuration */
int config_microphone_din_get(void);
int config_microphone_clk_get(void);
uint32_t config_microphone_sample_rate_get(void);

/* Motion Sensor Configuraton */
int config_motion_sensor_pin_get(void);

/* Ethernet Configuration */
const char *config_network_eth_phy_get(void);
int8_t config_network_eth_phy_power_pin_get(void);

/* MQTT Configuration*/
const char *config_mqtt_host_get(void);
uint16_t config_mqtt_port_get(void);
uint8_t config_mqtt_ssl_get(void);
const char *config_mqtt_server_cert_get(void);
const char *config_mqtt_client_cert_get(void);
const char *config_mqtt_client_key_get(void);
const char *config_mqtt_client_id_get(void);
const char *config_mqtt_username_get(void);
const char *config_mqtt_password_get(void);
uint8_t config_mqtt_qos_get(void);
uint8_t config_mqtt_retained_get(void);

/* Network Configuration */
config_network_type_t config_network_type_get(void);

/* WiFi Configuration*/
const char *config_network_hostname_get(void);
const char *config_network_wifi_ssid_get(void);
const char *config_network_wifi_password_get(void);
const char *config_eap_ca_cert_get(void);
const char *config_eap_client_cert_get(void);
const char *config_eap_client_key_get(void);
const char *config_eap_method_get(void);
const char *config_eap_identity_get(void);
const char *config_eap_username_get(void);
const char *config_eap_password_get(void);

/* Remote Logging Configuration */
const char *config_log_host_get(void);
uint16_t config_log_port_get(void);

/* Configuration Update */
int config_update_begin(config_update_handle_t **handle);
int config_update_write(config_update_handle_t *handle, uint8_t *data,
    size_t len);
int config_update_end(config_update_handle_t *handle);

char *config_version_get(void);
int config_initialize(void);

#endif
