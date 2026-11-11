#pragma once

#include "mqtt_client.h"
#include "cJSON.h"
#include <string>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <vector>
#include <time.h>
#include "SensorManager.hpp"
#include "WifiManager.hpp"
#include "../config/Config.hpp"

struct MqttFileData {
    std::string rootCA;
    std::string deviceCert;
    std::string deviceKey;
    std::string endpoint;
    std::string clientID;
    std::string publishTopic;
    std::string subscribeTopic;
    std::string deviceID;
    int port;
};

/**
 * @brief Sleep interval from the most recent downlink, held in RTC memory. Survives deep
 *        sleep; forgotten on a power cycle, reset or reflash.
 *
 * @return Minutes, or -1 if nothing has been adopted since the last cold boot.
 */
int get_adopted_sleep_time_min();

/**
 * @brief Record a downlink's interval so later cycles keep using it. No upper bound; values
 *        of zero or less are ignored so a bad downlink cannot clear a good interval.
 */
void set_adopted_sleep_time_min(int minutes);

class MqttManager {
private:
    static void mqtt_event_handler(void* handlerArgs, esp_event_base_t base, int32_t eventID, void *eventData);

    struct MqttFileData parse_mqtt_json(const std::string json_string);

    void deconstruct_mqtt_receive_packet(const std::string packet);

    /**
     * @brief O(n) Time & Space. If RAM becomes constrained this algo can be done in place for O(n^2) time.
     * 
     * @param originalString 
     * @return std::string 
     */
    std::string trim_precision(const std::string& originalString);

    esp_mqtt_client_config_t mqttCfg = {};
    char uri[128];
    esp_mqtt_client_handle_t client;
    struct MqttFileData fileData = {};

    int sleepTimeMin = DEFAULT_SLEEP_TIME_MIN;
    float batteryVolt = 2.4;

    // Connectivity reported in the packet's "net" object. Defaults describe a cycle that never
    // got a link, so a packet built before or without a connection still carries a valid shape.
    struct WifiLinkInfo linkInfo = {};
    uint32_t netFails = 0;
    uint8_t netReason = 0;

    // Broker connect duration, measured from connect() to the CONNACK that raises
    // MQTT_EVENT_CONNECTED. Separate from the Wi-Fi figure so a slow broker can be told apart
    // from a weak link.
    int64_t mqttConnectStartUs = 0;
    bool hasMqttConnectMs = false;
    uint32_t mqttConnectMs = 0;

public:
    MqttManager(); // init
    ~MqttManager();

    void connect();

    void disconnect();

    /**
     * @brief Build the uplink packet.
     *
     * @return The packet, or an empty string if it could not be built. An empty result must never
     *         be published or stored: the broker would acknowledge an empty payload like any
     *         other and the cycle would be counted as a delivered reading.
     */
    std::string construct_mqtt_packet(const std::vector<struct SensorItem> data);

    void publish_data(const std::string message);

    void subscribe_to_topic();

    int get_sleep_time_min() { return sleepTimeMin; }

    void change_voltage(float voltage) { batteryVolt = voltage; }

    /**
     * @brief Supply the connectivity reported in the packet's "net" object.
     *
     * @param info        Link sampled this cycle.
     * @param fails       Consecutive failed cycles preceding this one.
     * @param storedReason Disconnect reason carried over from the last failure.
     */
    void set_link_info(const struct WifiLinkInfo& info, uint32_t fails, uint8_t storedReason) {
        linkInfo = info;
        netFails = fails;

        // This cycle's own observation wins. Falling back to the stored one means a device that
        // connected cleanly still reports what went wrong on the cycles nobody saw.
        netReason = (info.reason != 0) ? info.reason : storedReason;
    }
};