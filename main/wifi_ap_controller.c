/*
 * WiFi AP Controller for ESP32-S3
 * Handles WiFi AP mode and DNS server for captive portal
 */

#include "wifi_ap_controller.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"
#include <string.h>

static const char *TAG = "WIFI_AP_CTRL";

static esp_netif_t *s_ap_netif = NULL;
static char s_ap_ip[16] = "192.168.123.1";
static bool s_ap_started = false;

// DNS server task handle
static TaskHandle_t dns_server_task_handle = NULL;

/**
 * @brief DNS server task
 * This task handles DNS requests and responds with the AP's IP for all queries
 */
static void dns_server_task(void *pvParameters)
{
    struct netconn *conn;
    struct netbuf *buf;
    err_t err;
    char *recv_buf;
    u16_t recv_len;

    // Create UDP connection on port 53 (DNS)
    conn = netconn_new(NETCONN_UDP);
    if (conn == NULL) {
        ESP_LOGE(TAG, "Failed to create DNS server connection");
        vTaskDelete(NULL);
        return;
    }

    err = netconn_bind(conn, IP_ADDR_ANY, 53);
    if (err != ERR_OK) {
        ESP_LOGE(TAG, "Failed to bind DNS server: %d", err);
        netconn_delete(conn);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Server started on %s:53", s_ap_ip);

    while (1) {
        err = netconn_recv(conn, &buf);
        if (err == ERR_OK) {
            recv_len = netbuf_len(buf);
            if (recv_len > 0) {
                recv_buf = (char *)malloc(recv_len);
                if (recv_buf != NULL) {
                    void* temp_ptr;
                    netbuf_data(buf, &temp_ptr, &recv_len);
                    if(temp_ptr != NULL) {
                        memcpy(recv_buf, temp_ptr, recv_len);
                        
                        // Check if it's a valid DNS query
                        if (recv_len >= 12 && (recv_buf[2] & 0x80) == 0) { // Check QR bit is 0 (query)
                            // Create DNS response
                            char *dns_resp = (char *)malloc(recv_len + 16);
                            if (dns_resp != NULL) {
                                // Copy original query
                                memcpy(dns_resp, recv_buf, recv_len);
                                
                                // Modify header to make it a response
                                dns_resp[2] |= 0x80; // Set QR bit to 1 (response)
                                dns_resp[3] = 0x80; // Set RCODE to 0 (no error)
                                
                                // Set answer count to 1
                                dns_resp[6] = 0; // ANCOUNT high byte
                                dns_resp[7] = 1; // ANCOUNT low byte
                                
                                // Set authority and additional counts to 0
                                dns_resp[8] = 0; // NSCOUNT high byte
                                dns_resp[9] = 0; // NSCOUNT low byte
                                dns_resp[10] = 0; // ARCOUNT high byte
                                dns_resp[11] = 0; // ARCOUNT low byte
                                
                                // Add response data (simplified)
                                int pos = recv_len;
                                
                                // Add answer name (pointer to query name)
                                dns_resp[pos++] = 0xC0; // Pointer
                                dns_resp[pos++] = 0x0C; // Offset to query name
                                
                                // Add type A (0x0001)
                                dns_resp[pos++] = 0x00;
                                dns_resp[pos++] = 0x01;
                                
                                // Add class IN (0x0001)
                                dns_resp[pos++] = 0x00;
                                dns_resp[pos++] = 0x01;
                                
                                // Add TTL (4 bytes, 300 seconds)
                                dns_resp[pos++] = 0x00;
                                dns_resp[pos++] = 0x00;
                                dns_resp[pos++] = 0x01;
                                dns_resp[pos++] = 0x2C;
                                
                                // Add data length (4 bytes for IPv4)
                                dns_resp[pos++] = 0x00;
                                dns_resp[pos++] = 0x04;
                                
                                // Add IP address
                                int ip_parts[4];
                                sscanf(s_ap_ip, "%d.%d.%d.%d", &ip_parts[0], &ip_parts[1], &ip_parts[2], &ip_parts[3]);
                                dns_resp[pos++] = ip_parts[0];
                                dns_resp[pos++] = ip_parts[1];
                                dns_resp[pos++] = ip_parts[2];
                                dns_resp[pos++] = ip_parts[3];
                                
                                // Send response
                                struct netbuf *response = netbuf_new();
                                if (response != NULL) {
                                    void *response_data = netbuf_alloc(response, pos);
                                    if (response_data != NULL) {
                                        memcpy(response_data, dns_resp, pos);
                                        
                                        // Send response back to client
                                        netconn_sendto(conn, response, &(buf->addr), buf->port);
                                    }
                                    netbuf_delete(response);
                                }
                            }
                            if (dns_resp) free(dns_resp);
                        }
                    }
                    free(recv_buf);
                }
            }
            netbuf_delete(buf);  // Free the received buffer
        } else {
            // Handle error case appropriately
            vTaskDelay(10 / portTICK_PERIOD_MS);  // Small delay to prevent tight loop
        }
    }
    
    // This point should never be reached, but include cleanup for completeness
    netconn_close(conn);
    netconn_delete(conn);
    vTaskDelete(NULL);
}

esp_err_t wifi_ap_controller_init(const char *ssid, const char *password, const char *ip_addr)
{
    esp_err_t ret;
    
    // Initialize TCP/IP stack
    ESP_LOGI(TAG, "Initializing TCP/IP stack");
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create default event loop
    ESP_LOGI(TAG, "Creating event loop");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Configure AP
    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    s_ap_netif = esp_netif_create_wifi(WIFI_IF_AP, &esp_netif_config);
    
    // Set WiFi AP configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Set default interface
    ESP_ERROR_CHECK(esp_netif_set_default_wifi_ap());
    
    // Set WiFi mode to AP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    
    // Configure AP parameters
    wifi_config_t wifi_config = {0};
    strcpy((char*)wifi_config.ap.ssid, ssid ? ssid : "ESP32-S3-HID-Bridge");
    wifi_config.ap.ssid_len = ssid ? strlen(ssid) : strlen("ESP32-S3-HID-Bridge");
    wifi_config.ap.channel = 1;
    wifi_config.ap.authmode = password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (password) {
        strcpy((char*)wifi_config.ap.password, password);
    } else {
        wifi_config.ap.password[0] = 0;
    }
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.beacon_interval = 100;
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    
    // Configure IP for AP if provided
    if (ip_addr) {
        strcpy(s_ap_ip, ip_addr);
    }
    
    // Set static IP for AP
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
    
    // Set IP address
    esp_netif_str_to_ip4(s_ap_ip, &ip_info.ip);
    esp_netif_str_to_ip4("255.255.255.0", &ip_info.netmask);
    esp_netif_str_to_ip4(s_ap_ip, &ip_info.gw); // Gateway is the AP itself
    
    esp_netif_dhcps_stop(s_ap_netif); // Stop DHCP server first
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif); // Start DHCP server
    
    ESP_LOGI(TAG, "WiFi AP configured with IP: %s", s_ap_ip);
    
    return ESP_OK;
}

esp_err_t wifi_ap_controller_start(void)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "Starting WiFi AP");
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi AP: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "WiFi AP started successfully");
    
    // Start DNS server task
    BaseType_t task_ret = xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_server_task_handle);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DNS server task");
        return ESP_FAIL;
    }
    
    s_ap_started = true;
    ESP_LOGI(TAG, "DNS server started");
    
    return ESP_OK;
}

esp_err_t wifi_ap_controller_stop(void)
{
    esp_err_t ret;
    
    if (dns_server_task_handle) {
        vTaskDelete(dns_server_task_handle);
        dns_server_task_handle = NULL;
    }
    
    ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop WiFi AP: %s", esp_err_to_name(ret));
        return ret;
    }
    
    s_ap_started = false;
    ESP_LOGI(TAG, "WiFi AP stopped");
    
    return ESP_OK;
}

const char* wifi_ap_controller_get_ip(void)
{
    return s_ap_ip;
}