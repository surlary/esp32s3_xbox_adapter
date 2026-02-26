/*
 * WiFi AP Controller for ESP32-S3
 * Handles WiFi AP mode and DNS server for captive portal
 */

#ifndef WIFI_AP_CONTROLLER_H
#define WIFI_AP_CONTROLLER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi in AP mode
 * 
 * @param ssid AP SSID
 * @param password AP password (NULL for open network)
 * @param ip_addr IP address for the AP (default: 192.168.123.1)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_ap_controller_init(const char *ssid, const char *password, const char *ip_addr);

/**
 * @brief Start WiFi AP and DNS server
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_ap_controller_start(void);

/**
 * @brief Stop WiFi AP and DNS server
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t wifi_ap_controller_stop(void);

/**
 * @brief Get the AP IP address
 * 
 * @return const char* IP address string
 */
const char* wifi_ap_controller_get_ip(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_AP_CONTROLLER_H */