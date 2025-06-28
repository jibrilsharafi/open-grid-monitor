#pragma once

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_core_dump.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "mbedtls/base64.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>
#include <sys/time.h>
#include <time.h>

#include "secrets.h"
#include "struct.h"

// WiFi configuration
#define WIFI_MAXIMUM_RETRY      5
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1

// OTA configuration
#define OTA_SERVER_PORT         8080
#define OTA_UPDATE_PATH         "/update"
#define OTA_VALIDATION_TIMEOUT  10000  // Time required to validate new firmware

// Graceful shutdown configuration
#define GRACEFUL_SHUTDOWN_TIMEOUT_MS  10000  // Maximum time to wait for graceful shutdown

// MQTT logging configuration
// #define MQTT_BROKER_URI         "mqtt://192.168.2.41"
#define MQTT_BROKER_URI         "mqtt://192.168.2.78"
#define MQTT_PORT               1883
// MQTT Authentication - set to default values to disable, or change to your credentials
#define MQTT_KEEPALIVE          60
#define MQTT_TASK_STACK_SIZE    (32 * 1024)  // Increased for log processing
#define MQTT_TASK_PRIORITY      3
#define MQTT_QUEUE_SIZE         100
#define MQTT_MSG_MAX_SIZE       256

// MQTT Topics
#define MQTT_TOPIC_BASE         "open_grid_monitor"
#define MQTT_TOPIC_LOGS         "logs"
#define MQTT_TOPIC_STATUS       "status" 
#define MQTT_TOPIC_MEASUREMENT  "measurement"
#define MQTT_TOPIC_SYSTEM       "system"
#define MQTT_TOPIC_ERROR        "error"
#define MQTT_TOPIC_DEBUG        "debug"
#define MQTT_TOPIC_COMMAND      "command"
#define MQTT_TOPIC_COREDUMP     "coredump"
#define MQTT_TOPIC_HEADER       "header"
#define MQTT_TOPIC_CHUNK        "chunk"
#define MQTT_TOPIC_COMPLETE     "complete"
#define MQTT_TOPIC_FIRMWARE     "firmware"

// MQTT Commands
#define MQTT_COMMAND_RESTART    "restart"
#define MQTT_COMMAND_OTA        "ota"
#define MQTT_COMMAND_COREDUMP   "coredump"

// MQTT OTA command format: {"url": "http://ip:port/firmware.bin"}
#define MQTT_OTA_URL_MAX_LEN    256

// Log buffer configuration
#define LOG_BUFFER_SIZE         20    // Number of log messages to buffer
#define LOG_BUFFER_MSG_SIZE     128   // Maximum size per log message

// Rollback task configuration
#define ROLLBACK_TASK_NAME       "rollback_task"
#define ROLLBACK_TASK_STACK_SIZE (4 * 1024)
#define ROLLBACK_TASK_PRIORITY   3

// Deferred shutdown task configuration
#define DEFERRED_SHUTDOWN_TASK_NAME       "deferred_shutdown_task"
#define DEFERRED_SHUTDOWN_TASK_STACK_SIZE (4 * 1024)
#define DEFERRED_SHUTDOWN_TASK_PRIORITY   2

// Measurement queue configuration
#define MEASUREMENT_QUEUE_SIZE  100
#define MEASUREMENT_TASK_STACK_SIZE (8 * 1024)
#define MEASUREMENT_TASK_PRIORITY   5

// SNTP configuration
#define SNTP_SERVER             "pool.ntp.org"
#define SNTP_SYNC_INTERVAL_MS   3600000  // 1 hour

// Log message structure for MQTT forwarding
typedef struct {
    char *msg;
    char *topic;
    struct timeval timestamp;
} log_message_t;

// WiFi status
typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED
} wifi_status_t;

// Log buffer structure
typedef struct {
    char messages[LOG_BUFFER_SIZE][LOG_BUFFER_MSG_SIZE];
    char topics[LOG_BUFFER_SIZE][64];
    struct timeval timestamps[LOG_BUFFER_SIZE];
    int write_index;
    int count;
    bool overflow;
} log_buffer_t;

// Network handle structure
typedef struct {
    EventGroupHandle_t wifi_event_group;
    wifi_status_t status;
    bool ota_enabled;
    bool mqtt_logging_enabled;
    bool mqtt_commands_enabled;
    bool measurement_publishing_enabled;
    int retry_count;
    char ip_address[16];
    char mac_address[13];  // 12 chars for MAC + null terminator (lowercase, no colons)
    char mqtt_client_id[32];
    char mqtt_topic_logs[64];
    char mqtt_topic_status[64];
    char mqtt_topic_measurement[64];
    char mqtt_topic_system[64];
    char mqtt_topic_error[64];
    char mqtt_topic_debug[64];
    char mqtt_topic_command[64];
    char mqtt_topic_coredump[64];
    char mqtt_topic_firmware[64];
    QueueHandle_t log_queue;
    QueueHandle_t measurement_queue;
    log_buffer_t *log_buffer;
} network_handle_t;

// Function prototypes
esp_err_t network_init(network_handle_t *handle);
esp_err_t network_deinit(network_handle_t *handle);
esp_err_t network_start_wifi(network_handle_t *handle);
esp_err_t network_stop_wifi(network_handle_t *handle);
esp_err_t network_start_ota(network_handle_t *handle);
esp_err_t network_stop_ota(network_handle_t *handle);
esp_err_t network_start_mqtt_logging(network_handle_t *handle);
esp_err_t network_stop_mqtt_logging(network_handle_t *handle);

// Status functions
wifi_status_t network_get_wifi_status(network_handle_t *handle);
const char* network_get_ip_address(network_handle_t *handle);
bool network_is_connected(network_handle_t *handle);

// MQTT logging functions
esp_err_t network_setup_log_forwarding(network_handle_t *handle);
esp_err_t network_stop_log_forwarding(network_handle_t *handle);
void network_broadcast_system_info(network_handle_t *handle);

// MQTT command handling functions
esp_err_t network_start_mqtt_commands(network_handle_t *handle);
esp_err_t network_stop_mqtt_commands(network_handle_t *handle);

// MQTT measurement publishing functions
esp_err_t network_start_measurement_publishing(network_handle_t *handle);
esp_err_t network_stop_measurement_publishing(network_handle_t *handle);
esp_err_t network_queue_measurement(network_handle_t *handle, const measurement_t *measurement);

// Get measurement queue handle
QueueHandle_t network_get_measurement_queue(network_handle_t *handle);

// Time synchronization functions
esp_err_t network_init_sntp(void);
int64_t network_get_time_ms(void);

// OTA rollback functions
esp_err_t network_check_ota_rollback(void);
void network_schedule_rollback_check(void);

// Graceful shutdown and restart functions
esp_err_t network_graceful_shutdown_and_restart(network_handle_t *handle, const char *reason);
esp_err_t network_graceful_shutdown(network_handle_t *handle);
esp_err_t network_schedule_deferred_restart(const char *reason);
esp_err_t network_schedule_deferred_shutdown(const char *reason);

// Core dump and firmware functions
esp_err_t network_check_and_publish_coredump(network_handle_t *handle);
esp_err_t network_request_and_publish_coredump(network_handle_t *handle);
esp_err_t network_publish_firmware_info(network_handle_t *handle);

// Log buffering functions for pre-MQTT logs
esp_err_t network_init_log_buffer(network_handle_t *handle);
esp_err_t network_flush_log_buffer(network_handle_t *handle);
esp_err_t network_deinit_log_buffer(network_handle_t *handle);

// Get MAC address formatted for MQTT (lowercase, no colons)
esp_err_t network_get_formatted_mac_address(char *mac_str, size_t mac_str_size);