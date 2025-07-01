#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// LED pin definitions
#define LED_RED_PIN         39
#define LED_GREEN_PIN       40
#define LED_BLUE_PIN        38

// Task configuration
#define LED_TASK_STACK_SIZE (4 * 1024)
#define LED_TASK_PRIORITY   0
#define LED_TASK_NAME       "led_task"

// LED PWM configuration
#define LED_FREQUENCY       5000        // 5kHz PWM frequency
#define LED_RESOLUTION      LEDC_TIMER_8_BIT  // 8-bit resolution (0-255)
#define LED_MAX_BRIGHTNESS  255         // Maximum brightness value
#define DEFAULT_LED_BRIGHTNESS 191     // 75% brightness

// LEDC timer and channel assignments
#define LED_TIMER           LEDC_TIMER_0
#define LED_MODE            LEDC_LOW_SPEED_MODE
#define LED_RED_CHANNEL     LEDC_CHANNEL_0
#define LED_GREEN_CHANNEL   LEDC_CHANNEL_1
#define LED_BLUE_CHANNEL    LEDC_CHANNEL_2

// Predefined colors (RGB values 0-255)
#define LED_COLOR_OFF       {0, 0, 0}
#define LED_COLOR_RED       {255, 0, 0}
#define LED_COLOR_GREEN     {0, 255, 0}
#define LED_COLOR_BLUE      {0, 0, 255}
#define LED_COLOR_YELLOW    {255, 255, 0}
#define LED_COLOR_CYAN      {0, 255, 255}
#define LED_COLOR_MAGENTA   {255, 0, 255}
#define LED_COLOR_WHITE     {255, 255, 255}
#define LED_COLOR_ORANGE    {255, 165, 0}
#define LED_COLOR_PURPLE    {128, 0, 128}

// LED status patterns
typedef enum {
    LED_STATUS_OFF = 0,
    LED_STATUS_INITIALIZING,   // Slow blue pulse
    LED_STATUS_WORKING,        // Slow green blink
    LED_STATUS_READY,          // Solid green
    LED_STATUS_READING,        // Fast green blink
    LED_STATUS_WARNING,        // Slow yellow blink
    LED_STATUS_ERROR,          // Solid red
    LED_STATUS_COMMUNICATION_ERROR, // Fast red blink
    LED_STATUS_CUSTOM          // Custom color/pattern
} led_status_t;

// LED pattern types
typedef enum {
    LED_PATTERN_SOLID = 0,
    LED_PATTERN_BLINK_SLOW,    // 1 second on/off
    LED_PATTERN_BLINK_FAST,    // 250ms on/off
    LED_PATTERN_PULSE_SLOW,    // Breathing effect
    LED_PATTERN_PULSE_FAST     // Fast breathing effect
} led_pattern_t;

// RGB color structure
typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_color_t;

// LED configuration structure
typedef struct {
    led_color_t color;
    led_pattern_t pattern;
    uint8_t brightness;     // 0-255
    bool enabled;
} led_config_t;

// LED handle structure
typedef struct {
    bool initialized;
    led_config_t current_config;
    TaskHandle_t pattern_task_handle;
    led_status_t current_status;
} led_handle_t;

// Error codes
typedef enum {
    LED_OK = 0,
    LED_ERROR_INIT = -1,
    LED_ERROR_INVALID_PARAM = -2,
    LED_ERROR_TASK = -3
} led_error_t;

// Function prototypes

// Initialization and cleanup
led_error_t led_init(led_handle_t *handle);
led_error_t led_deinit(led_handle_t *handle);

// Basic color control
led_error_t led_set_color(led_handle_t *handle, led_color_t color);
led_error_t led_set_rgb(led_handle_t *handle, uint8_t red, uint8_t green, uint8_t blue);
led_error_t led_set_brightness(led_handle_t *handle, uint8_t brightness);
led_error_t led_turn_off(led_handle_t *handle);

// Pattern control
led_error_t led_set_pattern(led_handle_t *handle, led_color_t color, led_pattern_t pattern);
led_error_t led_set_status(led_handle_t *handle, led_status_t status);

// Advanced control
led_error_t led_start_pattern_task(led_handle_t *handle);
led_error_t led_stop_pattern_task(led_handle_t *handle);

// Utility functions
led_color_t led_get_predefined_color(const char *color_name);
void led_show_startup_sequence(led_handle_t *handle);
