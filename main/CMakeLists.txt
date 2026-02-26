set(srcs 
        "esp_hid_host_main.c"
        "esp_hid_gap.c"
        "hid_ps4_driver.c"
        "wifi_ap_controller.c"
        "http_server.c"
		)
set(include_dirs ".")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "${include_dirs}"
                       REQUIRES driver esp_hid mbedtls esp_timer
                       PRIV_REQUIRES bt 
        usb
        esp_hid
        esp_http_server
        esp_wifi
        esp_netif
        nvs_flash
        driver
        esp_common
        esp_timer
        freertos
        lwip
        spiffs
		       EMBED_TXTFILES "key.pem" "serial.txt" "sig.bin") 
        
spiffs_create_partition_image(webroot ../webroot)
