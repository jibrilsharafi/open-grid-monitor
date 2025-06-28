#include "network.h"
#include "esp_mac.h"

static const char *TAG = "network";

// Global variables
static network_handle_t *g_network_handle = NULL;
static httpd_handle_t g_ota_server = NULL;
static TaskHandle_t g_mqtt_log_task = NULL;
static TaskHandle_t g_measurement_task = NULL;
static esp_mqtt_client_handle_t g_mqtt_client = NULL;
static QueueHandle_t g_log_queue = NULL;
static bool g_log_forwarding_initialized = false;
static vprintf_like_t g_original_log_function = NULL;
static TaskHandle_t g_rollback_check_task = NULL;
static bool g_time_synced = false;

// Forward declarations
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static esp_err_t ota_upload_handler(httpd_req_t *req);
static void mqtt_logging_task(void *pvParameters);
static void measurement_publishing_task(void *pvParameters);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static int custom_log_writer(const char *fmt, va_list args);
static void rollback_check_task(void *pvParameters);
static void add_to_log_buffer(network_handle_t *handle, const char *message, const char *topic);
static void add_to_log_buffer(network_handle_t *handle, const char *message, const char *topic);
static void handle_mqtt_command(const char *command);
static esp_err_t perform_mqtt_ota(const char *url);
static const char* ota_state_to_string(esp_ota_img_states_t state);
static void sntp_sync_notification_cb(struct timeval *tv);

// Custom vprintf implementation - MUST BE FAST AND NON-BLOCKING
static int custom_log_writer(const char *fmt, va_list args)
{
    // Prevent recursion and stack overflow
    static bool in_custom_writer = false;
    if (in_custom_writer) {
        // If we're already in the custom writer, just use the original function
        if (g_original_log_function) {
            return g_original_log_function(fmt, args);
        }
        return vprintf(fmt, args);
    }
    
    in_custom_writer = true;
    
    char temp_buffer[MQTT_MSG_MAX_SIZE];
    int msg_len = 0;
    int total_len = 0;

    va_list args_copy;
    va_copy(args_copy, args);
    msg_len = vsnprintf(temp_buffer, sizeof(temp_buffer) - 1, fmt, args_copy);
    va_end(args_copy);

    if (msg_len >= 0) {
        total_len = msg_len;
        if (total_len >= sizeof(temp_buffer)) {
            total_len = sizeof(temp_buffer) - 1;
        }
        temp_buffer[total_len] = '\0';

        // Determine log level and construct topic
        const char *log_level = "info";  // Default to info if no level found
        char topic_buffer[80];
        const char *topic = NULL;
        
        if (strstr(temp_buffer, "E (") != NULL) {
            log_level = "error";
        } else if (strstr(temp_buffer, "W (") != NULL) {
            log_level = "warning";
        } else if (strstr(temp_buffer, "I (") != NULL) {
            log_level = "info";
        } else if (strstr(temp_buffer, "D (") != NULL) {
            log_level = "debug";
        }
        
        if (g_network_handle) {
            snprintf(topic_buffer, sizeof(topic_buffer), "%s/%s/logs/%s", 
                     MQTT_TOPIC_BASE, g_network_handle->mac_address, log_level);
            topic = topic_buffer;
        } else {
            // Fallback to default topic if handle not available
            snprintf(topic_buffer, sizeof(topic_buffer), "%s/logs/%s", MQTT_TOPIC_BASE, log_level);
            topic = topic_buffer;
        }

        // Try to send to MQTT queue first if available
        if (g_log_forwarding_initialized && g_log_queue != NULL) {
            log_message_t log_msg;
            log_msg.msg = malloc(total_len + 1);
            
            if (log_msg.msg != NULL) {
                memcpy(log_msg.msg, temp_buffer, total_len + 1);
                log_msg.topic = malloc(strlen(topic) + 1);
                if (log_msg.topic != NULL) {
                    strcpy(log_msg.topic, topic);
                }
                gettimeofday(&log_msg.timestamp, NULL);
                
                // Use non-blocking send to avoid blocking in interrupt context
                if (xQueueSend(g_log_queue, &log_msg, 0) != pdTRUE) {
                    // Queue full, free memory to avoid leak
                    free(log_msg.msg);
                    if (log_msg.topic) free(log_msg.topic);
                }
            }
        } 
        // If MQTT queue not available, use log buffer (but only for important messages)
        else if (g_network_handle && g_network_handle->log_buffer) {
            // Only buffer important messages (Error, Warning, Info) to prevent buffer overflow
            if (strstr(temp_buffer, "E (") != NULL || 
                strstr(temp_buffer, "W (") != NULL || 
                strstr(temp_buffer, "I (") != NULL) {
                add_to_log_buffer(g_network_handle, temp_buffer, topic);
            }
        }
    }
    
    in_custom_writer = false;
    
    // Always call original printf to maintain console output
    if (g_original_log_function) {
        return g_original_log_function(fmt, args);
    }
    return vprintf(fmt, args);
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    network_handle_t *handle = (network_handle_t *)arg;
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        handle->status = WIFI_STATUS_CONNECTING;
        ESP_LOGI(TAG, "WiFi station started, connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (handle->retry_count < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            handle->retry_count++;
            handle->status = WIFI_STATUS_CONNECTING;
            ESP_LOGI(TAG, "Retry to connect to WiFi (%d/%d)", handle->retry_count, WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(handle->wifi_event_group, WIFI_FAIL_BIT);
            handle->status = WIFI_STATUS_FAILED;
            ESP_LOGE(TAG, "Failed to connect to WiFi after %d attempts", WIFI_MAXIMUM_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(handle->ip_address, sizeof(handle->ip_address), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP address: %s", handle->ip_address);
        handle->retry_count = 0;
        handle->status = WIFI_STATUS_CONNECTED;
        xEventGroupSetBits(handle->wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// OTA upload handler
static esp_err_t ota_upload_handler(httpd_req_t *req) {
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *ota_partition = NULL;
    esp_err_t err;
    char buf[1024];
    int received;
    int remaining = req->content_len;
    bool first_chunk = true;

    ESP_LOGI(TAG, "Starting OTA update, content length: %d", remaining);

    // Get the next update partition
    ota_partition = esp_ota_get_next_update_partition(NULL);
    if (ota_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    while (remaining > 0) {
        received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Error receiving OTA data");
            if (ota_handle) {
                esp_ota_abort(ota_handle);
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive data");
            return ESP_FAIL;
        }

        if (first_chunk) {
            // Begin OTA update
            err = esp_ota_begin(ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
                return ESP_FAIL;
            }
            first_chunk = false;
        }

        // Write data to OTA partition
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        remaining -= received;
        ESP_LOGD(TAG, "OTA progress: %d/%d bytes", req->content_len - remaining, req->content_len);
    }

    // End OTA update
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    // Set boot partition
    err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful, initiating graceful restart...");
    httpd_resp_sendstr(req, "OTA update successful, restarting gracefully...");
    
    // Allow HTTP response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Perform graceful restart
    esp_err_t shutdown_err = network_graceful_shutdown_and_restart(g_network_handle, "HTTP OTA update completed");
    if (shutdown_err != ESP_OK) {
        ESP_LOGW(TAG, "Graceful restart failed, performing immediate restart");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    
    return ESP_OK;
}

// MQTT event handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    // Safe logging with NULL checks
    if (event && event->topic && event->data) {
        ESP_LOGD(TAG, "MQTT event received: %ld | Topic: %.*s | Data length: %d | Data: %.*s",
                 event_id, event->topic_len, event->topic, event->data_len, event->data_len, event->data);
    } else {
        ESP_LOGD(TAG, "MQTT event received: %ld | Topic: %s | Data length: %d",
                 event_id, event && event->topic ? "valid" : "null", event ? event->data_len : 0);
    }
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT client connected");
            
            // Flush any buffered logs first
            if (g_network_handle) {
                network_flush_log_buffer(g_network_handle);
            }
            
            // Publish firmware information on connect
            if (g_network_handle) {
                network_publish_firmware_info(g_network_handle);
            }
            
            // Check and publish core dump if available
            if (g_network_handle) {
                network_check_and_publish_coredump(g_network_handle);
            }
            
            // Subscribe to command topic if command handling is enabled
            if (g_network_handle && g_network_handle->mqtt_commands_enabled) {
                int msg_id = esp_mqtt_client_subscribe(g_mqtt_client, g_network_handle->mqtt_topic_command, 0);
                ESP_LOGI(TAG, "Subscribed to command topic, msg_id=%d", msg_id);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT client disconnected");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT unsubscribed, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT data received");
            // Check if this is a command message
            if (event && event->topic && event->topic_len > 0 && g_network_handle &&
                strncmp(event->topic, g_network_handle->mqtt_topic_command, strlen(g_network_handle->mqtt_topic_command)) == 0) {
                // Null-terminate the data for safe string handling
                if (event->data && event->data_len > 0) {
                    char command[64];
                    int len = MIN(event->data_len, sizeof(command) - 1);
                    strncpy(command, event->data, len);
                    command[len] = '\0';
                    ESP_LOGI(TAG, "Received command: %s", command);
                    handle_mqtt_command(command);
                } else {
                    ESP_LOGW(TAG, "Received command message with no data");
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT client error");
            break;
        default:
            break;
    }
}

// MQTT command handler
static void handle_mqtt_command(const char *command)
{
    if (command == NULL) {
        ESP_LOGW(TAG, "Received null command");
        return;
    }
    
    ESP_LOGI(TAG, "Processing MQTT command: %s", command);
    
    if (strcmp(command, MQTT_COMMAND_RESTART) == 0) {
        ESP_LOGW(TAG, "Restart command received via MQTT - initiating graceful restart...");
        
        // Send confirmation back via MQTT if possible
        if (g_mqtt_client && g_network_handle) {
            esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_status, 
                                  "Restart command received, performing graceful restart", 0, 0, 0);
        }
        
        // Perform graceful restart
        esp_err_t shutdown_err = network_graceful_shutdown_and_restart(g_network_handle, "MQTT restart command");
        if (shutdown_err != ESP_OK) {
            ESP_LOGW(TAG, "Graceful restart failed, performing immediate restart");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
    } else if (strncmp(command, "{", 1) == 0) {
        // Try to parse as JSON command (for OTA)
        cJSON *json = cJSON_Parse(command);
        if (json != NULL) {
            cJSON *ota_cmd = cJSON_GetObjectItem(json, "ota");
            if (ota_cmd != NULL && cJSON_IsString(ota_cmd)) {
                char *url = cJSON_GetStringValue(ota_cmd);
                if (url != NULL && strlen(url) > 0) {
                    ESP_LOGI(TAG, "OTA command received via MQTT, URL: %s", url);
                    
                    // Send status update
                    if (g_mqtt_client && g_network_handle) {
                        char status_msg[256];
                        snprintf(status_msg, sizeof(status_msg), "Starting OTA update from: %s", url);
                        esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_status, status_msg, 0, 0, 0);
                    }
                    
                    // Perform OTA update
                    esp_err_t ota_ret = perform_mqtt_ota(url);
                    if (ota_ret != ESP_OK) {
                        ESP_LOGE(TAG, "MQTT OTA failed: %s", esp_err_to_name(ota_ret));
                        if (g_mqtt_client && g_network_handle) {
                            char error_msg[128];
                            snprintf(error_msg, sizeof(error_msg), "OTA update failed: %s", esp_err_to_name(ota_ret));
                            esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_error, error_msg, 0, 0, 0);
                        }
                    }
                }
            }
            cJSON_Delete(json);
        } else {
            ESP_LOGW(TAG, "Failed to parse JSON command: %s", command);
            if (g_mqtt_client && g_network_handle) {
                esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_error, "Invalid JSON command format", 0, 0, 0);
            }
        }
    } else {
        ESP_LOGW(TAG, "Unknown MQTT command received: %s", command);
        
        // Send error response back via MQTT if possible
        if (g_mqtt_client && g_network_handle) {
            char error_msg[128];
            snprintf(error_msg, sizeof(error_msg), "Unknown command: %s", command);
            esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_error, error_msg, 0, 0, 0);
        }
    }
}

// MQTT logging task
static void mqtt_logging_task(void *pvParameters) {
    network_handle_t *handle = (network_handle_t *)pvParameters;
    log_message_t log_msg;
    TickType_t system_info_timer = 0;
    char system_info[512];
    
    ESP_LOGI(TAG, "MQTT logging task started");
    
    while (handle->mqtt_logging_enabled) {
        // Check for log messages in queue
        if (xQueueReceive(handle->log_queue, &log_msg, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (handle->status == WIFI_STATUS_CONNECTED && g_mqtt_client && log_msg.msg != NULL) {
                // Forward log message via MQTT
                const char *topic = log_msg.topic ? log_msg.topic : handle->mqtt_topic_logs;
                esp_mqtt_client_publish(g_mqtt_client, topic, log_msg.msg, 0, 0, 0);
            }
            
            // Free the allocated message memory
            if (log_msg.msg != NULL) {
                free(log_msg.msg);
            }
            if (log_msg.topic != NULL) {
                free(log_msg.topic);
            }
        }
        
        // Publish system information every 30 seconds
        if (handle->status == WIFI_STATUS_CONNECTED && g_mqtt_client &&
            (xTaskGetTickCount() - system_info_timer) > pdMS_TO_TICKS(30000)) {
            
            snprintf(system_info, sizeof(system_info), 
                "{\"device\":\"grid_frequency_monitor\",\"ip\":\"%s\",\"uptime\":%lu,\"free_heap\":%lu,\"timestamp\":%llu}",
                handle->ip_address, 
                xTaskGetTickCount() * portTICK_PERIOD_MS / 1000, 
                esp_get_free_heap_size(),
                time(NULL));
            
            esp_mqtt_client_publish(g_mqtt_client, handle->mqtt_topic_system, system_info, 0, 0, 0);
            system_info_timer = xTaskGetTickCount();
            ESP_LOGD(TAG, "Published system info to MQTT");
        }
    }
    
    // Clean up remaining messages in queue
    while (xQueueReceive(handle->log_queue, &log_msg, 0) == pdTRUE) {
        if (log_msg.msg != NULL) {
            free(log_msg.msg);
        }
        if (log_msg.topic != NULL) {
            free(log_msg.topic);
        }
    }
    
    ESP_LOGI(TAG, "MQTT logging task stopped");
    vTaskDelete(NULL);
}

// Helper function to convert OTA state enum to string
static const char* ota_state_to_string(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW:
            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY:
            return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID:
            return "VALID";
        case ESP_OTA_IMG_INVALID:
            return "INVALID";
        case ESP_OTA_IMG_ABORTED:
            return "ABORTED";
        case ESP_OTA_IMG_UNDEFINED:
            return "UNDEFINED";
        default:
            return "UNKNOWN";
    }
}

// SNTP time synchronization callback
static void sntp_sync_notification_cb(struct timeval *tv) {
    g_time_synced = true;
    ESP_LOGI(TAG, "Time synchronized via SNTP");
}

// Initialize SNTP for time synchronization
esp_err_t network_init_sntp(void) {
    ESP_LOGI(TAG, "Initializing SNTP");
    
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_notification_cb);
    esp_sntp_init();
    
    // Wait for time to be synchronized (max 10 seconds)
    int retry = 0;
    const int retry_count = 100;
    while (!g_time_synced && ++retry < retry_count) {
        ESP_LOGD(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    if (g_time_synced) {
        ESP_LOGI(TAG, "Time synchronized successfully");
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed to synchronize time within timeout");
        return ESP_ERR_TIMEOUT;
    }
}

// Get current time in milliseconds since Unix epoch
int64_t network_get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
}

// Measurement publishing task
static void measurement_publishing_task(void *pvParameters) {
    network_handle_t *handle = (network_handle_t *)pvParameters;
    measurement_t measurement;
    
    ESP_LOGI(TAG, "Measurement publishing task started");
    
    while (handle->measurement_publishing_enabled) {
        // Wait for measurement data
        if (xQueueReceive(handle->measurement_queue, &measurement, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (handle->status == WIFI_STATUS_CONNECTED && g_mqtt_client) {
                // Create JSON payload
                cJSON *json = cJSON_CreateObject();
                if (json != NULL) {
                    cJSON_AddNumberToObject(json, "timestamp", measurement.timestamp_us);
                    cJSON_AddNumberToObject(json, "frequency", measurement.frequency);
                    cJSON_AddNumberToObject(json, "voltage", measurement.voltage);
                    
                    char *json_string = cJSON_Print(json);
                    if (json_string != NULL) {
                        // Publish to MQTT
                        esp_mqtt_client_publish(g_mqtt_client, handle->mqtt_topic_measurement, json_string, 0, 0, 0);
                        free(json_string);
                    }
                    cJSON_Delete(json);
                }
            }
        }
    }
    
    // Clean up remaining measurements in queue
    while (xQueueReceive(handle->measurement_queue, &measurement, 0) == pdTRUE) {
        // Just drain the queue
    }
    
    ESP_LOGI(TAG, "Measurement publishing task stopped");
    vTaskDelete(NULL);
}

// Initialize network
esp_err_t network_init(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memset(handle, 0, sizeof(network_handle_t));
    g_network_handle = handle;
    
    // Create log queue
    handle->log_queue = xQueueCreate(MQTT_QUEUE_SIZE, sizeof(log_message_t));
    if (!handle->log_queue) {
        ESP_LOGE(TAG, "Failed to create log queue");
        return ESP_ERR_NO_MEM;
    }
    g_log_queue = handle->log_queue;
    
    // Create measurement queue
    handle->measurement_queue = xQueueCreate(MEASUREMENT_QUEUE_SIZE, sizeof(measurement_t));
    if (!handle->measurement_queue) {
        ESP_LOGE(TAG, "Failed to create measurement queue");
        vQueueDelete(handle->log_queue);
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize log buffer for pre-MQTT logs
    esp_err_t ret = network_init_log_buffer(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize log buffer");
        vQueueDelete(handle->log_queue);
        vQueueDelete(handle->measurement_queue);
        return ret;
    }
    
    // Get MAC address and initialize MQTT client ID and topics
    ret = network_get_formatted_mac_address(handle->mac_address, sizeof(handle->mac_address));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get MAC address");
        vQueueDelete(handle->log_queue);
        vQueueDelete(handle->measurement_queue);
        network_deinit_log_buffer(handle);
        return ret;
    }
    
    // Set MQTT client ID using MAC address
    snprintf(handle->mqtt_client_id, sizeof(handle->mqtt_client_id), "grid_monitor_%s", handle->mac_address);
    
    // Initialize MQTT topics with MAC address as second element
    snprintf(handle->mqtt_topic_logs, sizeof(handle->mqtt_topic_logs), "%s/%s/logs", MQTT_TOPIC_BASE, handle->mac_address);
    snprintf(handle->mqtt_topic_status, sizeof(handle->mqtt_topic_status), "%s/%s/status", MQTT_TOPIC_BASE, handle->mac_address);
    snprintf(handle->mqtt_topic_measurement, sizeof(handle->mqtt_topic_measurement), "%s/%s/measurement", MQTT_TOPIC_BASE, handle->mac_address);
    snprintf(handle->mqtt_topic_system, sizeof(handle->mqtt_topic_system), "%s/%s/system", MQTT_TOPIC_BASE, handle->mac_address);
    snprintf(handle->mqtt_topic_error, sizeof(handle->mqtt_topic_error), "%s/%s/error", MQTT_TOPIC_BASE, handle->mac_address);
    snprintf(handle->mqtt_topic_debug, sizeof(handle->mqtt_topic_debug), "%s/%s/debug", MQTT_TOPIC_BASE, handle->mac_address);
    snprintf(handle->mqtt_topic_command, sizeof(handle->mqtt_topic_command), "%s/%s/command", MQTT_TOPIC_BASE, handle->mac_address);
    snprintf(handle->mqtt_topic_coredump, sizeof(handle->mqtt_topic_coredump), "%s/%s/coredump", MQTT_TOPIC_BASE, handle->mac_address);
    snprintf(handle->mqtt_topic_firmware, sizeof(handle->mqtt_topic_firmware), "%s/%s/firmware", MQTT_TOPIC_BASE, handle->mac_address);
    
    ESP_LOGI(TAG, "MAC address: %s", handle->mac_address);
    ESP_LOGI(TAG, "MQTT client ID: %s", handle->mqtt_client_id);
    
    // Don't setup log forwarding here to prevent stack overflow during WiFi init
    // Log forwarding will be set up when MQTT logging starts
    
    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Create event group
    handle->wifi_event_group = xEventGroupCreate();
    if (!handle->wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        vQueueDelete(handle->log_queue);
        vQueueDelete(handle->measurement_queue);
        return ESP_ERR_NO_MEM;
    }
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, handle, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, handle, NULL));
    
    handle->status = WIFI_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "Network initialized");
    
    return ESP_OK;
}

// Deinitialize network
esp_err_t network_deinit(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    network_stop_log_forwarding(handle);
    network_stop_measurement_publishing(handle);
    network_stop_mqtt_logging(handle);
    network_stop_ota(handle);
    network_stop_wifi(handle);
    
    // Clean up rollback check task
    if (g_rollback_check_task) {
        vTaskDelete(g_rollback_check_task);
        g_rollback_check_task = NULL;
    }
    
    if (handle->wifi_event_group) {
        vEventGroupDelete(handle->wifi_event_group);
    }
    
    if (handle->log_queue) {
        // Clean up any remaining messages
        log_message_t log_msg;
        while (xQueueReceive(handle->log_queue, &log_msg, 0) == pdTRUE) {
            if (log_msg.msg != NULL) {
                free(log_msg.msg);
            }
            if (log_msg.topic != NULL) {
                free(log_msg.topic);
            }
        }
        vQueueDelete(handle->log_queue);
        handle->log_queue = NULL;
        g_log_queue = NULL;
    }
    
    if (handle->measurement_queue) {
        // Clean up any remaining measurements
        measurement_t measurement;
        while (xQueueReceive(handle->measurement_queue, &measurement, 0) == pdTRUE) {
            // Just drain the queue
        }
        vQueueDelete(handle->measurement_queue);
        handle->measurement_queue = NULL;
    }
    
    // Cleanup log buffer
    network_deinit_log_buffer(handle);
    
    esp_wifi_deinit();
    esp_netif_deinit();
    
    g_network_handle = NULL;
    ESP_LOGI(TAG, "Network deinitialized");
    
    return ESP_OK;
}

// Start WiFi
esp_err_t network_start_wifi(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi started, connecting to %s...", WIFI_SSID);
    
    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(handle->wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE,
                                          pdFALSE,
                                          portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi SSID: %s", WIFI_SSID);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi SSID: %s", WIFI_SSID);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Unexpected WiFi event");
        return ESP_FAIL;
    }
}

// Stop WiFi
esp_err_t network_stop_wifi(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (handle->status == WIFI_STATUS_CONNECTED) {
        ESP_LOGI(TAG, "Disconnecting WiFi gracefully...");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for graceful disconnect
    }
    
    esp_wifi_stop();
    handle->status = WIFI_STATUS_DISCONNECTED;
    ESP_LOGI(TAG, "WiFi stopped");
    
    return ESP_OK;
}

// Start OTA server
esp_err_t network_start_ota(network_handle_t *handle) {
    if (!handle || handle->status != WIFI_STATUS_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = OTA_SERVER_PORT;
    config.max_uri_handlers = 1;
    config.stack_size = 8192;
    
    httpd_uri_t ota_uri = {
        .uri = OTA_UPDATE_PATH,
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL
    };
    
    if (httpd_start(&g_ota_server, &config) == ESP_OK) {
        httpd_register_uri_handler(g_ota_server, &ota_uri);
        handle->ota_enabled = true;
        ESP_LOGI(TAG, "OTA server started on port %d", OTA_SERVER_PORT);
        ESP_LOGI(TAG, "Upload firmware via: curl -X POST --data-binary @firmware.bin http://%s:%d%s", 
                 handle->ip_address, OTA_SERVER_PORT, OTA_UPDATE_PATH);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to start OTA server");
    return ESP_FAIL;
}

// Stop OTA server
esp_err_t network_stop_ota(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_ota_server) {
        httpd_stop(g_ota_server);
        g_ota_server = NULL;
        handle->ota_enabled = false;
        ESP_LOGI(TAG, "OTA server stopped");
    }
    
    return ESP_OK;
}

// Start UDP logging
// Start MQTT logging
esp_err_t network_start_mqtt_logging(network_handle_t *handle) {
    if (!handle || handle->status != WIFI_STATUS_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_mqtt_log_task) {
        ESP_LOGW(TAG, "MQTT logging already started");
        return ESP_OK;
    }
    
    // Configure MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .broker.address.port = MQTT_PORT,
        .credentials.client_id = handle->mqtt_client_id,
        .session.keepalive = MQTT_KEEPALIVE,
    };
    
    // Add authentication if username/password are provided
    if (strcmp(MQTT_USERNAME, "your_mqtt_username") != 0 && strlen(MQTT_USERNAME) > 0) {
        mqtt_cfg.credentials.username = MQTT_USERNAME;
        ESP_LOGI(TAG, "MQTT authentication enabled for user: %s", MQTT_USERNAME);
    }
    
    if (strcmp(MQTT_PASSWORD, "your_mqtt_password") != 0 && strlen(MQTT_PASSWORD) > 0) {
        mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    }
    
    g_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!g_mqtt_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }
    
    esp_mqtt_client_register_event(g_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    
    esp_err_t err = esp_mqtt_client_start(g_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
        return err;
    }
    
    handle->mqtt_logging_enabled = true;
    
    // Setup log forwarding to capture all ESP-IDF logs
    esp_err_t ret = network_setup_log_forwarding(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup log forwarding");
        handle->mqtt_logging_enabled = false;
        esp_mqtt_client_stop(g_mqtt_client);
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
        return ret;
    }
    
    BaseType_t task_ret = xTaskCreate(mqtt_logging_task, "mqtt_log", MQTT_TASK_STACK_SIZE, handle, MQTT_TASK_PRIORITY, &g_mqtt_log_task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQTT logging task");
        handle->mqtt_logging_enabled = false;
        network_stop_log_forwarding(handle);
        esp_mqtt_client_stop(g_mqtt_client);
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "MQTT logging started, publishing to %s", MQTT_BROKER_URI);
    return ESP_OK;
}

// Stop MQTT logging
esp_err_t network_stop_mqtt_logging(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    handle->mqtt_logging_enabled = false;
    
    // Stop log forwarding first
    network_stop_log_forwarding(handle);
    
    // Give the logging task time to process remaining messages
    if (g_mqtt_log_task) {
        ESP_LOGI(TAG, "Waiting for MQTT logging task to finish...");
        vTaskDelay(pdMS_TO_TICKS(500)); // Give time for queue to drain
        TaskHandle_t task_to_delete = g_mqtt_log_task;
        g_mqtt_log_task = NULL;  // Clear the global first
        vTaskDelete(task_to_delete);
    }
    
    // Stop MQTT client gracefully
    if (g_mqtt_client) {
        ESP_LOGI(TAG, "Stopping MQTT client gracefully...");
        esp_mqtt_client_stop(g_mqtt_client);
        vTaskDelay(pdMS_TO_TICKS(500)); // Allow time for proper disconnect
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
    }
    
    ESP_LOGI(TAG, "MQTT logging stopped");
    return ESP_OK;
}

// Start MQTT command handling
esp_err_t network_start_mqtt_commands(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!g_mqtt_client) {
        ESP_LOGE(TAG, "MQTT client not initialized. Start MQTT logging first.");
        return ESP_ERR_INVALID_STATE;
    }
    
    handle->mqtt_commands_enabled = true;
    
    // Subscription will happen in the MQTT_EVENT_CONNECTED handler
    // when the client connects or reconnects
    
    ESP_LOGI(TAG, "MQTT command handling enabled");
    return ESP_OK;
}

// Stop MQTT command handling
esp_err_t network_stop_mqtt_commands(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    handle->mqtt_commands_enabled = false;
    
    // Unsubscribe from command topic if MQTT client is available
    if (g_mqtt_client) {
        esp_mqtt_client_unsubscribe(g_mqtt_client, handle->mqtt_topic_command);
        ESP_LOGI(TAG, "Requested unsubscribe from command topic");
    }
    
    ESP_LOGI(TAG, "MQTT command handling disabled");
    return ESP_OK;
}

// Get WiFi status
wifi_status_t network_get_wifi_status(network_handle_t *handle) {
    return handle ? handle->status : WIFI_STATUS_DISCONNECTED;
}

// Get IP address
const char* network_get_ip_address(network_handle_t *handle) {
    return handle ? handle->ip_address : "0.0.0.0";
}

// Check if connected
bool network_is_connected(network_handle_t *handle) {
    return handle && handle->status == WIFI_STATUS_CONNECTED;
}

// UDP log function
// Setup log forwarding
esp_err_t network_setup_log_forwarding(network_handle_t *handle) {
    if (!handle || !handle->log_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Save original log function
    g_original_log_function = esp_log_set_vprintf(custom_log_writer);
    g_log_forwarding_initialized = true;
    
    ESP_LOGI(TAG, "Log forwarding enabled - all logs will be sent via MQTT");
    return ESP_OK;
}

// Stop log forwarding
esp_err_t network_stop_log_forwarding(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    g_log_forwarding_initialized = false;
    
    // Restore original log function
    if (g_original_log_function) {
        esp_log_set_vprintf(g_original_log_function);
        g_original_log_function = NULL;
    }
    
    ESP_LOGI(TAG, "Log forwarding disabled");
    return ESP_OK;
}

// Broadcast system info
void network_broadcast_system_info(network_handle_t *handle) {
    if (!handle || !handle->mqtt_logging_enabled) {
        return;
    }
    
    // This function can be called to manually trigger a system info broadcast
    // The automatic broadcasting is handled by the MQTT logging task
}

// Rollback check task - runs after OTA update to validate firmware
static void rollback_check_task(void *pvParameters) {
    ESP_LOGI(TAG, "Rollback check task started - waiting for %d seconds before validating it", OTA_VALIDATION_TIMEOUT / 1000);
    
    // Wait for the validation timeout
    vTaskDelay(pdMS_TO_TICKS(OTA_VALIDATION_TIMEOUT));
    
    // If we reach here, the firmware did not crash and we assume it is valid.
    ESP_LOGI(TAG, "Firmware validation timeout reached - marking firmware as valid");
    
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mark firmware as valid: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Firmware marked as valid successfully");
    }
    
    // This should never be reached
    vTaskDelete(NULL);
}

// Check OTA rollback status and handle post-update validation
esp_err_t network_check_ota_rollback(void) {
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(running_partition, &ota_state);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Current OTA state: %s (%d)", ota_state_to_string(ota_state), ota_state);
        
        switch (ota_state) {
            case ESP_OTA_IMG_NEW:
                // This should rarely happen as bootloader changes it to PENDING_VERIFY
                ESP_LOGW(TAG, "Running new firmware (%s) - validation required within %d seconds", 
                         ota_state_to_string(ota_state), OTA_VALIDATION_TIMEOUT / 1000);
                
                // Log current app information
                const esp_app_desc_t *app_desc = esp_app_get_description();
                ESP_LOGI(TAG, "App version: %s", app_desc->version);
                ESP_LOGI(TAG, "Compile time: %s %s", app_desc->date, app_desc->time);
                ESP_LOGI(TAG, "IDF version: %s", app_desc->idf_ver);
                
                // Start the rollback check task
                network_schedule_rollback_check();
                break;
                
            case ESP_OTA_IMG_PENDING_VERIFY:
                ESP_LOGW(TAG, "Running new firmware requiring validation (%s)", ota_state_to_string(ota_state));
                ESP_LOGW(TAG, "Must validate within %d seconds or rollback will occur on next reboot", 
                         OTA_VALIDATION_TIMEOUT / 1000);
                
                // Log current app information
                const esp_app_desc_t *app_desc_pending = esp_app_get_description();
                ESP_LOGI(TAG, "App version: %s", app_desc_pending->version);
                ESP_LOGI(TAG, "Compile time: %s %s", app_desc_pending->date, app_desc_pending->time);
                ESP_LOGI(TAG, "IDF version: %s", app_desc_pending->idf_ver);
                
                // Start the rollback check task to validate within timeout
                network_schedule_rollback_check();
                break;
                
            case ESP_OTA_IMG_VALID:
                ESP_LOGI(TAG, "Running validated firmware (%s)", ota_state_to_string(ota_state));
                break;
                
            case ESP_OTA_IMG_INVALID:
                ESP_LOGE(TAG, "Running invalid firmware (%s) - this should not happen", ota_state_to_string(ota_state));
                break;
                
            case ESP_OTA_IMG_ABORTED:
                ESP_LOGW(TAG, "Previous OTA was aborted (%s)", ota_state_to_string(ota_state));
                break;
                
            default:
                ESP_LOGW(TAG, "Unknown OTA state: %s (%d)", ota_state_to_string(ota_state), ota_state);
                break;
        }
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "OTA state not supported on this partition - most likely it is the factory one");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to get OTA state: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

// Schedule rollback check task
void network_schedule_rollback_check(void) {
    if (g_rollback_check_task) {
        ESP_LOGW(TAG, "Rollback check task already running");
        return;
    }

    BaseType_t ret = xTaskCreate(
        rollback_check_task, 
        ROLLBACK_TASK_NAME, 
        ROLLBACK_TASK_STACK_SIZE, 
        NULL, 
        ROLLBACK_TASK_PRIORITY, 
        &g_rollback_check_task
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create rollback check task");
    } else {
        ESP_LOGI(TAG, "Rollback check task scheduled");
    }
}

// MQTT OTA implementation with progress updates
static esp_err_t perform_mqtt_ota(const char *url)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *ota_partition = NULL;
    esp_err_t err;
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    // Send initial status
    if (g_mqtt_client && g_network_handle) {
        esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_status, "Connecting to OTA server...", 0, 0, 0);
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGE(TAG, "Invalid content length: %d", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Send download start status
    if (g_mqtt_client && g_network_handle) {
        char status_msg[128];
        snprintf(status_msg, sizeof(status_msg), "Downloading firmware (%d bytes)...", content_length);
        esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_status, status_msg, 0, 0, 0);
    }

    // Get the next update partition
    ota_partition = esp_ota_get_next_update_partition(NULL);
    if (ota_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Starting OTA update to partition: %s", ota_partition->label);

    err = esp_ota_begin(ota_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    char *upgrade_data_buf = malloc(1024);
    if (upgrade_data_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate upgrade data buffer");
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int binary_file_length = 0;
    int last_progress_report = 0;
    
    while (true) {
        int data_read = esp_http_client_read(client, upgrade_data_buf, 1024);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Data read error");
            break;
        } else if (data_read > 0) {
            err = esp_ota_write(ota_handle, (const void *)upgrade_data_buf, data_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                break;
            }
            binary_file_length += data_read;
            
            // Send progress updates via MQTT every 10%
            int progress = (binary_file_length * 100) / content_length;
            if (progress >= last_progress_report + 10) {
                last_progress_report = progress;
                if (g_mqtt_client && g_network_handle) {
                    char progress_msg[128];
                    snprintf(progress_msg, sizeof(progress_msg), "OTA Progress: %d%% (%d/%d bytes)", 
                            progress, binary_file_length, content_length);
                    esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_status, progress_msg, 0, 0, 0);
                }
                ESP_LOGI(TAG, "OTA Progress: %d%%", progress);
            }
        } else if (data_read == 0) {
            ESP_LOGI(TAG, "Connection closed, all data received");
            break;
        }
    }

    free(upgrade_data_buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (binary_file_length != content_length) {
        ESP_LOGE(TAG, "Incomplete download: %d/%d bytes", binary_file_length, content_length);
        esp_ota_abort(ota_handle);
        if (g_mqtt_client && g_network_handle) {
            esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_error, "OTA download incomplete", 0, 0, 0);
        }
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        if (g_mqtt_client && g_network_handle) {
            char error_msg[128];
            snprintf(error_msg, sizeof(error_msg), "OTA finalization failed: %s", esp_err_to_name(err));
            esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_error, error_msg, 0, 0, 0);
        }
        return err;
    }

    err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        if (g_mqtt_client && g_network_handle) {
            char error_msg[128];
            snprintf(error_msg, sizeof(error_msg), "OTA set boot partition failed: %s", esp_err_to_name(err));
            esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_error, error_msg, 0, 0, 0);
        }
        return err;
    }

    ESP_LOGI(TAG, "OTA update successful, initiating graceful restart...");
    if (g_mqtt_client && g_network_handle) {
        esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_status, "OTA update completed successfully, restarting gracefully...", 0, 0, 0);
    }
    
    // Graceful shutdown and restart instead of immediate restart
    esp_err_t shutdown_err = network_graceful_shutdown_and_restart(g_network_handle, "OTA update completed");
    if (shutdown_err != ESP_OK) {
        ESP_LOGW(TAG, "Graceful shutdown failed, performing immediate restart");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    
    return ESP_OK;
}

// Graceful shutdown function - properly closes all network connections
esp_err_t network_graceful_shutdown(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting graceful network shutdown...");
    TickType_t shutdown_start = xTaskGetTickCount();
    const TickType_t max_shutdown_time = pdMS_TO_TICKS(GRACEFUL_SHUTDOWN_TIMEOUT_MS);

    // Send final status message if MQTT is available
    if (g_mqtt_client && handle->mqtt_logging_enabled && g_network_handle) {
        esp_mqtt_client_publish(g_mqtt_client, g_network_handle->mqtt_topic_status, "Network shutting down gracefully...", 0, 0, 0);
        // Allow time for the message to be sent
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // Stop log forwarding first to prevent new log messages during shutdown
    ESP_LOGI(TAG, "Stopping log forwarding...");
    network_stop_log_forwarding(handle);

    // Check timeout
    if ((xTaskGetTickCount() - shutdown_start) > max_shutdown_time) {
        ESP_LOGW(TAG, "Graceful shutdown timeout exceeded, aborting remaining steps");
        return ESP_ERR_TIMEOUT;
    }

    // Stop MQTT command handling
    if (handle->mqtt_commands_enabled) {
        ESP_LOGI(TAG, "Stopping MQTT commands...");
        network_stop_mqtt_commands(handle);
    }

    // Stop measurement publishing
    if (handle->measurement_publishing_enabled) {
        ESP_LOGI(TAG, "Stopping measurement publishing...");
        network_stop_measurement_publishing(handle);
    }

    // Check timeout
    if ((xTaskGetTickCount() - shutdown_start) > max_shutdown_time) {
        ESP_LOGW(TAG, "Graceful shutdown timeout exceeded, aborting remaining steps");
        return ESP_ERR_TIMEOUT;
    }

    // Stop MQTT logging with proper cleanup
    if (handle->mqtt_logging_enabled) {
        ESP_LOGI(TAG, "Stopping MQTT logging...");
        handle->mqtt_logging_enabled = false;
        
        // Wait for logging task to stop
        if (g_mqtt_log_task) {
            vTaskDelay(pdMS_TO_TICKS(100)); // Give task time to exit gracefully
        }
        
        // Stop MQTT client gracefully
        if (g_mqtt_client) {
            ESP_LOGI(TAG, "Disconnecting MQTT client...");
            esp_mqtt_client_stop(g_mqtt_client);
            vTaskDelay(pdMS_TO_TICKS(300)); // Wait for graceful disconnect
            esp_mqtt_client_destroy(g_mqtt_client);
            g_mqtt_client = NULL;
        }
        
        // Clean up logging task
        if (g_mqtt_log_task) {
            TaskHandle_t task_to_delete = g_mqtt_log_task;
            g_mqtt_log_task = NULL;  // Clear the global first
            vTaskDelete(task_to_delete);
        }
    }

    // Check timeout
    if ((xTaskGetTickCount() - shutdown_start) > max_shutdown_time) {
        ESP_LOGW(TAG, "Graceful shutdown timeout exceeded, aborting remaining steps");
        return ESP_ERR_TIMEOUT;
    }

    // Stop OTA server
    if (handle->ota_enabled) {
        ESP_LOGI(TAG, "Stopping OTA server...");
        network_stop_ota(handle);
    }

    // Disconnect WiFi gracefully
    if (handle->status == WIFI_STATUS_CONNECTED) {
        ESP_LOGI(TAG, "Disconnecting WiFi...");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300)); // Wait for disconnect
        esp_wifi_stop();
        handle->status = WIFI_STATUS_DISCONNECTED;
    }

    // Clean up rollback check task
    if (g_rollback_check_task) {
        vTaskDelete(g_rollback_check_task);
        g_rollback_check_task = NULL;
    }

    ESP_LOGI(TAG, "Graceful network shutdown completed in %lu ms", 
             (xTaskGetTickCount() - shutdown_start) * portTICK_PERIOD_MS);
    return ESP_OK;
}

// Graceful shutdown and restart function
esp_err_t network_graceful_shutdown_and_restart(network_handle_t *handle, const char *reason) {
    ESP_LOGI(TAG, "Initiating graceful restart. Reason: %s", reason ? reason : "Unknown");
    
    // Perform graceful shutdown
    esp_err_t err = network_graceful_shutdown(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Graceful shutdown failed: %s", esp_err_to_name(err));
    }
    
    // Final delay before restart to ensure all operations complete
    ESP_LOGI(TAG, "Restarting system in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    esp_restart();
    return ESP_OK; // This line will never be reached
}

// Start measurement publishing
esp_err_t network_start_measurement_publishing(network_handle_t *handle) {
    if (!handle || handle->status != WIFI_STATUS_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (g_measurement_task) {
        ESP_LOGW(TAG, "Measurement publishing already started");
        return ESP_OK;
    }
    
    if (!g_mqtt_client) {
        ESP_LOGE(TAG, "MQTT client not initialized. Start MQTT logging first.");
        return ESP_ERR_INVALID_STATE;
    }
    
    handle->measurement_publishing_enabled = true;
    
    BaseType_t task_ret = xTaskCreate(measurement_publishing_task, "measurement_pub", 
                                     MEASUREMENT_TASK_STACK_SIZE, handle, 
                                     MEASUREMENT_TASK_PRIORITY, &g_measurement_task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create measurement publishing task");
        handle->measurement_publishing_enabled = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Measurement publishing started");
    return ESP_OK;
}

// Stop measurement publishing
esp_err_t network_stop_measurement_publishing(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    handle->measurement_publishing_enabled = false;
    
    if (g_measurement_task) {
        ESP_LOGI(TAG, "Stopping measurement publishing task...");
        // Give time for task to exit gracefully
        vTaskDelay(pdMS_TO_TICKS(200)); 
        
        // Check if task is still running before attempting to delete
        if (g_measurement_task) {
            TaskHandle_t task_to_delete = g_measurement_task;
            g_measurement_task = NULL;  // Clear the global first
            vTaskDelete(task_to_delete);
        }
    }
    
    ESP_LOGI(TAG, "Measurement publishing stopped");
    return ESP_OK;
}

// Queue a measurement for MQTT publishing
esp_err_t network_queue_measurement(network_handle_t *handle, const measurement_t *measurement) {
    if (!handle || !measurement) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!handle->measurement_publishing_enabled || !handle->measurement_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Try to send measurement to queue (non-blocking)
    if (xQueueSend(handle->measurement_queue, measurement, 0) != pdTRUE) {
        // Queue is full, measurement dropped
        ESP_LOGD(TAG, "Measurement queue full, dropping measurement");
        return ESP_ERR_NO_MEM;
    }
    
    return ESP_OK;
}

// Get measurement queue handle
QueueHandle_t network_get_measurement_queue(network_handle_t *handle) {
    if (!handle) {
        return NULL;
    }
    return handle->measurement_queue;
}

// Initialize log buffer for capturing logs before MQTT connection
esp_err_t network_init_log_buffer(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    handle->log_buffer = malloc(sizeof(log_buffer_t));
    if (!handle->log_buffer) {
        ESP_LOGE(TAG, "Failed to allocate log buffer");
        return ESP_ERR_NO_MEM;
    }
    
    memset(handle->log_buffer, 0, sizeof(log_buffer_t));
    ESP_LOGI(TAG, "Log buffer initialized (size: %d messages)", LOG_BUFFER_SIZE);
    return ESP_OK;
}

// Add a log message to the buffer (called by custom log writer)
static void add_to_log_buffer(network_handle_t *handle, const char *message, const char *topic) {
    if (!handle || !handle->log_buffer || !message || !topic) {
        return;
    }
    
    log_buffer_t *buffer = handle->log_buffer;
    
    // Store the message
    strncpy(buffer->messages[buffer->write_index], message, LOG_BUFFER_MSG_SIZE - 1);
    buffer->messages[buffer->write_index][LOG_BUFFER_MSG_SIZE - 1] = '\0';
    
    // Store the topic
    strncpy(buffer->topics[buffer->write_index], topic, sizeof(buffer->topics[0]) - 1);
    buffer->topics[buffer->write_index][sizeof(buffer->topics[0]) - 1] = '\0';
    
    // Store timestamp
    gettimeofday(&buffer->timestamps[buffer->write_index], NULL);
    
    // Update indices
    buffer->write_index = (buffer->write_index + 1) % LOG_BUFFER_SIZE;
    
    if (buffer->count < LOG_BUFFER_SIZE) {
        buffer->count++;
    } else {
        buffer->overflow = true;
    }
}

// Flush all buffered logs to MQTT
esp_err_t network_flush_log_buffer(network_handle_t *handle) {
    if (!handle || !handle->log_buffer || !g_mqtt_client) {
        return ESP_ERR_INVALID_ARG;
    }
    
    log_buffer_t *buffer = handle->log_buffer;
    if (buffer->count == 0) {
        return ESP_OK;
    }
    
    ESP_LOGD(TAG, "Flushing %d buffered log messages to MQTT", buffer->count);
    
    // Calculate start index for reading
    int start_index = buffer->overflow ? buffer->write_index : 0;
    int messages_to_send = buffer->overflow ? LOG_BUFFER_SIZE : buffer->count;
    
    for (int i = 0; i < messages_to_send; i++) {
        int index = (start_index + i) % LOG_BUFFER_SIZE;
        
        // Create a JSON message with timestamp
        cJSON *json = cJSON_CreateObject();
        if (json) {
            cJSON_AddStringToObject(json, "message", buffer->messages[index]);
            cJSON_AddNumberToObject(json, "timestamp", 
                buffer->timestamps[index].tv_sec * 1000LL + buffer->timestamps[index].tv_usec / 1000);
            cJSON_AddStringToObject(json, "source", "buffered");
            
            char *json_string = cJSON_Print(json);
            if (json_string) {
                esp_mqtt_client_publish(g_mqtt_client, buffer->topics[index], json_string, 0, 1, 0);
                free(json_string);
            }
            cJSON_Delete(json);
        }
        
        // Small delay to avoid overwhelming MQTT broker
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Clear the buffer
    buffer->count = 0;
    buffer->write_index = 0;
    buffer->overflow = false;
    
    ESP_LOGI(TAG, "Log buffer flushed successfully");
    return ESP_OK;
}

// Cleanup log buffer
esp_err_t network_deinit_log_buffer(network_handle_t *handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (handle->log_buffer) {
        free(handle->log_buffer);
        handle->log_buffer = NULL;
        ESP_LOGI(TAG, "Log buffer deinitialized");
    }
    
    return ESP_OK;
}

// Check for core dumps and publish if available
esp_err_t network_check_and_publish_coredump(network_handle_t *handle) {
    if (!handle || !g_mqtt_client) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check if core dump partition exists
    const esp_partition_t *core_dump_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    
    if (!core_dump_partition) {
        ESP_LOGD(TAG, "No core dump partition found");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Found core dump partition: size=%lu bytes, address=0x%lx", 
             core_dump_partition->size, core_dump_partition->address);
    
    // Read the first few bytes to check if there's data
    uint8_t header[16];
    esp_err_t err = esp_partition_read(core_dump_partition, 0, header, sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read core dump partition header: %s", esp_err_to_name(err));
        return err;
    }
    
    // Check if partition contains data (not all 0xFF)
    bool has_data = false;
    for (int i = 0; i < sizeof(header); i++) {
        if (header[i] != 0xFF) {
            has_data = true;
            break;
        }
    }
    
    if (!has_data) {
        ESP_LOGD(TAG, "No core dump data found (partition is empty)");
        return ESP_OK;
    }
    
    ESP_LOGW(TAG, "Core dump data detected! Publishing to MQTT...");
    
    // Send header info first
    cJSON *header_json = cJSON_CreateObject();
    if (!header_json) {
        ESP_LOGE(TAG, "Failed to create JSON for core dump header");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddNumberToObject(header_json, "timestamp", network_get_time_ms());
    cJSON_AddStringToObject(header_json, "type", "core_dump_start");
    cJSON_AddNumberToObject(header_json, "partition_size", core_dump_partition->size);
    cJSON_AddNumberToObject(header_json, "partition_address", core_dump_partition->address);
    
    // Add reset reason
    esp_reset_reason_t reset_reason = esp_reset_reason();
    const char* reset_reason_str = "unknown";
    switch (reset_reason) {
        case ESP_RST_POWERON: reset_reason_str = "power_on"; break;
        case ESP_RST_EXT: reset_reason_str = "external_reset"; break;
        case ESP_RST_SW: reset_reason_str = "software_reset"; break;
        case ESP_RST_PANIC: reset_reason_str = "panic"; break;
        case ESP_RST_INT_WDT: reset_reason_str = "interrupt_watchdog"; break;
        case ESP_RST_TASK_WDT: reset_reason_str = "task_watchdog"; break;
        case ESP_RST_WDT: reset_reason_str = "other_watchdog"; break;
        case ESP_RST_DEEPSLEEP: reset_reason_str = "deep_sleep"; break;
        case ESP_RST_BROWNOUT: reset_reason_str = "brownout"; break;
        case ESP_RST_SDIO: reset_reason_str = "sdio"; break;
        default: break;
    }
    cJSON_AddStringToObject(header_json, "reset_reason", reset_reason_str);
    
    // Add firmware info
    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(header_json, "firmware_version", app_desc->version);
    cJSON_AddStringToObject(header_json, "compile_date", app_desc->date);
    cJSON_AddStringToObject(header_json, "compile_time", app_desc->time);
    
    char header_topic[128];
    snprintf(header_topic, sizeof(header_topic), "%s/header", handle->mqtt_topic_coredump);
    
    char *header_string = cJSON_Print(header_json);
    if (header_string) {
        esp_mqtt_client_publish(g_mqtt_client, header_topic, header_string, 0, 1, 0);
        ESP_LOGI(TAG, "Core dump header published");
        free(header_string);
    }
    cJSON_Delete(header_json);
    
    // Now send the actual core dump data in chunks
    const size_t chunk_size = 1024; // 1KB chunks to stay well under MQTT limits
    uint8_t *chunk_buffer = malloc(chunk_size);
    if (!chunk_buffer) {
        ESP_LOGE(TAG, "Failed to allocate chunk buffer for core dump");
        return ESP_ERR_NO_MEM;
    }
    
    size_t total_chunks = (core_dump_partition->size + chunk_size - 1) / chunk_size;
    ESP_LOGI(TAG, "Sending core dump in %zu chunks of %zu bytes each", total_chunks, chunk_size);
    
    for (size_t chunk = 0; chunk < total_chunks; chunk++) {
        size_t offset = chunk * chunk_size;
        size_t read_size = MIN(chunk_size, core_dump_partition->size - offset);
        
        // Read chunk from partition
        err = esp_partition_read(core_dump_partition, offset, chunk_buffer, read_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read core dump chunk %zu: %s", chunk, esp_err_to_name(err));
            break;
        }
        
        // Create JSON for this chunk
        cJSON *chunk_json = cJSON_CreateObject();
        if (!chunk_json) {
            ESP_LOGE(TAG, "Failed to create JSON for chunk %zu", chunk);
            break;
        }
        
        cJSON_AddStringToObject(chunk_json, "type", "core_dump_chunk");
        cJSON_AddNumberToObject(chunk_json, "chunk_index", chunk);
        cJSON_AddNumberToObject(chunk_json, "total_chunks", total_chunks);
        cJSON_AddNumberToObject(chunk_json, "offset", offset);
        cJSON_AddNumberToObject(chunk_json, "size", read_size);
        
        // Convert binary data to base64
        size_t base64_len = ((read_size + 2) / 3) * 4 + 1;
        char *base64_data = malloc(base64_len);
        if (base64_data) {
            // Simple base64 encoding (you might want to use a library function)
            size_t encoded_len = 0;
            esp_err_t encode_err = mbedtls_base64_encode((unsigned char *)base64_data, base64_len, &encoded_len, chunk_buffer, read_size);
            if (encode_err == ESP_OK) {
                cJSON_AddStringToObject(chunk_json, "data", base64_data);
                
                char *chunk_string = cJSON_Print(chunk_json);
                if (chunk_string) {
                    char topic[128];
                    snprintf(topic, sizeof(topic), "%s/chunk/%zu", handle->mqtt_topic_coredump, chunk);
                    esp_mqtt_client_publish(g_mqtt_client, topic, chunk_string, 0, 1, 0);
                    free(chunk_string);
                }
            } else {
                ESP_LOGE(TAG, "Failed to encode chunk %zu to base64", chunk);
            }
            free(base64_data);
        }
        
        cJSON_Delete(chunk_json);
        
        // Progress logging
        if (chunk % 10 == 0 || chunk == total_chunks - 1) {
            ESP_LOGI(TAG, "Core dump progress: %zu/%zu chunks sent (%.1f%%)", 
                     chunk + 1, total_chunks, ((float)(chunk + 1) / total_chunks) * 100.0);
        }
        
        // Small delay to avoid overwhelming the MQTT broker
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    free(chunk_buffer);
    
    // Send completion message
    cJSON *completion_json = cJSON_CreateObject();
    if (completion_json) {
        cJSON_AddStringToObject(completion_json, "type", "core_dump_complete");
        cJSON_AddNumberToObject(completion_json, "total_chunks", total_chunks);
        cJSON_AddNumberToObject(completion_json, "total_size", core_dump_partition->size);
        
        char complete_topic[128];
        snprintf(complete_topic, sizeof(complete_topic), "%s/complete", handle->mqtt_topic_coredump);
        
        char *completion_string = cJSON_Print(completion_json);
        if (completion_string) {
            esp_mqtt_client_publish(g_mqtt_client, complete_topic, completion_string, 0, 1, 0);
            free(completion_string);
        }
        cJSON_Delete(completion_json);
    }
    
    ESP_LOGW(TAG, "Core dump data published to MQTT successfully");
    
    // Optionally clear the core dump after reporting
    esp_core_dump_image_erase();
    
    return ESP_OK;
}

// Publish firmware version and OTA status
esp_err_t network_publish_firmware_info(network_handle_t *handle) {
    if (!handle || !g_mqtt_client) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Publishing firmware information...");
    
    const esp_app_desc_t *app_desc = esp_app_get_description();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    
    if (running_partition) {
        esp_ota_get_state_partition(running_partition, &ota_state);
    }
    
    // Create firmware info JSON
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        ESP_LOGE(TAG, "Failed to create JSON for firmware info");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddNumberToObject(json, "timestamp", network_get_time_ms());
    cJSON_AddStringToObject(json, "type", "firmware_info");
    cJSON_AddStringToObject(json, "version", app_desc->version);
    cJSON_AddStringToObject(json, "project_name", app_desc->project_name);
    cJSON_AddStringToObject(json, "compile_time", app_desc->time);
    cJSON_AddStringToObject(json, "compile_date", app_desc->date);
    cJSON_AddStringToObject(json, "idf_version", app_desc->idf_ver);
    
    // Add OTA state
    cJSON_AddStringToObject(json, "ota_state", ota_state_to_string(ota_state));
    
    // Add partition info
    if (running_partition) {
        cJSON_AddStringToObject(json, "partition_label", running_partition->label);
        cJSON_AddNumberToObject(json, "partition_address", running_partition->address);
        cJSON_AddNumberToObject(json, "partition_size", running_partition->size);
    }
    
    // Add reset reason
    esp_reset_reason_t reset_reason = esp_reset_reason();
    const char* reset_reason_str = "unknown";
    switch (reset_reason) {
        case ESP_RST_POWERON: reset_reason_str = "power_on"; break;
        case ESP_RST_EXT: reset_reason_str = "external_reset"; break;
        case ESP_RST_SW: reset_reason_str = "software_reset"; break;
        case ESP_RST_PANIC: reset_reason_str = "panic"; break;
        case ESP_RST_INT_WDT: reset_reason_str = "interrupt_watchdog"; break;
        case ESP_RST_TASK_WDT: reset_reason_str = "task_watchdog"; break;
        case ESP_RST_WDT: reset_reason_str = "other_watchdog"; break;
        case ESP_RST_DEEPSLEEP: reset_reason_str = "deep_sleep"; break;
        case ESP_RST_BROWNOUT: reset_reason_str = "brownout"; break;
        case ESP_RST_SDIO: reset_reason_str = "sdio"; break;
        default: break;
    }
    cJSON_AddStringToObject(json, "reset_reason", reset_reason_str);
    
    // Add uptime
    int64_t uptime_ms = esp_timer_get_time() / 1000;
    cJSON_AddNumberToObject(json, "uptime_ms", uptime_ms);
    
    // Add free heap
    cJSON_AddNumberToObject(json, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(json, "minimum_free_heap", esp_get_minimum_free_heap_size());
    
    char *json_string = cJSON_Print(json);
    if (json_string) {
        esp_mqtt_client_publish(g_mqtt_client, handle->mqtt_topic_firmware, json_string, 0, 1, 0);
        ESP_LOGI(TAG, "Firmware info published: version %s, OTA state %s", 
                 app_desc->version, ota_state_to_string(ota_state));
        free(json_string);
    }
    
    cJSON_Delete(json);
    return ESP_OK;
}

// Get MAC address formatted for MQTT (lowercase, no colons)
esp_err_t network_get_formatted_mac_address(char *mac_str, size_t mac_str_size) {
    if (!mac_str || mac_str_size < 13) {  // Need 12 chars + null terminator
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Format as lowercase hex without colons
    snprintf(mac_str, mac_str_size, "%02x%02x%02x%02x%02x%02x", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return ESP_OK;
}
