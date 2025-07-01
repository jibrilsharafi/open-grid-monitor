#include "led.h"
#include <string.h>
#include <math.h>

static const char *TAG = "led";

// Forward declaration of pattern task
static void led_pattern_task(void *pvParameters);

// Internal function to update LED hardware
static led_error_t led_update_hardware(led_handle_t *handle, led_color_t color, uint8_t brightness) {
    if (!handle || !handle->initialized) {
        return LED_ERROR_INIT;
    }
    
    // Calculate brightness-adjusted values
    uint32_t red_duty = (color.red * brightness) / LED_MAX_BRIGHTNESS;
    uint32_t green_duty = (color.green * brightness) / LED_MAX_BRIGHTNESS;
    uint32_t blue_duty = (color.blue * brightness) / LED_MAX_BRIGHTNESS;
    
    // Update PWM duty cycles
    esp_err_t ret;
    ret = ledc_set_duty(LED_MODE, LED_RED_CHANNEL, red_duty);
    if (ret != ESP_OK) return LED_ERROR_INIT;
    
    ret = ledc_set_duty(LED_MODE, LED_GREEN_CHANNEL, green_duty);
    if (ret != ESP_OK) return LED_ERROR_INIT;
    
    ret = ledc_set_duty(LED_MODE, LED_BLUE_CHANNEL, blue_duty);
    if (ret != ESP_OK) return LED_ERROR_INIT;
    
    // Update all channels
    ledc_update_duty(LED_MODE, LED_RED_CHANNEL);
    ledc_update_duty(LED_MODE, LED_GREEN_CHANNEL);
    ledc_update_duty(LED_MODE, LED_BLUE_CHANNEL);
    
    return LED_OK;
}

// Configure LEDC for RGB LED
static led_error_t led_configure_pwm(void) {
    // Configure LEDC timer
    ledc_timer_config_t timer_config = {
        .speed_mode = LED_MODE,
        .timer_num = LED_TIMER,
        .duty_resolution = LED_RESOLUTION,
        .freq_hz = LED_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK
    };
    
    esp_err_t ret = ledc_timer_config(&timer_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer: %s", esp_err_to_name(ret));
        return LED_ERROR_INIT;
    }
    
    // Configure red channel
    ledc_channel_config_t red_config = {
        .speed_mode = LED_MODE,
        .channel = LED_RED_CHANNEL,
        .timer_sel = LED_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_RED_PIN,
        .duty = 0,
        .hpoint = 0
    };
    
    ret = ledc_channel_config(&red_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure red LED channel: %s", esp_err_to_name(ret));
        return LED_ERROR_INIT;
    }
    
    // Configure green channel
    ledc_channel_config_t green_config = {
        .speed_mode = LED_MODE,
        .channel = LED_GREEN_CHANNEL,
        .timer_sel = LED_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_GREEN_PIN,
        .duty = 0,
        .hpoint = 0
    };
    
    ret = ledc_channel_config(&green_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure green LED channel: %s", esp_err_to_name(ret));
        return LED_ERROR_INIT;
    }
    
    // Configure blue channel
    ledc_channel_config_t blue_config = {
        .speed_mode = LED_MODE,
        .channel = LED_BLUE_CHANNEL,
        .timer_sel = LED_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LED_BLUE_PIN,
        .duty = 0,
        .hpoint = 0
    };
    
    ret = ledc_channel_config(&blue_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure blue LED channel: %s", esp_err_to_name(ret));
        return LED_ERROR_INIT;
    }
    
    return LED_OK;
}

// Pattern task implementation
static void led_pattern_task(void *pvParameters) {
    led_handle_t *handle = (led_handle_t *)pvParameters;
    led_color_t off_color = LED_COLOR_OFF;
    uint32_t cycle_count = 0;
    
    ESP_LOGI(TAG, "LED pattern task started");
    
    while (true) {
        if (!handle->current_config.enabled) {
            led_update_hardware(handle, off_color, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        switch (handle->current_config.pattern) {
            case LED_PATTERN_SOLID:
                led_update_hardware(handle, handle->current_config.color, handle->current_config.brightness);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
                
            case LED_PATTERN_BLINK_SLOW:
                if (cycle_count % 20 < 10) { // 1 second on, 1 second off (20 * 100ms = 2 seconds total)
                    led_update_hardware(handle, handle->current_config.color, handle->current_config.brightness);
                } else {
                    led_update_hardware(handle, off_color, 0);
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                cycle_count++;
                if (cycle_count >= 20) cycle_count = 0;
                break;
                
            case LED_PATTERN_BLINK_FAST:
                if (cycle_count % 5 < 2) { // 250ms on, 250ms off (5 * 100ms = 500ms total)
                    led_update_hardware(handle, handle->current_config.color, handle->current_config.brightness);
                } else {
                    led_update_hardware(handle, off_color, 0);
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                cycle_count++;
                if (cycle_count >= 5) cycle_count = 0;
                break;
                
            case LED_PATTERN_PULSE_SLOW:
            case LED_PATTERN_PULSE_FAST: {
                uint32_t pulse_period = (handle->current_config.pattern == LED_PATTERN_PULSE_SLOW) ? 40 : 20;
                uint8_t brightness = (uint8_t)((sin(2.0 * M_PI * cycle_count / pulse_period) + 1.0) * handle->current_config.brightness / 2);
                led_update_hardware(handle, handle->current_config.color, brightness);
                vTaskDelay(pdMS_TO_TICKS(50));
                cycle_count++;
                if (cycle_count >= pulse_period) cycle_count = 0;
                break;
            }
                
            default:
                led_update_hardware(handle, handle->current_config.color, handle->current_config.brightness);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}

// Initialize LED system
led_error_t led_init(led_handle_t *handle) {
    if (!handle) {
        return LED_ERROR_INVALID_PARAM;
    }
    
    memset(handle, 0, sizeof(led_handle_t));
    
    ESP_LOGI(TAG, "Initializing LED system...");
    
    // Configure PWM
    led_error_t ret = led_configure_pwm();
    if (ret != LED_OK) {
        ESP_LOGE(TAG, "Failed to configure PWM");
        return ret;
    }
    
    // Set default configuration
    led_color_t default_color = LED_COLOR_OFF;
    handle->current_config.color = default_color;
    handle->current_config.pattern = LED_PATTERN_SOLID;
    handle->current_config.brightness = DEFAULT_LED_BRIGHTNESS;
    handle->current_config.enabled = true;
    handle->current_status = LED_STATUS_OFF;
    handle->initialized = true;
    
    // Turn off LED initially
    led_turn_off(handle);
    
    ESP_LOGI(TAG, "LED system initialized successfully");
    return LED_OK;
}

// Deinitialize LED system
led_error_t led_deinit(led_handle_t *handle) {
    if (!handle) {
        return LED_ERROR_INVALID_PARAM;
    }
    
    // Stop pattern task
    led_stop_pattern_task(handle);
    
    // Turn off LED
    led_turn_off(handle);
    
    handle->initialized = false;
    ESP_LOGI(TAG, "LED system deinitialized");
    return LED_OK;
}

// Set LED color
led_error_t led_set_color(led_handle_t *handle, led_color_t color) {
    if (!handle || !handle->initialized) {
        return LED_ERROR_INIT;
    }
    
    handle->current_config.color = color;
    handle->current_config.pattern = LED_PATTERN_SOLID;
    handle->current_config.enabled = true;
    
    return led_update_hardware(handle, color, handle->current_config.brightness);
}

// Set LED RGB values
led_error_t led_set_rgb(led_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue) {
    led_color_t color = {red, green, blue};
    return led_set_color(handle, color);
}

// Set LED brightness
led_error_t led_set_brightness(led_handle_t *handle, uint8_t brightness) {
    if (!handle || !handle->initialized) {
        return LED_ERROR_INIT;
    }
    
    handle->current_config.brightness = brightness;
    
    if (handle->current_config.enabled && handle->current_config.pattern == LED_PATTERN_SOLID) {
        return led_update_hardware(handle, handle->current_config.color, brightness);
    }
    
    return LED_OK;
}

// Turn off LED
led_error_t led_turn_off(led_handle_t *handle) {
    if (!handle || !handle->initialized) {
        return LED_ERROR_INIT;
    }
    
    led_color_t off_color = LED_COLOR_OFF;
    handle->current_config.enabled = false;
    handle->current_status = LED_STATUS_OFF;
    
    return led_update_hardware(handle, off_color, 0);
}

// Set LED pattern
led_error_t led_set_pattern(led_handle_t *handle, led_color_t color, led_pattern_t pattern) {
    if (!handle || !handle->initialized) {
        return LED_ERROR_INIT;
    }
    
    handle->current_config.color = color;
    handle->current_config.pattern = pattern;
    handle->current_config.enabled = true;
    
    // If it's a solid pattern, update immediately
    if (pattern == LED_PATTERN_SOLID) {
        return led_update_hardware(handle, color, handle->current_config.brightness);
    }
    
    // For dynamic patterns, ensure the task is running
    if (!handle->pattern_task_handle) {
        return led_start_pattern_task(handle);
    }
    
    return LED_OK;
}

// Set LED status with predefined patterns
led_error_t led_set_status(led_handle_t *handle, led_status_t status) {
    if (!handle || !handle->initialized) {
        ESP_LOGW(TAG, "Invalid LED handle to set status");
        return LED_ERROR_INIT;
    }
    
    handle->current_status = status;
    
    switch (status) {
        case LED_STATUS_OFF: {
            led_color_t off_color = LED_COLOR_OFF;
            return led_set_pattern(handle, off_color, LED_PATTERN_SOLID);
        }
        
        case LED_STATUS_INITIALIZING: {
            led_color_t blue_color = LED_COLOR_BLUE;
            return led_set_pattern(handle, blue_color, LED_PATTERN_PULSE_SLOW);
        }

        case LED_STATUS_WORKING: {
            led_color_t green_color = LED_COLOR_GREEN;
            return led_set_pattern(handle, green_color, LED_PATTERN_BLINK_SLOW);
        }
        
        case LED_STATUS_READY: {
            led_color_t green_color = LED_COLOR_GREEN;
            return led_set_pattern(handle, green_color, LED_PATTERN_SOLID);
        }
        
        case LED_STATUS_READING: {
            led_color_t green_color = LED_COLOR_GREEN;
            return led_set_pattern(handle, green_color, LED_PATTERN_BLINK_FAST);
        }

        case LED_STATUS_WARNING: {
            led_color_t yellow_color = LED_COLOR_YELLOW;
            return led_set_pattern(handle, yellow_color, LED_PATTERN_BLINK_SLOW);
        }
        
        case LED_STATUS_ERROR: {
            led_color_t red_color = LED_COLOR_RED;
            return led_set_pattern(handle, red_color, LED_PATTERN_SOLID);
        }
        
        case LED_STATUS_COMMUNICATION_ERROR: {
            led_color_t red_color = LED_COLOR_RED;
            return led_set_pattern(handle, red_color, LED_PATTERN_BLINK_FAST);
        }
        
        case LED_STATUS_CUSTOM:
            // Keep current configuration
            return LED_OK;
            
        default:
            return LED_ERROR_INVALID_PARAM;
    }
}

// Start pattern task
led_error_t led_start_pattern_task(led_handle_t *handle) {
    if (!handle || !handle->initialized) {
        return LED_ERROR_INIT;
    }
    
    if (handle->pattern_task_handle) {
        ESP_LOGW(TAG, "Pattern task already running");
        return LED_OK;
    }
    
    BaseType_t ret = xTaskCreate(
        led_pattern_task,
        LED_TASK_NAME,
        LED_TASK_STACK_SIZE,
        handle,
        LED_TASK_PRIORITY,
        &handle->pattern_task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED pattern task");
        return LED_ERROR_TASK;
    }
    
    ESP_LOGI(TAG, "LED pattern task started");
    return LED_OK;
}

// Stop pattern task
led_error_t led_stop_pattern_task(led_handle_t *handle) {
    if (!handle) {
        return LED_ERROR_INVALID_PARAM;
    }
    
    if (handle->pattern_task_handle) {
        vTaskDelete(handle->pattern_task_handle);
        handle->pattern_task_handle = NULL;
        ESP_LOGI(TAG, "LED pattern task stopped");
    }
    
    return LED_OK;
}

// Get predefined color by name
led_color_t led_get_predefined_color(const char *color_name) {
    if (strcmp(color_name, "red") == 0) {
        led_color_t color = LED_COLOR_RED;
        return color;
    } else if (strcmp(color_name, "green") == 0) {
        led_color_t color = LED_COLOR_GREEN;
        return color;
    } else if (strcmp(color_name, "blue") == 0) {
        led_color_t color = LED_COLOR_BLUE;
        return color;
    } else if (strcmp(color_name, "yellow") == 0) {
        led_color_t color = LED_COLOR_YELLOW;
        return color;
    } else if (strcmp(color_name, "cyan") == 0) {
        led_color_t color = LED_COLOR_CYAN;
        return color;
    } else if (strcmp(color_name, "magenta") == 0) {
        led_color_t color = LED_COLOR_MAGENTA;
        return color;
    } else if (strcmp(color_name, "white") == 0) {
        led_color_t color = LED_COLOR_WHITE;
        return color;
    } else if (strcmp(color_name, "orange") == 0) {
        led_color_t color = LED_COLOR_ORANGE;
        return color;
    } else if (strcmp(color_name, "purple") == 0) {
        led_color_t color = LED_COLOR_PURPLE;
        return color;
    } else {
        led_color_t color = LED_COLOR_OFF;
        return color;
    }
}

// Show startup sequence
void led_show_startup_sequence(led_handle_t *handle) {
    if (!handle || !handle->initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "Starting LED startup sequence");
    
    // Red for 200ms
    led_color_t red = LED_COLOR_RED;
    led_set_color(handle, red);
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Green for 200ms
    led_color_t green = LED_COLOR_GREEN;
    led_set_color(handle, green);
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Blue for 200ms
    led_color_t blue = LED_COLOR_BLUE;
    led_set_color(handle, blue);
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // White for 200ms
    led_color_t white = LED_COLOR_WHITE;
    led_set_color(handle, white);
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Turn off
    led_turn_off(handle);
    
    ESP_LOGI(TAG, "LED startup sequence completed");
}
