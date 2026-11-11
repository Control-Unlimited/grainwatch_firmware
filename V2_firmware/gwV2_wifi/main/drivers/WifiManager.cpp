#include "../includes/WifiManager.hpp"

#include "esp_timer.h"
#include <inttypes.h>
#include <string.h>

static EventGroupHandle_t s_wifi_event_group;
static const char *TAG = "Wifi Manager";
static int s_retry_num = 0;

// Connection timing and the last disconnect reason, collected by the event handler for the
// uplink's "net" object. Statics because the handler is a free function with no instance.
static int64_t s_connect_start_us = 0;
static uint32_t s_connect_ms = 0;
static bool s_has_connect_ms = false;
static uint8_t s_last_disconnect_reason = 0;

namespace {
    static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
            ESP_LOGW(TAG, "Disconnect reason: %d", event->reason);

            // Kept even if a later attempt succeeds. A reason alongside a successful cycle says
            // the link wobbled and recovered, which is what a site looks like before it fails.
            s_last_disconnect_reason = event->reason;

            if (s_retry_num < MAXIMUM_WIFI_RETRY_ATTEMPTS) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "Retrying connection... (%d/%d)", s_retry_num, MAXIMUM_WIFI_RETRY_ATTEMPTS);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Successfully assigned IP: " IPSTR, IP2STR(&event->ip_info.ip));

            // Radio on to usable address, retries included. This is the number that creeps upward
            // as a site degrades, so it is measured to the IP rather than to the association.
            s_connect_ms = (uint32_t)((esp_timer_get_time() - s_connect_start_us) / 1000);
            s_has_connect_ms = true;
            ESP_LOGI(TAG, "Connection took %" PRIu32 " ms", s_connect_ms);

            s_retry_num = 0;
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

struct WifiLinkInfo WifiManager::get_link_info() {
    struct WifiLinkInfo info;

    info.reason = s_last_disconnect_reason;
    info.hasConnMs = s_has_connect_ms;
    info.connMs = s_connect_ms;

    wifi_ap_record_t ap = {};
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) {
        // Not associated, so nothing the AP would have told us is available. The disconnect
        // reason above is the only thing worth reporting from a cycle that never connected.
        ESP_LOGW(TAG, "No AP info available (%s). Link fields will be reported as null.", esp_err_to_name(err));
        return info;
    }

    info.connected = true;

    memcpy(info.bssid, ap.bssid, sizeof(info.bssid));
    info.hasBssid = true;

    info.channel = ap.primary;
    info.hasChannel = true;

    // Averaged rather than taken once, see RSSI_SAMPLE_COUNT in Config.hpp.
    int total = 0;
    int samples = 0;
    for (int i = 0; i < RSSI_SAMPLE_COUNT; i++) {
        int rssi = 0;
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
            total += rssi;
            samples++;
        }

        if (i + 1 < RSSI_SAMPLE_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(RSSI_SAMPLE_INTERVAL_MS));
        }
    }

    if (samples > 0) {
        info.rssi = total / samples;
        info.hasRssi = true;
    }

    ESP_LOGI(TAG, "Link: RSSI %d dBm (%d samples), channel %d", info.rssi, samples, info.channel);

    return info;
}

esp_err_t WifiManager::start_wifi() {
    s_retry_num = 0;
    s_connect_ms = 0;
    s_has_connect_ms = false;
    s_last_disconnect_reason = 0;

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Init Stack
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    // Config WiFi 
    wifi_config_t wifi_config = {};

    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, pass.c_str(), sizeof(wifi_config.sta.password));

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; 
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Started immediately before the radio comes up, since esp_wifi_start() is what triggers the
    // STA_START event and therefore the first connection attempt.
    s_connect_start_us = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi Station initialized. Waiting for connection...");

    // Block until connected or failed
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    esp_err_t status = ESP_FAIL;
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP: %s", ssid.c_str());
        status = ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid.c_str());
        status = ESP_FAIL;
    }

    // Clean up to prevent memory leaks
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);

    return status;
}