/*
 * HTTP Server for ESP32-S3
 * Provides web server functionality for captive portal
 */

#include "http_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "HTTP_SERVER";

static httpd_handle_t s_server = NULL;
static char s_base_path[ESP_VFS_PATH_MAX + 1] = {0};

/* SPIFFS mount status */
static bool spiffs_mounted = false;

/* SPIFFS initialization and deinitialization functions */
esp_err_t http_server_spiffs_init(void)
{
    if (spiffs_mounted) {
        ESP_LOGW(TAG, "SPIFFS already mounted");
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "webroot",  // Matches the name in partitions.csv
        .max_files = 5,               // Maximum number of files that can be open at once
        .format_if_mount_failed = false
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        switch (ret) {
        case ESP_FAIL:
            ESP_LOGE(TAG, "Failed to mount or format SPIFFS");
            break;
        case ESP_ERR_NOT_FOUND:
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
            break;
        case ESP_ERR_INVALID_STATE:
            ESP_LOGE(TAG, "SPIFFS already mounted");
            spiffs_mounted = true;
            ret = ESP_OK;
            break;
        case ESP_ERR_NO_MEM:
            ESP_LOGE(TAG, "Failed to allocate memory for SPIFFS");
            break;
        case ESP_ERR_INVALID_SIZE:
            ESP_LOGE(TAG, "Invalid size for partition");
            break;
        default:
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
            break;
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("webroot", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    spiffs_mounted = true;
    ESP_LOGI(TAG, "SPIFFS initialized successfully");
    return ESP_OK;
}

esp_err_t http_server_spiffs_deinit(void)
{
    if (!spiffs_mounted) {
        ESP_LOGW(TAG, "SPIFFS not mounted");
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_spiffs_unregister("webroot");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unregister SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }

    spiffs_mounted = false;
    ESP_LOGI(TAG, "SPIFFS deinitialized successfully");
    return ESP_OK;
}

/* Max length a file path can have on the storage */
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)

/* Max size of an individual file. Make sure this isn't larger than the biggest chunk
 * that the application can hold.
 */
#define MAX_FILE_SIZE   (200*1024) // 200 KB
#define MAX_FILE_SIZE_STR "200KB"

/* Scratch buffer size */
#define SCRATCH_BUFSIZE  8192

/* Maximum number of tracked client IPs */
#define MAX_CLIENT_IPS 10

/* Client IP storage structure */
typedef struct {
    char ip_addr[16];  // Store IP as string (e.g., "192.168.1.100")
    bool active;
} client_ip_entry_t;

/* Global array to store client IPs */
static client_ip_entry_t s_client_ips[MAX_CLIENT_IPS] = {0};
static SemaphoreHandle_t s_ip_mutex = NULL;

struct file_server_data {
    const char *base_path;
    char scratch[SCRATCH_BUFSIZE];
};


/* Redirect page with meta refresh */
static const char *REDIRECT_PAGE = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"  <meta http-equiv=\"refresh\" content=\"0; url=/\" />"
"</head>"
"<body>"
"  <p>If you are not redirected automatically, <a href=\"/\">click here</a>.</p>"
"</body>"
"</html>";

/* Check if IP exists in the list */
static bool is_client_ip_tracked(const char *ip_addr) {
    if (s_ip_mutex == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(s_ip_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENT_IPS; i++) {
            if (s_client_ips[i].active && strcmp(s_client_ips[i].ip_addr, ip_addr) == 0) {
                xSemaphoreGive(s_ip_mutex);
                return true;
            }
        }
        xSemaphoreGive(s_ip_mutex);
    }
    return false;
}

/* Add IP to the list */
static bool add_client_ip(const char *ip_addr) {
    if (s_ip_mutex == NULL) {
        return false;
    }
    
    if (xSemaphoreTake(s_ip_mutex, portMAX_DELAY) == pdTRUE) {
        // First check if IP already exists
        for (int i = 0; i < MAX_CLIENT_IPS; i++) {
            if (s_client_ips[i].active && strcmp(s_client_ips[i].ip_addr, ip_addr) == 0) {
                xSemaphoreGive(s_ip_mutex);
                return true; // Already exists
            }
        }
        
        // Find an empty slot
        for (int i = 0; i < MAX_CLIENT_IPS; i++) {
            if (!s_client_ips[i].active) {
                strncpy(s_client_ips[i].ip_addr, ip_addr, sizeof(s_client_ips[i].ip_addr) - 1);
                s_client_ips[i].ip_addr[sizeof(s_client_ips[i].ip_addr) - 1] = '\0';
                s_client_ips[i].active = true;
                xSemaphoreGive(s_ip_mutex);
                ESP_LOGI(TAG, "Added new client IP: %s", ip_addr);
                return true;
            }
        }
        
        xSemaphoreGive(s_ip_mutex);
        ESP_LOGW(TAG, "Client IP list is full, cannot add: %s", ip_addr);
    }
    return false;
}

/* Get client IP from request */
static esp_err_t get_client_ip(httpd_req_t *req, char *ip_buf, size_t ip_buf_len) {
    httpd_conn_info_t conn_info;
    memset(&conn_info, 0, sizeof(conn_info));
    conn_info.sockfd = httpd_req_to_sockfd(req);

    esp_err_t err = httpd_get_conn_info(req->handle, &conn_info);
    if (err == ESP_OK) {
        if (conn_info.cli_addr.sa_family == AF_INET) {
            struct sockaddr_in *addr4 = (struct sockaddr_in*)&conn_info.cli_addr;
            inet_ntoa_r(addr4->sin_addr, ip_buf, ip_buf_len);
        } else if (conn_info.cli_addr.sa_family == AF_INET6) {
            // For simplicity, we'll just return an error for IPv6
            return ESP_ERR_NOT_SUPPORTED;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return err;
}

/* Smart captive portal handler that checks client IP */
static esp_err_t smart_captive_portal_handler(httpd_req_t *req) {
    char client_ip[16];
    esp_err_t err = get_client_ip(req, client_ip, sizeof(client_ip));
    
    // Check if this client IP has already visited
    if (err == ESP_OK && is_client_ip_tracked(client_ip)) {
        // IP exists, serve the file normally
        return send_file(req);
    } else {
        // New client, add to list and redirect
        add_client_ip(client_ip);
        
        // Send redirect page
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, REDIRECT_PAGE, strlen(REDIRECT_PAGE));
        return ESP_OK;
    }
}

static const char *get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);
    
    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }
    
    if (base_pathlen + pathlen + 1 > destsize) {
        return NULL;
    }
    
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);
    char *lastslash = strrchr(dest, '/');
    if (lastslash) {
        *(lastslash + 1) = '\0';
    }
    return dest;
}

/* Send HTTP response with the contents of the requested file */
static esp_err_t send_file(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    struct file_server_data *server_data = (struct file_server_data *)(req->user_ctx);
    
    // Get the filename from URL
    const char *filename = get_path_from_uri(filepath, server_data->base_path, 
                                             req->uri, sizeof(filepath)) ;
    
    if (!filename) {
        ESP_LOGE(TAG, "Filename is too long");
        httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "Filename too long");
        return ESP_FAIL;
    }

    // If the filename is empty or ends with '/', serve index.html
    if (strlen(filename) == 0 || (req->uri[strlen(req->uri) - 1] == '/')) {
        strlcpy(filepath, server_data->base_path, sizeof(filepath));
        strlcat(filepath, "/index.html", sizeof(filepath));
    }

    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        // If file not found, return a standard 404 error
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    struct stat file_stat;
    if (fstat(fd, &file_stat) == -1) {
        ESP_LOGE(TAG, "Failed to obtain file statistics");
        close(fd);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Unable to obtain file statistics");
        return ESP_FAIL;
    }

    if (file_stat.st_size > MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "File too large: %ld bytes", file_stat.st_size);
        close(fd);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "File too large");
        return ESP_FAIL;
    }

    // Get the file extension
    const char *ext;
    const char *last_dot = strrchr(filename, '.');
    if (last_dot) {
        ext = last_dot + 1;
    } else {
        ext = "bin"; // default to binary
    }

    // Set content type based on file extension
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) {
        httpd_resp_set_type(req, "text/html");
    } else if (strcmp(ext, "css") == 0) {
        httpd_resp_set_type(req, "text/css");
    } else if (strcmp(ext, "js") == 0) {
        httpd_resp_set_type(req, "application/javascript");
    } else if (strcmp(ext, "json") == 0) {
        httpd_resp_set_type(req, "application/json");
    } else if (strcmp(ext, "png") == 0) {
        httpd_resp_set_type(req, "image/png");
    } else if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) {
        httpd_resp_set_type(req, "image/jpeg");
    } else if (strcmp(ext, "gif") == 0) {
        httpd_resp_set_type(req, "image/gif");
    } else if (strcmp(ext, "svg") == 0) {
        httpd_resp_set_type(req, "image/svg+xml");
    } else if (strcmp(ext, "ico") == 0) {
        httpd_resp_set_type(req, "image/x-icon");
    } else if (strcmp(ext, "xml") == 0) {
        httpd_resp_set_type(req, "text/xml");
    } else if (strcmp(ext, "pdf") == 0) {
        httpd_resp_set_type(req, "application/pdf");
    } else if (strcmp(ext, "zip") == 0) {
        httpd_resp_set_type(req, "application/zip");
    } else if (strcmp(ext, "txt") == 0) {
        httpd_resp_set_type(req, "text/plain");
    } else {
        // Default to binary for all other file types
        httpd_resp_set_type(req, "application/octet-stream");
    }

    // Send file content
    char *chunk = server_data->scratch;
    ssize_t sent = 0;
    do {
        /* Read file in chunks into the scratch buffer */
        int bytes_read = read(fd, chunk, SCRATCH_BUFSIZE);
        if (bytes_read <= 0) {
            break;
        }
        /* Send the buffer contents as HTTP response chunk */
        if (httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK) {
            close(fd);
            ESP_LOGE(TAG, "File sending failed!");
            return ESP_FAIL;
        }
        sent += bytes_read;
    } while (sent < file_stat.st_size);

    /* Close file after sending complete */
    close(fd);
    ESP_LOGI(TAG, "File sent successfully: %s (%ld bytes)", req->uri, sent);
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t http_server_init(const char *base_path)
{
    if (base_path) {
        strncpy(s_base_path, base_path, sizeof(s_base_path));
    } else {
        strncpy(s_base_path, "/spiffs", sizeof(s_base_path));
    }
    
    // Create mutex for protecting client IP list
    s_ip_mutex = xSemaphoreCreateMutex();
    if (s_ip_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create IP mutex");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

esp_err_t http_server_start(int port)
{
    if (s_server != NULL) {
        ESP_LOGE(TAG, "HTTP server already running");
        return ESP_FAIL;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.uri_match_fn = httpd_uri_match_wildcard;
    
    struct file_server_data *server_data = calloc(1, sizeof(struct file_server_data));
    if (!server_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for server data");
        return ESP_ERR_NO_MEM;
    }
    strncpy(server_data->base_path, s_base_path, sizeof(server_data->base_path));

    ESP_LOGI(TAG, "Starting HTTP Server on port: %d", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        free(server_data);
        return ESP_FAIL;
    }

    // URI handler for getting files
    httpd_uri_t file_download = {
        .uri       = "/*",  // Match all URIs of type /path/to/file
        .method    = HTTP_GET,
        .handler   = send_file,
        .user_ctx  = server_data    // Pass server data as context
    };
    httpd_register_uri_handler(s_server, &file_download);

    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;
    
    // Clean up mutex
    if (s_ip_mutex != NULL) {
        vSemaphoreDelete(s_ip_mutex);
        s_ip_mutex = NULL;
    }
    
    // Reset client IP list
    memset(s_client_ips, 0, sizeof(s_client_ips));
    
    return ret;
}


esp_err_t http_server_register_smart_captive_portal(void)
{
    if (s_server == NULL) {
        ESP_LOGE(TAG, "HTTP server not initialized");
        return ESP_FAIL;
    }

    // Register a catch-all handler for the smart captive portal
    httpd_uri_t smart_captive_handler = {
        .uri       = "/*",  // Match all URIs
        .method    = HTTP_GET,
        .handler   = smart_captive_portal_handler,
        .user_ctx  = NULL
    };
    
    // Unregister the existing wildcard handler first
    httpd_unregister_uri_handler(s_server, "/*", HTTP_GET);
    
    // Register the smart captive portal handler
    esp_err_t ret = httpd_register_uri_handler(s_server, &smart_captive_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register smart captive portal handler: %s", esp_err_to_name(ret));
    }
    
    return ret;
}
