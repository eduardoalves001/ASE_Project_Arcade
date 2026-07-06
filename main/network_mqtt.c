#include "network_mqtt.h"
#include "arcade_state.h"
#include "board_pins.h"
#include "generated_mqtt_config.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "NetworkMQTT";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

extern const uint8_t mqtt_broker_ca_pem_start[] asm("_binary_mqtt_broker_ca_pem_start");

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_H2E_IDENTIFIER ""
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define ESP_WIFI_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#ifndef ESP_WIFI_SAE_MODE
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define ESP_WIFI_H2E_IDENTIFIER ""
#endif

/* ----------------------- command handling ----------------------- */

static void handle_command(const char *cmd)
{
    uint32_t id;
    bool ok = true;
    if      (strcmp(cmd, "menu")         == 0) id = REMOTE_MENU;
    else if (strcmp(cmd, "start_flappy") == 0) id = REMOTE_START_FLAPPY;
    else if (strcmp(cmd, "start_pong")   == 0) id = REMOTE_START_PONG;
    else if (strcmp(cmd, "start_dino")   == 0) id = REMOTE_START_DINO;
    else if (strcmp(cmd, "select")       == 0) id = REMOTE_SELECT;
    else if (strcmp(cmd, "reset_scores") == 0) id = REMOTE_RESET_SCORES;
    else { ok = false; id = 0; }

    if (ok) {
        xQueueSend(button_evt_queue, &id, 0);
        ESP_LOGI(TAG, "MQTT command: %s", cmd);
    } else {
        ESP_LOGW(TAG, "Unknown MQTT command: %s", cmd);
    }
}

/* ----------------------- Wi-Fi station ----------------------- */

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected from AP, reason=%d", disconnected->reason);
        if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection to the AP");
        } else {
            ESP_LOGW(TAG, "Maximum Wi-Fi retries reached; retrying from the first attempt again.");
            s_retry_num = 0;
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_country_t country = {
        .cc = "PT",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt = CONFIG_ESP_MAXIMUM_RETRY,
#if CONFIG_ESP_WIFI_AUTH_OPEN
            .threshold.authmode = WIFI_AUTH_OPEN,
#elif CONFIG_ESP_WIFI_AUTH_WEP
            .threshold.authmode = WIFI_AUTH_WEP,
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
            .threshold.authmode = WIFI_AUTH_WPA_PSK,
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
            .threshold.authmode = WIFI_AUTH_WPA3_PSK,
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
            .threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK,
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
            .threshold.authmode = WIFI_AUTH_WAPI_PSK,
#endif
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = ESP_WIFI_H2E_IDENTIFIER,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

/* ----------------------- MQTT ----------------------- */

static const char *screen_str(screen_t s)
{
    switch (s) {
        case SCREEN_MENU:     return "menu";
        case SCREEN_PLAY:     return "play";
        case SCREEN_GAMEOVER: return "gameover";
        case SCREEN_SLEEP:    return "sleep";
        default:              return "?";
    }
}

static const char *game_str(game_id_t g)
{
    switch (g) {
        case GAME_FLAPPY: return "flappy";
        case GAME_PONG:  return "pong";
        case GAME_DINO:  return "dino";
        default:         return "?";
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        esp_mqtt_client_subscribe(event->client, "arcade/command", 0);
        ESP_LOGI(TAG, "MQTT subscribed: arcade/command");
    } else if (event_id == MQTT_EVENT_DATA) {
        if (event->topic_len == 14 && strncmp(event->topic, "arcade/command", 14) == 0) {
            char payload[64];
            int len = event->data_len < 63 ? event->data_len : 63;
            memcpy(payload, event->data, len);
            payload[len] = '\0';
            handle_command(payload);
        }
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGE(TAG, "MQTT error while connected to %s", ARCADE_MQTT_BROKER_URI);
    }
}

void net_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting secure MQTT connection to %s", ARCADE_MQTT_BROKER_URI);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = ARCADE_MQTT_BROKER_URI,
        .broker.verification.certificate = (const char *)mqtt_broker_ca_pem_start,
        .credentials.username = ARCADE_MQTT_USERNAME,
        .credentials.authentication.password = ARCADE_MQTT_PASSWORD,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    while (1) {
        arcade_state_t s;
        if (xSemaphoreTake(state_mutex, portMAX_DELAY)) {
            s = g_state;
            xSemaphoreGive(state_mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        char payload[384];
        snprintf(payload, sizeof(payload),
                 "{\"screen\":\"%s\",\"game\":\"%s\",\"score\":%lu,"
                 "\"high_flappy\":%lu,\"high_pong\":%lu,\"high_dino\":%lu,"
                 "\"is_sleeping\":%s}",
                 screen_str(s.screen), game_str(s.current_game),
                 (unsigned long)s.score,
                 (unsigned long)s.high[GAME_FLAPPY],
                 (unsigned long)s.high[GAME_PONG],
                 (unsigned long)s.high[GAME_DINO],
                 s.is_sleeping ? "true" : "false");

        ESP_LOGD(TAG, "MQTT status: %s", payload);
        esp_mqtt_client_publish(client, "arcade/status", payload, 0, 1, 0);

        /* wake up early when a task notifies us of a state change */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
    }
}
