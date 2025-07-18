#pragma once

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
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

#include "ade7953.h"
#include "led.h"
#include "secrets.h"

// WiFi configuration
#define WIFI_MAXIMUM_RETRY      5       // Number of WiFi connection retries before rebooting device
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1

// OTA configuration
#define OTA_VALIDATION_TIMEOUT  15000  // Time required to validate new firmware

// HTTP Web Server configuration
#define WEB_SERVER_PORT         80
#define WEB_SERVER_MAX_URI      10
#define WEB_SERVER_STACK_SIZE   (8 * 1024)

// Graceful shutdown configuration
#define GRACEFUL_SHUTDOWN_TIMEOUT_MS  10000  // Maximum time to wait for graceful shutdown

// MQTT logging configuration
#define MQTT_DEFAULT_BROKER_URI         "mqtt://192.168.1.1"
#define MQTT_DEFAULT_PORT               1883
#define MQTT_DEFAULT_USERNAME           "open_grid_monitor"
#define NVS_MQTT_NAMESPACE              "mqtt_config"

// MQTT Authentication - set to default values to disable, or change to your credentials
#define MQTT_KEEPALIVE          60
#define MQTT_CREDENTIALS_MAX_LEN 64
#define MQTT_TASK_NAME          "mqtt_task"
#define MQTT_TASK_STACK_SIZE    (32 * 1024)  // Increased for log processing
#define MQTT_TASK_PRIORITY      3
#define MQTT_QUEUE_SIZE         100
#define MQTT_MSG_MAX_SIZE       256

// MQTT Topics
#define MQTT_TOPIC_LEN          64
#define MQTT_TOPIC_BASE         "open_grid_monitor"
#define MQTT_TOPIC_LOGS         "logs"
#define MQTT_TOPIC_STATUS       "status" 
#define MQTT_TOPIC_SYSTEM       "system"
#define MQTT_TOPIC_MEASUREMENT  "measurement"
#define MQTT_TOPIC_DEBUG        "debug"
#define MQTT_TOPIC_COMMANDS     "commands"
#define MQTT_TOPIC_RESPONSES    "responses"
#define MQTT_TOPIC_FIRMWARE     "firmware"

// MQTT Commands
#define MQTT_TOPIC_COMMAND_RESTART "restart"
#define MQTT_TOPIC_COMMAND_OTA     "ota"
#define MQTT_COMMAND_PAYLOAD_LEN   256
#define MQTT_COMMAND_DEFAULT_ID    -1

// MQTT other constants
#define MQTT_STATUS_INTERVAL    10000

// MQTT definitions
#define QOS_0                   0
#define QOS_1                   1
#define QOS_2                   2

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
#define MEASUREMENT_TASK_NAME   "measurement_pub_task"
#define MEASUREMENT_TASK_STACK_SIZE (8 * 1024)
#define MEASUREMENT_TASK_PRIORITY   7

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

// MQTT credentials structure
typedef struct {
    char broker_uri[128];
    uint16_t port;
    char username[MQTT_CREDENTIALS_MAX_LEN];
    char password[MQTT_CREDENTIALS_MAX_LEN];
    bool use_auth;
} mqtt_credentials_t;

// Network handle structure
typedef struct {
    EventGroupHandle_t wifi_event_group;
    wifi_status_t status;
    bool ota_enabled;
    bool mqtt_logging_enabled;
    bool mqtt_commands_enabled;
    bool measurement_publishing_enabled;
    bool web_server_enabled;
    int retry_count;
    char ip_address[16];
    char mac_address[13];  // 12 chars for MAC + null terminator (lowercase, no colons)
    char mqtt_client_id[32];
    char mqtt_topic_logs[MQTT_TOPIC_LEN];
    char mqtt_topic_status[MQTT_TOPIC_LEN];
    char mqtt_topic_measurement[MQTT_TOPIC_LEN];
    char mqtt_topic_system[MQTT_TOPIC_LEN];
    char mqtt_topic_commands_restart[MQTT_TOPIC_LEN];
    char mqtt_topic_commands_ota[MQTT_TOPIC_LEN];
    char mqtt_topic_responses_restart[MQTT_TOPIC_LEN];
    char mqtt_topic_responses_ota[MQTT_TOPIC_LEN];
    char mqtt_topic_firmware[MQTT_TOPIC_LEN];
    QueueHandle_t log_queue;
    QueueHandle_t measurement_queue;
    log_buffer_t *log_buffer;
    led_handle_t *led_handle;
    ade7953_handle_t *ade7953_handle;
    mqtt_credentials_t mqtt_credentials;
} network_handle_t;

typedef enum {
    MQTT_COMMAND_RESTART,
    MQTT_COMMAND_OTA
} mqtt_command_t;

// Function prototypes
esp_err_t network_init(network_handle_t *handle, led_handle_t *led_handle, ade7953_handle_t *ade7953_handle);
esp_err_t network_deinit(network_handle_t *handle);
esp_err_t network_start_wifi(network_handle_t *handle);
esp_err_t network_stop_wifi(network_handle_t *handle);
esp_err_t network_start_mqtt_logging(network_handle_t *handle);
esp_err_t network_stop_mqtt_logging(network_handle_t *handle);

// Status functions
wifi_status_t network_get_wifi_status(network_handle_t *handle);
const char* network_get_ip_address(network_handle_t *handle);
bool network_is_connected(network_handle_t *handle);
bool network_is_mqtt_connected(void);

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

// Firmware functions
esp_err_t network_publish_firmware_info(network_handle_t *handle);

// HTTP Web Server functions
esp_err_t network_start_web_server(network_handle_t *handle);
esp_err_t network_stop_web_server(network_handle_t *handle);

// Log buffering functions for pre-MQTT logs
esp_err_t network_init_log_buffer(network_handle_t *handle);
esp_err_t network_flush_log_buffer(network_handle_t *handle);
esp_err_t network_deinit_log_buffer(network_handle_t *handle);

// Get MAC address formatted for MQTT (lowercase, no colons)
esp_err_t network_get_formatted_mac_address(char *mac_str, size_t mac_str_size);

// MQTT credentials management functions
esp_err_t network_load_mqtt_credentials(mqtt_credentials_t *credentials);
esp_err_t network_save_mqtt_credentials(const mqtt_credentials_t *credentials);
esp_err_t network_get_mqtt_credentials(network_handle_t *handle, mqtt_credentials_t *credentials);
esp_err_t network_set_mqtt_credentials(network_handle_t *handle, const mqtt_credentials_t *credentials);