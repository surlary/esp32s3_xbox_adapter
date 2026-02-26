/*
 * HTTP Server for ESP32-S3
 * Provides web server functionality for captive portal
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SPIFFS filesystem
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t http_server_spiffs_init(void);

/**
 * @brief Unmount SPIFFS filesystem
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t http_server_spiffs_deinit(void);

/**
 * @brief Initialize HTTP server
 * 
 * @param base_path Base path for serving files from SPIFFS
 * @return esp_err_t ESP_OK on success
 */
esp_err_t http_server_init(const char *base_path);

/**
 * @brief Start HTTP server on specified port
 * 
 * @param port Port number (default: 80)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t http_server_start(int port);

/**
 * @brief Stop HTTP server
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t http_server_stop(void);

/**
 * @brief Register smart captive portal handler
 * This tracks client IPs and only redirects new clients
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t http_server_register_smart_captive_portal(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SERVER_H */