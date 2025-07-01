#include <stdio.h>
#include "ade7953.h"
#include "led.h"
#include "network.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define ENABLE_MQTT_LOGGING
#define ENABLE_MEASUREMENT_PUBLISHING

static const char *TAG = "main";

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("network", ESP_LOG_DEBUG);
    esp_log_level_set("main", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "Starting Open Grid Frequency Monitor");

    // Initialize NVS for WiFi
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    
    // Initialize LED handle
    led_handle_t led_handle;
    
    // Initialize the LED system
    led_error_t led_ret = led_init(&led_handle);
    if (led_ret != LED_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED: %d", led_ret);
    } else {
        ESP_LOGI(TAG, "LED initialized successfully");
        // Set status to initializing
        led_set_status(&led_handle, LED_STATUS_INITIALIZING);
    }
    
    
    // Initialize ADE7953 handle
    ade7953_handle_t ade7953_handle;
    
    // Initialize the ADE7953 driver
    ade7953_error_t ret = ade7953_init(&ade7953_handle);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Failed to initialize ADE7953: %d", ret);
        if (led_ret == LED_OK) {
            led_set_status(&led_handle, LED_STATUS_ERROR);
        }
        return;
    }
    
    ESP_LOGI(TAG, "ADE7953 initialized successfully");
    
    // Start the background task for continuous readings
    ret = ade7953_start_task(&ade7953_handle);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Failed to start ADE7953 task: %d", ret);
        if (led_ret == LED_OK) {
            led_set_status(&led_handle, LED_STATUS_ERROR);
        }
        ade7953_deinit(&ade7953_handle);
        return;
    }
    
    ESP_LOGI(TAG, "ADE7953 background task started");
    
    // Initialize network handle
    network_handle_t network_handle;

    // Initialize network
    esp_err_t net_ret = network_init(&network_handle, &led_handle, &ade7953_handle);
    if (net_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize network: %s", esp_err_to_name(net_ret));
        if (led_ret == LED_OK) {
            led_set_status(&led_handle, LED_STATUS_ERROR);
        }
    } else {
        ESP_LOGI(TAG, "Network initialized successfully");
        
        // Check OTA rollback status early in startup
        esp_err_t rollback_ret = network_check_ota_rollback();
        if (rollback_ret != ESP_OK) {
            ESP_LOGW(TAG, "OTA rollback check failed");
        }
        
        // Start WiFi
        net_ret = network_start_wifi(&network_handle);
        if (net_ret == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected successfully to %s", WIFI_SSID);
            ESP_LOGI(TAG, "IP Address: %s", network_get_ip_address(&network_handle));
            
            // Initialize SNTP for time synchronization
            esp_err_t sntp_ret = network_init_sntp();
            if (sntp_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to synchronize time via SNTP");
            }

            // Start web server
            net_ret = network_start_web_server(&network_handle);
            if (net_ret == ESP_OK) {
                ESP_LOGI(TAG, "Web server started successfully");
            } else {
                ESP_LOGW(TAG, "Failed to start web server");
            }
            
            // Start MQTT logging
            #ifdef ENABLE_MQTT_LOGGING
            net_ret = network_start_mqtt_logging(&network_handle);
            if (net_ret == ESP_OK) {
                ESP_LOGI(TAG, "MQTT logging started successfully");
                
                // Start MQTT command handling
                net_ret = network_start_mqtt_commands(&network_handle);
                if (net_ret == ESP_OK) {
                    ESP_LOGI(TAG, "MQTT command handling started successfully");
                } else {
                    ESP_LOGW(TAG, "Failed to start MQTT command handling");
                }
                
                // Start measurement publishing
                #ifdef ENABLE_MEASUREMENT_PUBLISHING
                net_ret = network_start_measurement_publishing(&network_handle);
                if (net_ret == ESP_OK) {
                    ESP_LOGI(TAG, "MQTT measurement publishing started successfully");
                } else {
                    ESP_LOGW(TAG, "Failed to start MQTT measurement publishing");
                }
                #endif
            } else {
                ESP_LOGW(TAG, "Failed to start MQTT logging");
            }
            #endif
            
            if (led_ret == LED_OK) {
                led_set_status(&led_handle, LED_STATUS_READY);
            }
        } else {
            ESP_LOGE(TAG, "Failed to connect to WiFi");
            if (led_ret == LED_OK) {
                led_set_status(&led_handle, LED_STATUS_WARNING);
            }
        }
    }
    
    // Set the measurement queue for automatic measurement publishing
    ade7953_set_measurement_queue(&ade7953_handle, network_get_measurement_queue(&network_handle));
    
    // Start LED pattern task for dynamic patterns
    led_set_status(&led_handle, LED_STATUS_WORKING);
    
    // Main loop - process readings and publish via MQTT
    uint32_t reading_count = 0;
    bool last_network_connected = false;
    
    uint32_t loop_count = 0;
    while (true) {        
        bool network_connected = network_is_connected(&network_handle);
        
        if (network_connected != last_network_connected) {
            if (network_connected) {
                ESP_LOGI(TAG, "Network connection established");
            } else {
                ESP_LOGW(TAG, "Network connection lost");
            }
        }
        
        last_network_connected = network_connected;
        reading_count++;

        loop_count++;
        if (loop_count % 10 == 0) {
            float frequency = ade7953_get_latest_frequency(&ade7953_handle);
            float voltage = ade7953_get_latest_voltage(&ade7953_handle);
            ESP_LOGI(TAG, "Frequency: %.3f Hz | Voltage: %.1f V", frequency, voltage);
        }

        // Wait before next reading (1 second for status monitoring)
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
