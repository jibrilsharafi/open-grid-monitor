#include "ade7953.h"

static const char *TAG = "ade7953";

// Forward declaration of the task function
static void ade7953_task(void *pvParameters);

// Communication verification functions
static ade7953_error_t ade7953_verify_last_communication(ade7953_handle_t *handle, uint16_t expected_address, uint8_t expected_bits, uint32_t expected_data, bool was_write);
static ade7953_error_t ade7953_test_communication(ade7953_handle_t *handle);

// Hardware reset function
static void ade7953_hardware_reset(void) {
    gpio_set_level(ADE7953_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(ADE7953_RESET_DURATION_MS));
    gpio_set_level(ADE7953_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow time for startup
}

// Configure GPIO pins
static ade7953_error_t ade7953_configure_gpio(void) {
    gpio_config_t io_conf = {};
    
    // Configure reset pin as output
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << ADE7953_RESET_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    if (gpio_config(&io_conf) != ESP_OK) {
        return ADE7953_ERROR_INIT;
    }
    
    // Configure interrupt pin as input (for future use)
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << ADE7953_INTERRUPT_PIN);
    io_conf.pull_up_en = 1;
    if (gpio_config(&io_conf) != ESP_OK) {
        return ADE7953_ERROR_INIT;
    }
    
    // Set initial states
    gpio_set_level(ADE7953_RESET_PIN, 1);
    
    return ADE7953_OK;
}

// Configure SPI interface
static ade7953_error_t ade7953_configure_spi(ade7953_handle_t *handle) {
    spi_bus_config_t buscfg = {
        .mosi_io_num = ADE7953_MOSI_PIN,
        .miso_io_num = ADE7953_MISO_PIN,
        .sclk_io_num = ADE7953_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    
    spi_device_interface_config_t devcfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,  // SPI Mode 0
        .duty_cycle_pos = 0,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .clock_speed_hz = ADE7953_SPI_FREQUENCY,
        .input_delay_ns = 0,
        .spics_io_num = ADE7953_SS_PIN,
        .flags = 0,
        .queue_size = 1,
        .pre_cb = NULL,
        .post_cb = NULL,
    };
    
    // Initialize the SPI bus
    esp_err_t ret = spi_bus_initialize(ADE7953_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ADE7953_ERROR_INIT;
    }
    
    // Add device to the SPI bus
    ret = spi_bus_add_device(ADE7953_SPI_HOST, &devcfg, &handle->spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        spi_bus_free(ADE7953_SPI_HOST);
        return ADE7953_ERROR_INIT;
    }
    
    return ADE7953_OK;
}

// Write to ADE7953 register
ade7953_error_t ade7953_write_register(ade7953_handle_t *handle, uint16_t reg_addr, uint8_t n_bits, uint32_t data) {
    if (!handle || !handle->initialized) {
        return ADE7953_ERROR_INIT;
    }
    
    // Take mutex with timeout
    if (xSemaphoreTake(handle->spi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire SPI mutex");
        return ADE7953_ERROR_TIMEOUT;
    }
    
    uint8_t tx_data[8];  // Max 2 bytes address + 1 byte command + 4 bytes data + 1 byte padding
    uint8_t tx_len = 3 + (n_bits / 8);  // Address (2) + command (1) + data
    
    // Prepare transmission data
    tx_data[0] = (reg_addr >> 8) & 0xFF;  // Address high byte
    tx_data[1] = reg_addr & 0xFF;         // Address low byte
    tx_data[2] = WRITE_TRANSFER;          // Write command
    
    // Add data bytes (MSB first)
    if (n_bits == 32) {
        tx_data[3] = (data >> 24) & 0xFF;
        tx_data[4] = (data >> 16) & 0xFF;
        tx_data[5] = (data >> 8) & 0xFF;
        tx_data[6] = data & 0xFF;
    } else if (n_bits == 24) {
        tx_data[3] = (data >> 16) & 0xFF;
        tx_data[4] = (data >> 8) & 0xFF;
        tx_data[5] = data & 0xFF;
    } else if (n_bits == 16) {
        tx_data[3] = (data >> 8) & 0xFF;
        tx_data[4] = data & 0xFF;
    } else if (n_bits == 8) {
        tx_data[3] = data & 0xFF;
    } else {
        xSemaphoreGive(handle->spi_mutex);
        return ADE7953_ERROR_COMMUNICATION;
    }
    
    spi_transaction_t trans = {
        .length = tx_len * 8,  // Length in bits
        .tx_buffer = tx_data,
        .rx_buffer = NULL,
    };
    
    esp_err_t ret = spi_device_transmit(handle->spi_handle, &trans);
    xSemaphoreGive(handle->spi_mutex);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI write failed: %s", esp_err_to_name(ret));
        return ADE7953_ERROR_SPI;
    }
    
    ESP_LOGD(TAG, "Write register 0x%04X: 0x%08lX (%d bits)", reg_addr, data, n_bits);
    return ADE7953_OK;
}

// Read from ADE7953 register
ade7953_error_t ade7953_read_register(ade7953_handle_t *handle, uint16_t reg_addr, uint8_t n_bits, uint32_t *data) {
    if (!handle || !handle->initialized || !data) {
        return ADE7953_ERROR_INIT;
    }
    
    // Take mutex with timeout
    if (xSemaphoreTake(handle->spi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire SPI mutex");
        return ADE7953_ERROR_TIMEOUT;
    }
    
    uint8_t tx_data[3];
    uint8_t rx_data[8];  // Max 4 bytes data + some padding
    uint8_t data_bytes = n_bits / 8;
    uint8_t total_len = 3 + data_bytes;  // Address (2) + command (1) + data
    
    // Prepare command
    tx_data[0] = (reg_addr >> 8) & 0xFF;  // Address high byte
    tx_data[1] = reg_addr & 0xFF;         // Address low byte
    tx_data[2] = READ_TRANSFER;           // Read command
    
    spi_transaction_t trans = {
        .length = total_len * 8,  // Length in bits
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };
    
    esp_err_t ret = spi_device_transmit(handle->spi_handle, &trans);
    xSemaphoreGive(handle->spi_mutex);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI read failed: %s", esp_err_to_name(ret));
        return ADE7953_ERROR_SPI;
    }
    
    // Extract data from response (skip first 3 bytes which are echo of command)
    *data = 0;
    for (int i = 0; i < data_bytes; i++) {
        *data = (*data << 8) | rx_data[3 + i];
    }
    
    ESP_LOGD(TAG, "Read register 0x%04X: 0x%08lX (%d bits)", reg_addr, *data, n_bits);
    return ADE7953_OK;
}

// Write to ADE7953 register with communication verification
ade7953_error_t ade7953_write_register_verified(ade7953_handle_t *handle, uint16_t reg_addr, uint8_t n_bits, uint32_t data) {
    ade7953_error_t ret;
    
    // Perform the write operation
    ret = ade7953_write_register(handle, reg_addr, n_bits, data);
    if (ret != ADE7953_OK) {
        return ret;
    }
    
    // Verify the write operation
    ret = ade7953_verify_last_communication(handle, reg_addr, n_bits, data, true);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Write verification failed for register 0x%04X", reg_addr);
        return ret;
    }
    
    return ADE7953_OK;
}

// Read from ADE7953 register with communication verification
ade7953_error_t ade7953_read_register_verified(ade7953_handle_t *handle, uint16_t reg_addr, uint8_t n_bits, uint32_t *data) {
    ade7953_error_t ret;
    
    // Perform the read operation
    ret = ade7953_read_register(handle, reg_addr, n_bits, data);
    if (ret != ADE7953_OK) {
        return ret;
    }
    
    // Verify the read operation
    ret = ade7953_verify_last_communication(handle, reg_addr, n_bits, *data, false);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Read verification failed for register 0x%04X", reg_addr);
        return ret;
    }
    
    return ADE7953_OK;
}

// Configure ADE7953 with default settings
static ade7953_error_t ade7953_configure_device(ade7953_handle_t *handle) {
    ade7953_error_t ret;
    
    ESP_LOGI(TAG, "Configuring ADE7953 with verified writes...");
    
    // Unlock optimum register with verification
    ret = ade7953_write_register_verified(handle, UNLOCK_OPTIMUM_REGISTER, 8, UNLOCK_OPTIMUM_REGISTER_VALUE);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Failed to unlock optimum register");
        return ret;
    }
    
    // Set optimum register with verification
    ret = ade7953_write_register_verified(handle, Reserved_16, 16, DEFAULT_OPTIMUM_REGISTER);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Failed to set optimum register");
        return ret;
    }
    
    ESP_LOGI(TAG, "ADE7953 configured successfully with verification");
    return ADE7953_OK;
}

// Read grid frequency
ade7953_error_t ade7953_read_frequency(ade7953_handle_t *handle, float *frequency) {
    if (!handle || !frequency) {
        return ADE7953_ERROR_INIT;
    }
    
    uint32_t period_reg;
    ade7953_error_t ret = ade7953_read_register(handle, PERIOD_16, 16, &period_reg);
    if (ret != ADE7953_OK) {
        return ret;
    }
    
    if (period_reg == 0) {
        *frequency = 0.0f;  // Invalid reading
        return ADE7953_ERROR_COMMUNICATION;
    }
    
    *frequency = GRID_FREQUENCY_CONVERSION_FACTOR / (float)period_reg;
    return ADE7953_OK;
}

// Read voltage RMS
ade7953_error_t ade7953_read_voltage(ade7953_handle_t *handle, float *voltage) {
    if (!handle || !voltage) {
        return ADE7953_ERROR_INIT;
    }
    
    uint32_t vrms_reg;
    ade7953_error_t ret = ade7953_read_register(handle, VRMS_32, 32, &vrms_reg);
    if (ret != ADE7953_OK) {
        return ret;
    }
    
    // This conversion factor may need calibration based on your hardware
    // For now, using a basic conversion - you'll need to calibrate this
    *voltage = (float)vrms_reg * VOLTAGE_CONVERSION_FACTOR;  // Placeholder conversion
    return ADE7953_OK;
}

// Background task for continuous readings
static void ade7953_task(void *pvParameters) {
    ade7953_handle_t *handle = (ade7953_handle_t *)pvParameters;
    
    ESP_LOGI(TAG, "ADE7953 task started");
    
    while (true) {
        float frequency, voltage;
        bool frequency_valid = false;
        bool voltage_valid = false;
        
        // Read frequency
        if (ade7953_read_frequency(handle, &frequency) == ADE7953_OK) {
            handle->grid_frequency = frequency;
            frequency_valid = true;
        }
        
        // Read voltage
        if (ade7953_read_voltage(handle, &voltage) == ADE7953_OK) {
            handle->voltage_rms = voltage;
            voltage_valid = true;
        }
        
        handle->last_reading_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        // Queue measurement to MQTT if both readings are valid and measurement queue is set
        if (frequency_valid && voltage_valid && handle->measurement_queue) {
            // Check if readings are within reasonable ranges before queuing
            if (frequency > 45.0f && frequency < 65.0f && voltage > 50.0f && voltage < 300.0f) {
                
                struct timeval tv_now;
                gettimeofday(&tv_now, NULL);
                int64_t time_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;

                measurement_t measurement = {
                    .timestamp_us = time_us,
                    .frequency = frequency,
                    .voltage = voltage
                };
                
                // Queue measurement (non-blocking)
                if (xQueueSend(handle->measurement_queue, &measurement, 0) != pdTRUE) {
                    // Queue is full, which is expected under high load
                    // Don't log every failure to avoid spam
                }
            }
        }
        
        // Wait for next sample (20ms for 50Hz measurements)
        vTaskDelay(pdMS_TO_TICKS(ADE7953_SAMPLE_INTERVAL_MS));
    }
}

// Initialize ADE7953
ade7953_error_t ade7953_init(ade7953_handle_t *handle) {
    if (!handle) {
        return ADE7953_ERROR_INIT;
    }
    
    memset(handle, 0, sizeof(ade7953_handle_t));
    
    ESP_LOGI(TAG, "Initializing ADE7953...");
    
    // Configure GPIO pins
    ade7953_error_t ret = ade7953_configure_gpio();
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO");
        return ret;
    }
    
    // Hardware reset
    ade7953_hardware_reset();
    
    // Configure SPI
    ret = ade7953_configure_spi(handle);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Failed to configure SPI");
        return ret;
    }
    
    // Create mutex
    handle->spi_mutex = xSemaphoreCreateMutex();
    if (!handle->spi_mutex) {
        ESP_LOGE(TAG, "Failed to create SPI mutex");
        spi_bus_remove_device(handle->spi_handle);
        spi_bus_free(ADE7953_SPI_HOST);
        return ADE7953_ERROR_INIT;
    }
    
    handle->initialized = true;
    
    // Test communication before configuration
    ESP_LOGI(TAG, "Testing ADE7953 communication...");
    ret = ade7953_test_communication(handle);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "ADE7953 communication test failed");
        ade7953_deinit(handle);
        return ret;
    }
    
    // Configure device
    ret = ade7953_configure_device(handle);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Failed to configure ADE7953 device");
        ade7953_deinit(handle);
        return ret;
    }
    
    ESP_LOGI(TAG, "ADE7953 initialized successfully with communication verification");
    return ADE7953_OK;
}

// Deinitialize ADE7953
ade7953_error_t ade7953_deinit(ade7953_handle_t *handle) {
    if (!handle) {
        return ADE7953_ERROR_INIT;
    }
    
    // Stop task if running
    ade7953_stop_task(handle);
    
    // Clean up SPI
    if (handle->spi_handle) {
        spi_bus_remove_device(handle->spi_handle);
        spi_bus_free(ADE7953_SPI_HOST);
    }
    
    // Clean up mutex
    if (handle->spi_mutex) {
        vSemaphoreDelete(handle->spi_mutex);
    }
    
    handle->initialized = false;
    ESP_LOGI(TAG, "ADE7953 deinitialized");
    return ADE7953_OK;
}

// Start background task
ade7953_error_t ade7953_start_task(ade7953_handle_t *handle) {
    if (!handle || !handle->initialized) {
        return ADE7953_ERROR_INIT;
    }
    
    if (handle->task_handle) {
        ESP_LOGW(TAG, "Task already running");
        return ADE7953_OK;
    }
    
    BaseType_t ret = xTaskCreate(
        ade7953_task,
        ADE7953_TASK_NAME,
        ADE7953_TASK_STACK_SIZE,
        handle,
        ADE7953_TASK_PRIORITY,
        &handle->task_handle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ADE7953 task");
        return ADE7953_ERROR_INIT;
    }
    
    return ADE7953_OK;
}

// Stop background task
ade7953_error_t ade7953_stop_task(ade7953_handle_t *handle) {
    if (!handle) {
        return ADE7953_ERROR_INIT;
    }
    
    if (handle->task_handle) {
        vTaskDelete(handle->task_handle);
        handle->task_handle = NULL;
    }
    
    return ADE7953_OK;
}

// Get latest frequency reading (non-blocking)
float ade7953_get_latest_frequency(ade7953_handle_t *handle) {
    if (!handle) {
        return 0.0f;
    }
    return handle->grid_frequency;
}

// Get latest voltage reading (non-blocking)
float ade7953_get_latest_voltage(ade7953_handle_t *handle) {
    if (!handle) {
        return 0.0f;
    }
    return handle->voltage_rms;
}

// Get timestamp of last reading
uint32_t ade7953_get_last_reading_time(ade7953_handle_t *handle) {
    if (!handle) {
        return 0;
    }
    return handle->last_reading_ms;
}

// Verify last communication with ADE7953
static ade7953_error_t ade7953_verify_last_communication(ade7953_handle_t *handle, uint16_t expected_address, uint8_t expected_bits, uint32_t expected_data, bool was_write) {
    uint32_t last_address, last_op, last_data;
    ade7953_error_t ret;
    
    // Check last address
    ret = ade7953_read_register(handle, LAST_ADD_16, 16, &last_address);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Failed to read LAST_ADD register");
        return ret;
    }
    
    if (last_address != expected_address) {
        ESP_LOGE(TAG, "Address verification failed: expected 0x%04X, got 0x%08lX", expected_address, last_address);
        return ADE7953_ERROR_COMMUNICATION;
    }
    
    // Check last operation type
    ret = ade7953_read_register(handle, LAST_OP_8, 8, &last_op);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Failed to read LAST_OP register");
        return ret;
    }
    
    uint32_t expected_op = was_write ? LAST_OP_WRITE_VALUE : LAST_OP_READ_VALUE;
    if (last_op != expected_op) {
        ESP_LOGE(TAG, "Operation type verification failed: expected 0x%02lX, got 0x%08lX", expected_op, last_op);
        return ADE7953_ERROR_COMMUNICATION;
    }
    
    // Check last data (select appropriate register based on bit size)
    uint16_t data_register;
    switch (expected_bits) {
        case 8:
            data_register = LAST_RWDATA_8;
            break;
        case 16:
            data_register = LAST_RWDATA_16;
            break;
        case 24:
            data_register = LAST_RWDATA_24;
            break;
        case 32:
            data_register = LAST_RWDATA_32;
            break;
        default:
            ESP_LOGE(TAG, "Invalid bit size for verification: %d", expected_bits);
            return ADE7953_ERROR_COMMUNICATION;
    }
    
    ret = ade7953_read_register(handle, data_register, expected_bits, &last_data);
    if (ret != ADE7953_OK) {
        ESP_LOGE(TAG, "Failed to read LAST_RWDATA register (0x%04X)", data_register);
        return ret;
    }
    
    if (last_data != expected_data) {
        ESP_LOGE(TAG, "Data verification failed: expected 0x%08lX, got 0x%08lX", expected_data, last_data);
        return ADE7953_ERROR_COMMUNICATION;
    }
    
    ESP_LOGD(TAG, "Communication verification successful: addr=0x%04X, op=0x%02lX, data=0x%08lX", 
             expected_address, expected_op, expected_data);
    return ADE7953_OK;
}

// Test communication with ADE7953 by reading a known register with default value
static ade7953_error_t ade7953_test_communication(ade7953_handle_t *handle) {
    ade7953_error_t ret;
    uint32_t read_value;
    
    ESP_LOGI(TAG, "Testing ADE7953 communication...");
    
    for (int attempt = 0; attempt < ADE7953_MAX_VERIFY_ATTEMPTS; attempt++) {
        ESP_LOGD(TAG, "Attempt (%d/%d) to communicate with ADE7953", attempt + 1, ADE7953_MAX_VERIFY_ATTEMPTS);
        
        // Perform hardware reset before each attempt
        ade7953_hardware_reset();
        
        // Read the AP_NOLOAD_32 register which has a known default value
        ret = ade7953_read_register(handle, AP_NOLOAD_32_REGISTER, 32, &read_value);
        if (ret != ADE7953_OK) {
            ESP_LOGW(TAG, "Failed to read AP_NOLOAD register on attempt %d", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(ADE7953_VERIFY_DELAY_MS));
            continue;
        }
        
        // Check if the read value matches the expected default value
        if (read_value == DEFAULT_EXPECTED_AP_NOLOAD_REGISTER) {
            ESP_LOGI(TAG, "Communication test successful on attempt %d", attempt + 1);
            return ADE7953_OK;
        } else {
            ESP_LOGW(TAG, "Failed to communicate with ADE7953 on attempt (%d/%d). Expected 0x%08lX, got 0x%08lX. Retrying in %d ms", 
                     attempt + 1, ADE7953_MAX_VERIFY_ATTEMPTS, 
                     (uint32_t)DEFAULT_EXPECTED_AP_NOLOAD_REGISTER, read_value, 
                     ADE7953_VERIFY_DELAY_MS);
        }
        
        vTaskDelay(pdMS_TO_TICKS(ADE7953_VERIFY_DELAY_MS));
    }
    
    ESP_LOGE(TAG, "Communication test failed after %d attempts", ADE7953_MAX_VERIFY_ATTEMPTS);
    return ADE7953_ERROR_COMMUNICATION;
}

// Set network handle for MQTT publishing
void ade7953_set_measurement_queue(ade7953_handle_t *handle, QueueHandle_t measurement_queue) {
    if (handle) {
        handle->measurement_queue = measurement_queue;
        ESP_LOGI(TAG, "Measurement queue set for publishing");
    }
}
