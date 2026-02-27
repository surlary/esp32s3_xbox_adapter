/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "tinyusb.h"
#include "hid_ps4_driver.h"

#include "esp_hidh.h"
#include "esp_hid_gap.h"
#include "esp_timer.h"

// LED Strip
#include "led_strip.h"
#include "driver/gpio.h"

// New includes for AP mode and captive portal
#include "wifi_ap_controller.h"
#include "http_server.h"

// SPIFFS header for initialization
#include "esp_spiffs.h"


static const char *TAG = "xbox";

#define XBOX_CONTROLLER_INDEX_BUTTONS_DIR 12
#define XBOX_CONTROLLER_INDEX_BUTTONS_MAIN 13
#define XBOX_CONTROLLER_INDEX_BUTTONS_CENTER 14
#define XBOX_CONTROLLER_INDEX_BUTTONS_SHARE 15

// GPIO for WS2812 LED
#define LED_GPIO 48
#define LED_NUM 1
#define LED_BRIGHTNESS_FACTOR 0.5  // 50% brightness

// GPIO for boot button
#define BOOT_BUTTON_GPIO 0

// Battery level ranges for color mapping
typedef enum {
    BATTERY_LEVEL_ORANGE = 0,   // [0, 5]
    BATTERY_LEVEL_PURPLE,       // (5, 25]
    BATTERY_LEVEL_RED,          // (25, 45]
    BATTERY_LEVEL_YELLOW,       // (45, 65]
    BATTERY_LEVEL_BLUE,         // (65, 85]
    BATTERY_LEVEL_GREEN         // (85, 100]
} battery_level_t;

// Global variables for LED control
static led_strip_handle_t led_strip;
static bool led_initialized = false;
static battery_level_t last_battery_level = -1;  // Initialize to invalid value
static bool first_battery_event = true;  // Track if it's the first battery event
static volatile bool led_trigger_pending = false;  // Flag to trigger LED from ISR/task (volatile for ISR safety)
static volatile int64_t last_button_press_time = 0;  // Timestamp for debouncing (volatile for ISR safety)

// Timer for LED control
static TimerHandle_t led_timer = NULL;

// Button debounce time in microseconds (50ms)
#define BUTTON_DEBOUNCE_US 500000

//static const uint16_t maxJoy = 0xffff;

//int64_t timer = 0;


hid_ps4_report_t ps4_report = {};


// Function to determine battery level based on percentage
battery_level_t get_battery_level(uint8_t battery_percentage) {
    // Clamp battery percentage to valid range [0, 100]
    if (battery_percentage > 100) {
        battery_percentage = 100;
    }
    
    if (battery_percentage <= 5) {
        return BATTERY_LEVEL_ORANGE;
    } else if (battery_percentage <= 25) {
        return BATTERY_LEVEL_PURPLE;
    } else if (battery_percentage <= 45) {
        return BATTERY_LEVEL_RED;
    } else if (battery_percentage <= 65) {
        return BATTERY_LEVEL_YELLOW;
    } else if (battery_percentage <= 85) {
        return BATTERY_LEVEL_BLUE;
    } else {
        return BATTERY_LEVEL_GREEN;
    }
}

// Function to get RGB values for each battery level with reduced brightness
void get_rgb_for_battery_level(battery_level_t level, uint32_t *red, uint32_t *green, uint32_t *blue) {
    // Full brightness values
    switch (level) {
        case BATTERY_LEVEL_ORANGE:
            *red = 255 * LED_BRIGHTNESS_FACTOR;   // Orange: (255, 165, 0) -> (128, 82, 0)
            *green = 165 * LED_BRIGHTNESS_FACTOR;
            *blue = 0 * LED_BRIGHTNESS_FACTOR;
            break;
        case BATTERY_LEVEL_PURPLE:
            *red = 128 * LED_BRIGHTNESS_FACTOR;   // Purple: (128, 0, 128) -> (64, 0, 64)
            *green = 0 * LED_BRIGHTNESS_FACTOR;
            *blue = 128 * LED_BRIGHTNESS_FACTOR;
            break;
        case BATTERY_LEVEL_RED:
            *red = 255 * LED_BRIGHTNESS_FACTOR;   // Red: (255, 0, 0) -> (128, 0, 0)
            *green = 0 * LED_BRIGHTNESS_FACTOR;
            *blue = 0 * LED_BRIGHTNESS_FACTOR;
            break;
        case BATTERY_LEVEL_YELLOW:
            *red = 255 * LED_BRIGHTNESS_FACTOR;   // Yellow: (255, 255, 0) -> (128, 128, 0)
            *green = 255 * LED_BRIGHTNESS_FACTOR;
            *blue = 0 * LED_BRIGHTNESS_FACTOR;
            break;
        case BATTERY_LEVEL_BLUE:
            *red = 0 * LED_BRIGHTNESS_FACTOR;     // Blue: (0, 0, 255) -> (0, 0, 128)
            *green = 0 * LED_BRIGHTNESS_FACTOR;
            *blue = 255 * LED_BRIGHTNESS_FACTOR;
            break;
        case BATTERY_LEVEL_GREEN:
            *red = 0 * LED_BRIGHTNESS_FACTOR;     // Green: (0, 255, 0) -> (0, 128, 0)
            *green = 255 * LED_BRIGHTNESS_FACTOR;
            *blue = 0 * LED_BRIGHTNESS_FACTOR;
            break;
        default:
            // Default to red if invalid level
            *red = 255 * LED_BRIGHTNESS_FACTOR;
            *green = 0 * LED_BRIGHTNESS_FACTOR;
            *blue = 0 * LED_BRIGHTNESS_FACTOR;
            break;
    }
    
    // Round to nearest integer
    *red = (uint32_t)(*red + 0.5);
    *green = (uint32_t)(*green + 0.5);
    *blue = (uint32_t)(*blue + 0.5);
}

// LED timer callback to turn off the LED after 300ms
void led_timer_callback(TimerHandle_t xTimer) {
    if (led_initialized && led_strip != NULL) {
        // Turn off the LED
        esp_err_t err = led_strip_set_pixel(led_strip, 0, 0, 0, 0);
        if (err == ESP_OK) {
            led_strip_refresh(led_strip);
        }
    }
}

// Function to trigger LED with specific color for 300ms
void trigger_led_with_color(uint32_t red, uint32_t green, uint32_t blue) {
    if (led_initialized && led_strip != NULL && led_timer != NULL) {
        // Set the LED to the specified color
        esp_err_t err = led_strip_set_pixel(led_strip, 0, (uint8_t)red, (uint8_t)green, (uint8_t)blue);
        if (err == ESP_OK) {
            led_strip_refresh(led_strip);
        }
        
        // Restart the timer to turn off the LED after 300ms
        xTimerStop(led_timer, 0);
        xTimerStart(led_timer, 0);
    }
}



// Function to handle battery level change
void handle_battery_change(uint8_t battery_percentage) {
    battery_level_t current_level = get_battery_level(battery_percentage);
    
    // Check if this is the first battery event or if the level has changed
    if (first_battery_event || current_level != last_battery_level) {
        // Update the last level
        last_battery_level = current_level;
        first_battery_event = false;
        
        // Get the RGB values for the current level
        uint32_t red, green, blue;
        get_rgb_for_battery_level(current_level, &red, &green, &blue);
        
        // Trigger the LED with the new color
        trigger_led_with_color(red, green, blue);
    }
}


void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event)
    {
    case ESP_HIDH_OPEN_EVENT:
    {
        if (param->open.status == ESP_OK)
        {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            ESP_LOGI(TAG, ESP_BD_ADDR_STR " OPEN: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->open.dev));
            esp_hidh_dev_dump(param->open.dev, stdout);
        }
        else
        {
            ESP_LOGE(TAG, " OPEN failed!");
        }
        break;
    }
    case ESP_HIDH_BATTERY_EVENT:
    {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->battery.dev);
        ESP_LOGI(TAG, ESP_BD_ADDR_STR " BATTERY: %d%%", ESP_BD_ADDR_HEX(bda), param->battery.level);
        
        // Handle battery level change
        handle_battery_change(param->battery.level);
        
        break;
    }
    case ESP_HIDH_INPUT_EVENT:
    {
        //const uint8_t *bda = esp_hidh_dev_bda_get(param->input.dev);
        // ESP_LOGI(TAG, ESP_BD_ADDR_STR " INPUT: %8s, MAP: %2u, ID: %3u, Len: %d, Data:", ESP_BD_ADDR_HEX(bda), esp_hid_usage_str(param->input.usage), param->input.map_index, param->input.report_id, param->input.length);
        // ESP_LOG_BUFFER_HEX(TAG, param->input.data, param->input.length);
        // ESP_LOGI(TAG, "id:%3u", param->input.report_id);

        // 1.25 ms
        // if (timer + 1250 > esp_timer_get_time())
        //{
        //     break;
        //}

        memset(&ps4_report, 0, sizeof(ps4_report));
        ps4_report.report_id = 0x1;

        /*
        bool btnA, btnB, btnX, btnY;
        bool btnShare, btnStart, btnSelect, btnXbox;
        // side top button
        bool btnLB, btnRB;
        // button on joy stick
        bool btnLS, btnRS;
       
        
        uint16_t joyLHori = maxJoy / 2;
        uint16_t joyLVert = maxJoy / 2;
        uint16_t joyRHori = maxJoy / 2;
        uint16_t joyRVert = maxJoy / 2;
        */
        bool btnDirUp, btnDirLeft, btnDirRight, btnDirDown;
        uint16_t joyLHori ;
        uint16_t joyLVert ;
        uint16_t joyRHori ;
        uint16_t joyRVert;
        uint16_t trigLT, trigRT;

        uint8_t btnBits;
        btnBits = param->input.data[XBOX_CONTROLLER_INDEX_BUTTONS_MAIN];

        if (btnBits & 0b00000001) // btnA
            ps4_report.buttons1 |= (1 << 5);

        if (btnBits & 0b00000010) // btnB
            ps4_report.buttons1 |= (1 << 6);

        if (btnBits & 0b00001000) // btnX
            ps4_report.buttons1 |= (1 << 4);

        if (btnBits & 0b00010000) // btnY
            ps4_report.buttons1 |= (1 << 7);

        if (btnBits & 0b01000000) // btnLB
            ps4_report.buttons2 |= (1 << 0);

        if (btnBits & 0b10000000) // btnRB
            ps4_report.buttons2 |= (1 << 1);

        /*
        btnA = btnBits & 0b00000001;
        btnB = btnBits & 0b00000010;
        btnX = btnBits & 0b00001000;
        btnY = btnBits & 0b00010000;
        btnLB = btnBits & 0b01000000;
        btnRB = btnBits & 0b10000000;
        */

        btnBits = param->input.data[XBOX_CONTROLLER_INDEX_BUTTONS_CENTER];

        if (btnBits & 0b00000100) // Select
            ps4_report.buttons2 |= (1 << 4);

        if (btnBits & 0b00001000) // Start
            ps4_report.buttons2 |= (1 << 5);

        

        if (btnBits & 0b00010000) // Xbox
            ps4_report.buttons3 |= (1 << 0);

        if (btnBits & 0b00100000) // LS
            ps4_report.buttons2 |= (1 << 6);

        if (btnBits & 0b01000000) // RS
            ps4_report.buttons2 |= (1 << 7);

 /*
        btnSelect = btnBits & 0b00000100;
        btnStart = btnBits & 0b00001000;
        btnXbox = btnBits & 0b00010000;
        btnLS = btnBits & 0b00100000;
        btnRS = btnBits & 0b01000000;
        */

        btnBits = param->input.data[XBOX_CONTROLLER_INDEX_BUTTONS_SHARE]; // btnShare

        if (btnBits & 0b00000001)
            ps4_report.buttons3 |= (1 << 1);

        /*
        btnShare = btnBits & 0b00000001;
        */

        btnBits = param->input.data[XBOX_CONTROLLER_INDEX_BUTTONS_DIR];
        btnDirUp = btnBits == 1 || btnBits == 2 || btnBits == 8;
        btnDirRight = 2 <= btnBits && btnBits <= 4;
        btnDirDown = 4 <= btnBits && btnBits <= 6;
        btnDirLeft = 6 <= btnBits && btnBits <= 8;
        

        if (btnDirUp && btnDirRight)
        {

            ps4_report.buttons1 |= 0x01;
        }
        else if (btnDirDown && btnDirLeft)
        {

            ps4_report.buttons1 |= 0x05;
        }
        else if (btnDirDown && btnDirRight)
        {

            ps4_report.buttons1 |= 0x03;
        }
        else if (btnDirUp && btnDirLeft)
        {

            ps4_report.buttons1 |= 0x07;
        }
        else if (btnDirUp)
        {

            ps4_report.buttons1 |= 0x00;
        }
        else if (btnDirRight)
        {

            ps4_report.buttons1 |= 0x02;
        }
        else if (btnDirDown)
        {

            ps4_report.buttons1 |= 0x04;
        }
        else if (btnDirLeft)
        {

            ps4_report.buttons1 |= 0x06;
        }
        else
        {

            ps4_report.buttons1 |= 0x08;
        }

        joyLHori = (uint16_t)param->input.data[0] | ((uint16_t)param->input.data[1] << 8); // 0-65535
        joyLVert = (uint16_t)param->input.data[2] | ((uint16_t)param->input.data[3] << 8);
        joyRHori = (uint16_t)param->input.data[4] | ((uint16_t)param->input.data[5] << 8);
        joyRVert = (uint16_t)param->input.data[6] | ((uint16_t)param->input.data[7] << 8);

        trigLT = (uint16_t)param->input.data[8] | ((uint16_t)param->input.data[9] << 8); // 0-1024
        trigRT = (uint16_t)param->input.data[10] | ((uint16_t)param->input.data[11] << 8);

        if (trigLT)
            ps4_report.buttons2 |= (1 << 2);

        if (trigRT)
            ps4_report.buttons2 |= (1 << 3);

        ps4_report.lt = trigLT / 4;
        ps4_report.rt = trigRT / 4;

        ps4_report.lx = joyLHori / 256;
        ps4_report.ly = joyLVert / 256;

        ps4_report.rx = joyRHori / 256;
        ps4_report.ry = joyRVert / 256;

       //ps4_report.timestamp = last_timestamp;

        // printf("A:%01x B:%01x X:%01x Y:%01x LB:%01x RB:%01x Select:%01x Start:%01x Xbox:%01x LS:%01x RS:%01x Share:%01x\n", btnA, btnB, btnX, btnY, btnLB, btnRB, btnSelect, btnStart, btnXbox, btnLS, btnRS, btnShare);

        // printf("lx:%04x ly:%04x rx:%04x ry:%04x LT:%04x RT:%01x \n", joyLHori, joyLVert, joyRHori, joyRVert, trigLT, trigRT);

        // memcpy(last_buf, param->input.data, 16);
        // timer = esp_timer_get_time();

        ps4_report.battery = 0 | (1 << 4) | 11;

        ps4_report.gyrox = 0;
        ps4_report.gyroy = 0;
        ps4_report.gyroz = 0;
        ps4_report.accelx = 0;
        ps4_report.accely = 0;
        ps4_report.accelz = 0;

        ps4_report.extension = 0x01;

        ps4_report.touchpad_event_active = 0;
        ps4_report.touchpad_counter = 0;
        ps4_report.touchpad1_touches = (1 << 7);
        ps4_report.touchpad2_touches = (1 << 7);

        ps4_report.unknown3[1] = 0x80;
        ps4_report.unknown3[5] = 0x80;
        ps4_report.unknown3[10] = 0x80;
        ps4_report.unknown3[14] = 0x80;
        ps4_report.unknown3[19] = 0x80;

       
        send_hid_ps4_report(&ps4_report);

        break;
    }
    case ESP_HIDH_FEATURE_EVENT:
    {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->feature.dev);
        ESP_LOGI(TAG, ESP_BD_ADDR_STR " FEATURE: %8s, MAP: %2u, ID: %3u, Len: %d", ESP_BD_ADDR_HEX(bda),
                 esp_hid_usage_str(param->feature.usage), param->feature.map_index, param->feature.report_id,
                 param->feature.length);
        ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
        break;
    }
    case ESP_HIDH_CLOSE_EVENT:
    {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->close.dev);
        ESP_LOGI(TAG, ESP_BD_ADDR_STR " CLOSE: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->close.dev));
        break;
    }
    default:
        ESP_LOGI(TAG, "EVENT: %d", event);
        break;
    }
}

#define SCAN_DURATION_SECONDS 5

// Task to handle AP mode when no HID devices are found
void ap_mode_task(void *pvParameters)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "No HID devices found, starting AP mode...");
    
    // Initialize SPIFFS before starting HTTP server
    ret = http_server_spiffs_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS: %s", esp_err_to_name(ret));
        // Continue anyway, as the system might still work without SPIFFS
    } else {
        ESP_LOGI(TAG, "SPIFFS initialized successfully");
    }
    
    // Initialize and start WiFi AP
    ret = wifi_ap_controller_init("PS5JB", NULL, "192.168.123.1");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi AP: %s", esp_err_to_name(ret));
        // Clean up SPIFFS if initialized
        http_server_spiffs_deinit();
        vTaskDelete(NULL);
        return;
    }
    
    ret = wifi_ap_controller_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi AP: %s", esp_err_to_name(ret));
        // Clean up SPIFFS if initialized
        http_server_spiffs_deinit();
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "WiFi AP started with IP: %s", wifi_ap_controller_get_ip());
    
    // Initialize and start HTTP server
    ret = http_server_init("/spiffs");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize HTTP server");
        // Clean up SPIFFS if initialized
        http_server_spiffs_deinit();
        vTaskDelete(NULL);
        return;
    }
    
    ret = http_server_start(80);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        // Clean up SPIFFS if initialized
        http_server_spiffs_deinit();
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "HTTP server started on port 80");
    
    // Register smart captive portal handler
    ret = http_server_register_smart_captive_portal();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register smart captive portal handler");
    } else {
        ESP_LOGI(TAG, "Smart captive portal registered");
    }
    
    // Stay in AP mode indefinitely
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void hid_task(void *pvParameters)
{
    size_t results_len = 0;
    esp_hid_scan_result_t *results = NULL;
    ESP_LOGI(TAG, "SCAN...");
    // start scan for HID devices
    esp_hid_scan(SCAN_DURATION_SECONDS, &results_len, &results);
    ESP_LOGI(TAG, "SCAN: %u results", results_len);
    if (results_len)
    {
        esp_hid_scan_result_t *r = results;
        esp_hid_scan_result_t *cr = NULL;
        while (r)
        {
            printf("  %s: " ESP_BD_ADDR_STR ", ", (r->transport == ESP_HID_TRANSPORT_BLE) ? "BLE" : "BT ", ESP_BD_ADDR_HEX(r->bda));
            printf("RSSI: %d, ", r->rssi);
            printf("USAGE: %s, ", esp_hid_usage_str(r->usage));

            if (r->transport == ESP_HID_TRANSPORT_BLE)
            {
                cr = r;
                printf("APPEARANCE: 0x%04x, ", r->ble.appearance);
                printf("ADDR_TYPE: '%s', ", ble_addr_type_str(r->ble.addr_type));
            }

            printf("NAME: %s ", r->name ? r->name : "");
            printf("\n");
            r = r->next;
        }
        if (cr)
        {
            // open the last result
            esp_hidh_dev_open(cr->bda, cr->transport, cr->ble.addr_type);
        }
        // free the results
        esp_hid_scan_results_free(results);

        if (!cr) {
            ESP_LOGI(TAG, "No HID devices found, switching to AP mode");
            xTaskCreate(&ap_mode_task, "ap_mode_task", 8 * 1024, NULL, 5, NULL);
        }
    }
    else
    {
        // No devices found, start AP mode
        ESP_LOGI(TAG, "No HID devices found, switching to AP mode");
        xTaskCreate(&ap_mode_task, "ap_mode_task", 8 * 1024, NULL, 5, NULL);
    }
    vTaskDelete(NULL);
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
    //ESP_LOGI("PS4_DRV", "tud_hid_set_report_cb");

    hid_ps4_set_report_cb(itf, report_id, report_type, buffer, bufsize);
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    //ESP_LOGI("PS4_DRV", "tud_hid_get_report_cb");

    return hid_ps4_get_report_cb(itf, report_id, report_type, buffer, reqlen);
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf)
{

    return ps4_desc_hid_report;
}

// GPIO interrupt handler for boot button with debouncing
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    if (gpio_num == BOOT_BUTTON_GPIO) {
        // Get current time in microseconds
        int64_t current_time = esp_timer_get_time();
        
        // Check if enough time has passed since last button press (debouncing)
        if ((current_time - last_button_press_time) > BUTTON_DEBOUNCE_US) {
            last_button_press_time = current_time;
            // Set flag to true (single direction: ISR only sets, task only clears)
            led_trigger_pending = true;
        }
    }
}

// Initialize LED strip
esp_err_t init_led_strip(void)
{
    // Reset LED state
    led_initialized = false;
    led_strip = NULL;
    
    // LED strip general initialization, according to your led board design
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,   // The GPIO that connected to the LED strip's data line
        .max_leds = LED_NUM,          // The number of LEDs in the strip,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB, // Pixel format of your LED strip
        .led_model = LED_MODEL_WS2812, // LED strip model
        .flags.invert_out = false,    // whether to invert the output signal
    };

    // LED strip backend driver initialization
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,        // different clock source can lead to different power consumption
        .resolution_hz = 10 * 1000 * 1000,   // 10MHz resolution, 1 tick = 0.1us,
        .flags.with_dma = false,              // whether to enable the DMA feature
    };

    // Initialize the RMT channel to drive the LED strip
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    // Clear the LED strip (turn off all LEDs)
    ret = led_strip_clear(led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear LED strip: %s", esp_err_to_name(ret));
        return ret;
    }
    
    led_initialized = true;
    ESP_LOGI(TAG, "LED strip initialized successfully on GPIO %d", LED_GPIO);
    
    return ESP_OK;
}

// Task to handle LED updates from button press
void led_button_task(void *pvParameters)
{
    while (1) {
        // Check the trigger flag
        if (led_trigger_pending && led_initialized && last_battery_level != -1) {
            // Clear flag with critical section to prevent race condition
            portDISABLE_INTERRUPTS();
            led_trigger_pending = false;
            portENABLE_INTERRUPTS();
            
            uint32_t red, green, blue;
            get_rgb_for_battery_level(last_battery_level, &red, &green, &blue);
            trigger_led_with_color(red, green, blue);
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Initialize boot button
esp_err_t init_boot_button(void)
{
    // Configure the GPIO as input
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // Enable pull-up resistor
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE  // Trigger on falling edge (button pressed)
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure boot button GPIO: %s", esp_err_to_name(ret));
        return ret;
    }

    // Install GPIO ISR service
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        return ret;
    }

    // Add ISR handler for the boot button
    ret = gpio_isr_handler_add(BOOT_BUTTON_GPIO, gpio_isr_handler, (void*)BOOT_BUTTON_GPIO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Boot button initialized on GPIO %d", BOOT_BUTTON_GPIO);
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Starting Xbox to PS4/PC HID converter");

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize LED strip first
    ret = init_led_strip();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED strip, continuing without LED support");
        // Continue without LED support
        led_initialized = false;
    }
    
    // Create the timer for LED control (300ms timeout)
    led_timer = xTimerCreate("led_timer", pdMS_TO_TICKS(300), pdFALSE, (void*)0, led_timer_callback);
    if (led_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create LED timer");
        // Continue without timer support
    }

    // Initialize boot button
    ret = init_boot_button();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize boot button, continuing without button support");
        // Continue without button support
    }
    
    // Create LED button task to handle button-triggered LED updates
    xTaskCreate(&led_button_task, "led_button_task", 2048, NULL, 5, NULL);

    ps4_driver_init();

    ESP_LOGI(TAG, "USB initialization");

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = (const tusb_desc_device_t *)ds4_desc_device,
        .string_descriptor = ps4_string_descriptors,
        .string_descriptor_count = 4,
        .external_phy = false, // In the most cases you need to use a `false` value
        .configuration_descriptor = ps4_desc_cfg,
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    ESP_ERROR_CHECK(esp_hid_gap_init(HIDH_BLE_MODE));

    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&config));

    xTaskCreate(&hid_task, "hid_task", 6 * 1024, NULL, 10, NULL);

    ESP_LOGI(TAG, "System initialization completed");
}
