#include "../includes/WorkTask.hpp"

static const char* TAG = "Work Task";

SemaphoreHandle_t mqttSyncSemaphore = NULL;
SemaphoreHandle_t mqttSubTopicSemaphore = NULL;
SemaphoreHandle_t mqttPubSuccessSemaphore = NULL;

/**
 * @brief Sleep applied after a fatal error. A device keeps to its adopted interval even on a
 *        failed cycle; only one that was never given an interval uses the 6-hour default.
 */
static uint64_t error_sleep_us() {
    int adopted = get_adopted_sleep_time_min();

    if (adopted > 0) {
        return (uint64_t)adopted * 60ULL * 1000000ULL;
    }

    return (uint64_t)TIME_TO_SLEEP_FOR_AFTER_ERROR_SECONDS * 1000000ULL;
}

/**
 * @brief Record a failed cycle and write a packet to storage carrying the failure count including
 *        this one, so the backlog reads as a continuous sequence once it is flushed.
 *
 * Only for failures where the packet never reached the broker. A packet that has already been
 * published must be stored byte-identical instead, so the receiver can recognise the later retry
 * as a duplicate of a message it may already hold.
 */
static void store_failed_cycle(MqttManager& mqttManager, const struct WifiLinkInfo& linkInfo,
                               const std::vector<struct SensorItem>& data) {
    record_net_failure_to_NVS(linkInfo.reason);

    struct NetFailureState updated = read_net_failures_from_NVS();
    mqttManager.set_link_info(linkInfo, updated.fails, updated.reason);

    std::string message = mqttManager.construct_mqtt_packet(data);
    if (message.empty()) {
        // Nothing is stored rather than storing an empty entry, which would sit in the backlog
        // and be published later as a meaningless reading. The failure is already counted above,
        // so the cycle still shows up in the next uplink.
        ESP_LOGE(TAG, "Packet could not be built. Nothing stored for this cycle.");
        return;
    }

    store_packet(message);
}

void sync_time() {
    // If already initialized, don't re-init
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    ESP_LOGI("TIME", "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    
    //setenv("TZ", "AEST-10", 1);
    tzset();

    esp_sntp_init();

    int retry = 0;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 30) {
        ESP_LOGI("TIME", "Waiting for sync... (%d)", retry);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI("TIME", "Time synchronized: %s", asctime(&timeinfo));
}

void work_task(void* pvParameters) {
    TaskHandle_t notifyTarget = (TaskHandle_t)pvParameters;

    // Get Creds
    std::pair<int, struct NVSData> storedDataPair = read_creds_from_NVS();
    if (storedDataPair.first != FUNCTION_SUCCESS) { // Unable to get credentials
        xTaskNotifyGive(notifyTarget);
        vTaskSuspend(NULL); // Suspend task while main is deleting.
    }
    ESP_LOGI(TAG, "Extracted SSID %s from NVS", storedDataPair.second.ssid.c_str());

    // --- Connect to Wi-Fi ---
    WifiManager wifiManager(storedDataPair.second.ssid, storedDataPair.second.pass);

    esp_err_t err = wifiManager.start_wifi();
    bool wifiSuccess = false;

    // Init I2C so we can use RTC
    SensorManager sensorManager;
    
    // Scan I2C Bus
    int found = sensorManager.scan_i2c_bus();

    // Retrieve all I2C devices data
    if (found == 0) {
        set_status_display(Status::I2C_BUS_FAIL);

        ESP_LOGW(TAG, "Failure finding any devices in I2C bus. Execution will continue until issue can be reported to MQTT broker.");
        
        // Non-fatal error. Continue to MQTT connection and report issue.
        // vTaskDelay(pdMS_TO_TICKS(TIME_TO_STAY_ACTIVE_AFTER_ERROR_SECONDS / 1000)); // Delay turn off so the LED display can be seen
        // esp_deep_sleep(TIME_TO_SLEEP_FOR_AFTER_ERROR_SECONDS * 1000000LL); // Go back to sleep
        // TODO: Add a way to later notify MQTT broker of this problem
    }

    if (err == ESP_OK) { // If there is wifi get NTP time
        set_status_display(Status::WIFI_ACTIVE);
        wifiSuccess = true;

        // Sync time via NTP
        sync_time();

        // Save sync time to I2C RTC
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        if (sensorManager.DS1337_SetTime(&timeinfo) == ESP_OK) {
            ESP_LOGI(TAG, "RTC synchronized with NTP time.");
        } else {
            ESP_LOGE(TAG, "Failed to update RTC with NTP time.");
        }
    } else { // If there is no wifi get RTC time
        set_status_display(Status::WIFI_CONNECTION_FAIL);

        // Get I2C Time
        struct tm rtc_time;
        if (sensorManager.DS1337_GetTime(&rtc_time) == ESP_OK) {
            time_t time_to_set = mktime(&rtc_time);
            struct timeval tv = { .tv_sec = time_to_set };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "System time set from RTC: %s", asctime(&rtc_time));
        }
    }

    // ---   Get I2C data   ---
    vTaskDelay(pdMS_TO_TICKS(50));
    std::vector<struct SensorItem> data = sensorManager.collect_sensor_data();

    // --- Create Packet ---
    // Init MQTT
    MqttManager mqttManager;

    // Store voltage
    int bat_mv = read_battery_mvol();
    mqttManager.change_voltage(bat_mv / 1000.0);

    // Sample the link while the station is still associated. On a cycle that never connected this
    // still returns the disconnect reason, which is the only thing that cycle has to report.
    struct WifiLinkInfo linkInfo = wifiManager.get_link_info();

    // Failures from earlier cycles that never reached the broker. They are reported here because
    // those cycles had no way of reporting themselves.
    struct NetFailureState netFailures = read_net_failures_from_NVS();
    mqttManager.set_link_info(linkInfo, netFailures.fails, netFailures.reason);

    // --- If no wifi, Store Packet ---
    if (!wifiSuccess) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi. Device will sleep for %d seconds in %d seconds.", TIME_TO_SLEEP_FOR_AFTER_ERROR_SECONDS, TIME_TO_STAY_ACTIVE_AFTER_ERROR_SECONDS);

        store_failed_cycle(mqttManager, linkInfo, data);

        vTaskDelay(pdMS_TO_TICKS(TIME_TO_STAY_ACTIVE_AFTER_ERROR_SECONDS * 1000)); // Delay turn off so the LED display can be seen
        esp_deep_sleep(error_sleep_us()); // Go back to sleep
    }
    
    // --- Handle MQTT Broker ---
    // Init Semaphore
    mqttSyncSemaphore = xSemaphoreCreateBinary();

    // Connect to Broker
    mqttManager.connect();

    // Wait until connection is active
    if (xSemaphoreTake(mqttSyncSemaphore, pdMS_TO_TICKS(MAX_MQTT_CONNECTION_WAIT_SECONDS * 1000)) != pdTRUE) {
        set_status_display(Status::MQTT_CONNECTION_FAIL);
        ESP_LOGW(TAG, "MQTT broker connection failed within %d seconds", MAX_MQTT_CONNECTION_WAIT_SECONDS);

        store_failed_cycle(mqttManager, linkInfo, data);

        vTaskDelay(pdMS_TO_TICKS(TIME_TO_STAY_ACTIVE_AFTER_ERROR_SECONDS * 1000)); // Delay turn off so the LED display can be seen
        esp_deep_sleep(error_sleep_us());
    }

    // Subscribe to topic
    // Subscribed before the uplink is published so the broker has the subscription
    // registered by the time the downlink is generated in response to it. A reply
    // that arrives before we reach the wait below is still captured, because the
    // event handler gives the semaphore as soon as the data event fires.
    mqttSubTopicSemaphore = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "Subscribing to topic");
    mqttManager.subscribe_to_topic();

    // Publish data to topic
    mqttPubSuccessSemaphore = xSemaphoreCreateBinary();

    // Create Packet
    // The packet is built at the latest moment possible so the readings are current. This is also
    // the first point at which the broker connect time is known, so it is the only packet that
    // can carry t_mqtt as a value rather than null.
    // sleep_min reports the interval this cycle is running on; a new value from the
    // downlink below takes effect on the next wake.
    std::string message = mqttManager.construct_mqtt_packet(data);

    if (message.empty()) {
        // Publishing nothing would still draw a PUBACK, and the cycle would be recorded as a
        // delivered reading. Treated as a failed cycle instead so it is visible in the next
        // uplink. Nothing is stored because there is no packet to store.
        set_status_display(Status::MQTT_CONNECTION_FAIL);
        ESP_LOGE(TAG, "Packet could not be built. Skipping publish.");

        record_net_failure_to_NVS(linkInfo.reason);

        vTaskDelay(pdMS_TO_TICKS(TIME_TO_STAY_ACTIVE_AFTER_ERROR_SECONDS * 1000)); // Delay turn off so the LED display can be seen
        esp_deep_sleep(error_sleep_us());
    }

    ESP_LOGI(TAG, "Publishing message");
    mqttManager.publish_data(message);

    // Wait until send data on pub topic
    if (xSemaphoreTake(mqttPubSuccessSemaphore, pdMS_TO_TICKS(MAX_MQTT_PUBACK_WAIT_SECONDS * 1000)) != pdTRUE) {
        set_status_display(Status::MQTT_CONNECTION_FAIL);
        ESP_LOGW(TAG, "Failed to receive PUBACK after %d seconds", MAX_MQTT_PUBACK_WAIT_SECONDS);

        // Counted so the next cycle reports it, but the packet is stored exactly as published.
        // The broker may already hold it, and an identical copy is what lets the receiver
        // recognise the later retry as a duplicate rather than a second reading.
        record_net_failure_to_NVS(linkInfo.reason);
        store_packet(message);

        vTaskDelay(pdMS_TO_TICKS(TIME_TO_STAY_ACTIVE_AFTER_ERROR_SECONDS * 1000)); // Delay turn off so the LED display can be seen
        esp_deep_sleep(error_sleep_us());
    }

    // The uplink is acknowledged, so this cycle counts as a success. Cleared here rather than at
    // the end because a failure while flushing the backlog below does not make this cycle a
    // failed one - this packet did get through.
    clear_net_failures_in_NVS();

    // Wait until retrieve data on sub topic. The downlink is sent in response to the
    // uplink above, so this wait only starts once the broker has acknowledged it.
    if (xSemaphoreTake(mqttSubTopicSemaphore, pdMS_TO_TICKS(MAX_MQTT_SUB_RECEIVE_WAIT_SECONDS * 1000)) != pdTRUE) {
        set_status_display(Status::MQTT_CONNECTION_FAIL);
        ESP_LOGW(TAG, "No downlink after %d seconds. Continuing with sleep of %d minutes.", MAX_MQTT_SUB_RECEIVE_WAIT_SECONDS, mqttManager.get_sleep_time_min());

        // sleepTimeMin keeps the last adopted value - fall through and send stored packets.
    }

    // Now send
    std::pair<int, std::vector<std::string>> packets = read_stored_packets();
    ESP_LOGI(TAG, "Stored Data Response: %d", packets.first);
    if (packets.first == FUNCTION_SUCCESS) {
        if (packets.second.size() == 0) {
            ESP_LOGI(TAG, "No old packets to send. Skipping...");
        }
        ESP_LOGI(TAG, "Sending %d old packets saved in storage.", packets.second.size());

        bool error_occured = false;
        for (std::string packet : packets.second) {
            // Dropped rather than retried. An empty entry can never draw a PUBACK, so re-storing
            // it on failure would leave it in the backlog to be retried on every wake forever.
            if (packet.empty()) {
                ESP_LOGW(TAG, "Discarding empty stored packet");
                continue;
            }

            xSemaphoreTake(mqttPubSuccessSemaphore, 0); // Clear stale sem

            mqttManager.publish_data(packet);

            // If a message fails store it and try the next message
            if (xSemaphoreTake(mqttPubSuccessSemaphore, pdMS_TO_TICKS(MAX_MQTT_PUBACK_WAIT_SECONDS * 1000)) != pdTRUE) {
                ESP_LOGW(TAG, "Failed to receive PUBACK after %d seconds", MAX_MQTT_PUBACK_WAIT_SECONDS);

                store_packet(packet);

                error_occured = true;
            }
        }

        // If one (or multiple) of the messages failed to send, display error
        if (error_occured) {
            set_status_display(Status::MQTT_CONNECTION_FAIL);

            vTaskDelay(pdMS_TO_TICKS(TIME_TO_STAY_ACTIVE_AFTER_ERROR_SECONDS * 1000)); // Delay turn off so the LED display can be seen
            esp_deep_sleep(error_sleep_us());
        }
    }

    // turn off 3v3 pow
    disable_3v3_pow();

    // ---  Sleep/Power-Off  ---
    int sleepMin = mqttManager.get_sleep_time_min();
    if (sleepMin <= 0) sleepMin = DEFAULT_SLEEP_TIME_MIN; // Guard in case nothing valid was ever set

    // 64-bit throughout so the interval is bounded only by what the operator asks for.
    uint64_t sleepMicroseconds = (uint64_t)sleepMin * 60ULL * 1000000ULL;
    ESP_LOGI(TAG, "Operations successfully complete. Device will sleep for %d minutes.", sleepMin);

    esp_deep_sleep(sleepMicroseconds);
}