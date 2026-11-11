#pragma once

#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "../config/Config.hpp"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/**
 * @brief Connectivity gathered over one wake cycle, reported in the uplink's "net" object.
 *
 * Each measurement carries its own validity flag. A field that could not be read is published as
 * JSON null rather than a number, because every value in the plausible range of an RSSI or a
 * timing is itself a legal reading and could not be told apart from a placeholder.
 */
struct WifiLinkInfo {
    bool connected = false;

    bool hasRssi = false;
    int rssi = 0;

    bool hasBssid = false;
    uint8_t bssid[6] = {};

    bool hasChannel = false;
    uint8_t channel = 0;

    bool hasConnMs = false;
    uint32_t connMs = 0;

    // Last disconnect reason seen this cycle, reported even when the device went on to connect.
    // wifi_err_reason_t starts at 1, so 0 is safe as a "nothing was recorded" value and never
    // collides with a real code.
    uint8_t reason = 0;
};

class WifiManager {
private:
    std::string ssid;
    std::string pass;

public:
    WifiManager(std::string ssid, std::string pass) : ssid(ssid), pass(pass) {}

    esp_err_t start_wifi();

    /**
     * @brief Sample the link for the uplink's "net" object.
     *
     * Must be called while the station is still associated, i.e. before esp_wifi_stop(). Safe to
     * call after a failed connection: the fields the AP would have supplied stay unset, and the
     * disconnect reason is still returned.
     *
     * @note Blocks for roughly RSSI_SAMPLE_COUNT * RSSI_SAMPLE_INTERVAL_MS while averaging.
     */
    struct WifiLinkInfo get_link_info();
};