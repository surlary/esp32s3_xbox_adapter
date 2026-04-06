idf_component_register(
    SRCS "esp_hid_host_main.c" "esp_hid_gap.c" "hid_ps4_driver.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_hid mbedtls esp_timer
    PRIV_REQUIRES bt esp_hid driver esp_tinyusb led_strip nvs_flash
    EMBED_TXTFILES "key.pem" "serial.txt" "sig.bin"
)
