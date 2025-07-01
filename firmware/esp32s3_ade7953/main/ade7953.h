#pragma once

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "struct.h"

// Pin definitions
#define ADE7953_SS_PIN          48
#define ADE7953_SCK_PIN         36
#define ADE7953_MISO_PIN        35
#define ADE7953_MOSI_PIN        45
#define ADE7953_RESET_PIN       21
#define ADE7953_INTERRUPT_PIN   37

// SPI configuration
#define ADE7953_SPI_FREQUENCY   2000000  // 2MHz max frequency
#define ADE7953_SPI_HOST        SPI2_HOST

// Register addresses (key ones for frequency and voltage)
#define PERIOD_16               0x10E    // Period register for frequency calculation
#define VRMS_32                 0x31C    // Voltage RMS register
#define UNLOCK_OPTIMUM_REGISTER 0x00FE   // Register to unlock optimum settings
#define Reserved_16             0x120    // Reserved register for optimum settings

// Communication verification registers
#define LAST_OP_8               0x0FD    // Contains the type of last successful communication
#define LAST_ADD_16             0x1FE    // Contains the address of last successful communication  
#define LAST_RWDATA_8           0x0FF    // Contains data from last successful 8-bit communication
#define LAST_RWDATA_16          0x1FF    // Contains data from last successful 16-bit communication
#define LAST_RWDATA_24          0x2FF    // Contains data from last successful 24-bit communication
#define LAST_RWDATA_32          0x3FF    // Contains data from last successful 32-bit communication

// Default configuration values
#define AP_NOLOAD_32_REGISTER           0x303
#define DEFAULT_X_NOLOAD_REGISTER       0x00E419            // No-load threshold
#define DEFAULT_EXPECTED_AP_NOLOAD_REGISTER 0x00E419        // Expected default value for AP_NOLOAD register
#define UNLOCK_OPTIMUM_REGISTER_VALUE   0xAD                // Value to unlock optimum register
#define DEFAULT_OPTIMUM_REGISTER        0x0030              // Optimum register value

// SPI transfer commands
#define READ_TRANSFER           0x80
#define WRITE_TRANSFER          0x00

// Communication verification constants
#define LAST_OP_READ_VALUE      0x35    // Value stored in LAST_OP register after a read operation
#define LAST_OP_WRITE_VALUE     0xCA    // Value stored in LAST_OP register after a write operation
#define ADE7953_MAX_VERIFY_ATTEMPTS 5   // Maximum attempts for communication verification
#define ADE7953_VERIFY_DELAY_MS     10  // Delay between verification attempts

// Conversion factors
#define GRID_FREQUENCY_CONVERSION_FACTOR 223750.0f // Clock of the period measurement of 223.75 kHz
#define VOLTAGE_CONVERSION_FACTOR 0.00003879f // Conversion accounting for 990kohm to 1 kohm voltage divider

// Task configuration
#define ADE7953_TASK_STACK_SIZE (8 * 1024)
#define ADE7953_TASK_PRIORITY   10
#define ADE7953_TASK_NAME       "ade7953_task"

// Timing
#define ADE7953_RESET_DURATION_MS       200
#define ADE7953_SAMPLE_INTERVAL_MS      20  // 50Hz grid = 20ms per cycle

// Error codes
typedef enum {
    ADE7953_OK = 0,
    ADE7953_ERROR_INIT = -1,
    ADE7953_ERROR_SPI = -2,
    ADE7953_ERROR_COMMUNICATION = -3,
    ADE7953_ERROR_TIMEOUT = -4
} ade7953_error_t;

// ADE7953 handle structure
typedef struct {
    spi_device_handle_t spi_handle;
    SemaphoreHandle_t spi_mutex;
    TaskHandle_t task_handle;
    bool initialized;
    
    // Latest readings
    float grid_frequency;
    float voltage_rms;
    uint32_t last_reading_ms;
    
    // Measurement queue for MQTT publishing
    QueueHandle_t measurement_queue;
} ade7953_handle_t;

// Function prototypes
ade7953_error_t ade7953_init(ade7953_handle_t *handle);
ade7953_error_t ade7953_deinit(ade7953_handle_t *handle);

// Low-level register access
ade7953_error_t ade7953_write_register(ade7953_handle_t *handle, uint16_t reg_addr, uint8_t n_bits, uint32_t data);
ade7953_error_t ade7953_read_register(ade7953_handle_t *handle, uint16_t reg_addr, uint8_t n_bits, uint32_t *data);

// Low-level register access with verification
ade7953_error_t ade7953_write_register_verified(ade7953_handle_t *handle, uint16_t reg_addr, uint8_t n_bits, uint32_t data);
ade7953_error_t ade7953_read_register_verified(ade7953_handle_t *handle, uint16_t reg_addr, uint8_t n_bits, uint32_t *data);

// High-level measurement functions
ade7953_error_t ade7953_read_frequency(ade7953_handle_t *handle, float *frequency);
ade7953_error_t ade7953_read_voltage(ade7953_handle_t *handle, float *voltage);

// Task management
ade7953_error_t ade7953_start_task(ade7953_handle_t *handle);
ade7953_error_t ade7953_stop_task(ade7953_handle_t *handle);

// Get latest readings (non-blocking)
float ade7953_get_latest_frequency(ade7953_handle_t *handle);
float ade7953_get_latest_voltage(ade7953_handle_t *handle);
uint32_t ade7953_get_last_reading_time(ade7953_handle_t *handle);

// Set measurement queue for MQTT publishing
void ade7953_set_measurement_queue(ade7953_handle_t *handle, QueueHandle_t measurement_queue);
