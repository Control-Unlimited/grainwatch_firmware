#include "../includes/MqttManager.hpp"

#include "esp_attr.h"
#include "esp_timer.h"
#include <climits>
#include <cstdio>
#include <inttypes.h>

static const char *TAG = "MqttManager";

// .rtc.data survives deep sleep but is reloaded on a battery pull, reset or reflash, which
// is what returns a device to DEFAULT_SLEEP_TIME_MIN. Magic word guards against junk in RTC.
static const uint32_t ADOPTED_SLEEP_MAGIC = 0x51EEB17C;

static RTC_DATA_ATTR uint32_t rtcAdoptedSleepMagic = 0;
static RTC_DATA_ATTR int rtcAdoptedSleepMin = 0;

int get_adopted_sleep_time_min() {
    if (rtcAdoptedSleepMagic != ADOPTED_SLEEP_MAGIC || rtcAdoptedSleepMin <= 0) {
        return -1;
    }

    return rtcAdoptedSleepMin;
}

void set_adopted_sleep_time_min(int minutes) {
    if (minutes <= 0) {
        ESP_LOGW(TAG, "Ignoring sleep interval of %d min", minutes);
        return;
    }

    rtcAdoptedSleepMin = minutes;
    rtcAdoptedSleepMagic = ADOPTED_SLEEP_MAGIC;
}

extern SemaphoreHandle_t mqttSyncSemaphore;
extern SemaphoreHandle_t mqttSubTopicSemaphore;
extern SemaphoreHandle_t mqttPubSuccessSemaphore;

extern "C" {
    extern const uint8_t cert_json_start[] asm("_binary_certfile_json_start");
    extern const uint8_t cert_json_end[]   asm("_binary_certfile_json_end");
}

std::string MqttManager::trim_precision(const std::string& originalString) {
    std::string output;
    output.reserve(originalString.size());

    int digitsAfterDot = -1;
    
    for (char ch : originalString) {
        if (ch == '.') { // Decimal Point
            digitsAfterDot = 0;
        } else if (!std::isdigit(ch)) { // Non-numerical Character
            digitsAfterDot = -1; // Reset on non-numerical character
        } else if (digitsAfterDot >= 0) { // Numerical Chacter & MQTT_VALUE_PRECISION bigger than last decimal point location
            if (digitsAfterDot >= MQTT_VALUE_PRECISION) {
                continue; // Skip over
            }
            digitsAfterDot++;
        }

        output += ch;
    }

    return output;
}

void MqttManager::mqtt_event_handler(void* handlerArgs, esp_event_base_t base, int32_t eventID, void *eventData) {
    MqttManager* self = static_cast<MqttManager*>(handlerArgs);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;
    
    switch ((esp_mqtt_event_id_t)eventID) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "MQTT Connection active");

            // Timed here rather than where the caller waits, so the figure is the broker's
            // actual CONNACK and not the scheduling delay on the semaphore.
            if (self->mqttConnectStartUs > 0) {
                self->mqttConnectMs = (uint32_t)((esp_timer_get_time() - self->mqttConnectStartUs) / 1000);
                self->hasMqttConnectMs = true;
                ESP_LOGI(TAG, "Broker connect took %" PRIu32 " ms", self->mqttConnectMs);
            }

            xSemaphoreGive(mqttSyncSemaphore);

            break;
        }
        case MQTT_EVENT_DATA: {
            ESP_LOGI(TAG, "Message received on topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Data: %.*s", event->data_len, event->data);

            // esp-mqtt hands over a pointer into its transport buffer with a separate length and
            // does not terminate it, and sets it to NULL outright for a zero-length payload.
            // Built with an explicit length so parsing cannot run past the message, and guarded
            // so an empty downlink cannot be turned into a std::string from NULL.
            if (event->data != NULL && event->data_len > 0) {
                self->deconstruct_mqtt_receive_packet(std::string(event->data, event->data_len));
            } else {
                ESP_LOGW(TAG, "Empty downlink. Keeping current sleep interval.");
            }

            // Set Semaphore
            xSemaphoreGive(mqttSubTopicSemaphore);

            break;
        }
        case MQTT_EVENT_PUBLISHED: {
            ESP_LOGI(TAG, "Message published successfully");

            // Give sem
            xSemaphoreGive(mqttPubSuccessSemaphore);

            break;
        }
        case MQTT_EVENT_ERROR: {
            ESP_LOGW(TAG, "MQTT Error occured");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "Last ESP-TLS error: 0x%x", event->error_handle->esp_tls_last_esp_err);
                ESP_LOGE(TAG, "Last TLS Stack error: 0x%x", event->error_handle->esp_tls_stack_err);
            }
            break;
        }
        default: {
            ESP_LOGD(TAG, "Unknown MQTT event");
            break;
        }
    }
}

void MqttManager::deconstruct_mqtt_receive_packet(const std::string packet) {
    // Only a well-formed downlink may change sleepTimeMin. Clearing it on bad input would drop
    // the device back to DEFAULT_SLEEP_TIME_MIN, which is what this interval exists to prevent.
    cJSON* root = cJSON_Parse(packet.c_str());
    if (root == NULL) {
        ESP_LOGW(TAG, "Downlink not valid JSON. Keeping %d min.", sleepTimeMin);
        return;
    }

    cJSON* device_config = cJSON_GetObjectItem(root, "device_config");

    if (cJSON_IsObject(device_config)) {
        cJSON* sleep_item = cJSON_GetObjectItem(device_config, "sleep_interval_mins");

        if (cJSON_IsNumber(sleep_item)) {
            // valuedouble, not valueint: cJSON saturates valueint at INT_MAX. No upper bound
            // is imposed on the interval an operator may ask for.
            double requested = sleep_item->valuedouble;

            if (requested > 0.0 && requested <= (double)INT_MAX) {
                sleepTimeMin = (int)requested;
                set_adopted_sleep_time_min(sleepTimeMin);
                ESP_LOGI(TAG, "Downlink set sleep interval to %d min", sleepTimeMin);
            } else {
                ESP_LOGW(TAG, "sleep_interval_mins %f out of range. Keeping %d min.", requested, sleepTimeMin);
            }
        } else {
            ESP_LOGW(TAG, "sleep_interval_mins missing/not a number. Keeping %d min.", sleepTimeMin);
        }
    } else {
        ESP_LOGW(TAG, "device_config not found. Keeping %d min.", sleepTimeMin);
    }

    cJSON_Delete(root);
}

void MqttManager::subscribe_to_topic() {
    esp_mqtt_client_subscribe(client, fileData.subscribeTopic.c_str(), 1);
}

struct MqttFileData MqttManager::parse_mqtt_json(const std::string jsonString) {
    struct MqttFileData newCreds = {};

    if (jsonString.empty()) {
        ESP_LOGE(TAG, "JSON string is empty");
        return newCreds;
    }

    cJSON* root = cJSON_Parse(jsonString.c_str());
    if (root == NULL) {
        const char* errorPtr = cJSON_GetErrorPtr();
        if (errorPtr != NULL) {
            ESP_LOGE(TAG, "JSON Parse Error before: %s", errorPtr);
        }
        return newCreds;
    }

    auto get_string = [&](const char* key, std::string& target) {
        cJSON* item = cJSON_GetObjectItem(root, key);
        if (cJSON_IsString(item) && (item->valuestring != NULL)) {
            target = item->valuestring; // Deep copies into std::string
        } else {
            ESP_LOGW(TAG, "Missing or invalid field: %s", key);
        }
    };

    get_string("ROOT_CA", newCreds.rootCA);
    get_string("BROKER_ENDPOINT", newCreds.endpoint);
    get_string("CLIENT_ID", newCreds.clientID);
    get_string("USERNAME", newCreds.username);
    get_string("PASSWORD", newCreds.password);
    get_string("MQTT_TOPIC_TO_PUBLISH_TO", newCreds.publishTopic);
    get_string("MQTT_TOPIC_TO_SUBSCRIBE_TO", newCreds.subscribeTopic);
    get_string("DEVICE_NAME", newCreds.deviceName);
    
    cJSON* portItem = cJSON_GetObjectItem(root, "BROKER_PORT");
    if (cJSON_IsNumber(portItem)) {
        newCreds.port = portItem->valueint;
    } else {
        ESP_LOGW(TAG, "Missing or invalid BROKER_PORT");
    }
    
    cJSON_Delete(root);
    return newCreds;
}

MqttManager::MqttManager() {
    // Interval adopted on an earlier wake. Only a cold boot leaves nothing here, so the
    // default applies on the first cycle and the last downlink's value on every one after.
    int adopted = get_adopted_sleep_time_min();
    if (adopted > 0) {
        sleepTimeMin = adopted;
        ESP_LOGI(TAG, "Resuming sleep interval of %d min", sleepTimeMin);
    } else {
        ESP_LOGI(TAG, "Cold boot. Using default of %d min.", sleepTimeMin);
    }

    // Get config file contents
    size_t cert_size = (cert_json_end - cert_json_start);
    const uint8_t* data_ptr = cert_json_start;

    //ESP_LOGI(TAG, "File: %s", fileContent.c_str());
    // Check for UTF-8 BOM
    if (cert_size >= 3 && data_ptr[0] == 0xEF && data_ptr[1] == 0xBB && data_ptr[2] == 0xBF) {
        ESP_LOGW(TAG, "UTF-8 BOM detect in JSON, skipping first 3 bytes");
        data_ptr += 3;
        cert_size -= 3;
    }

    // From data_ptr, not cert_json_start: cert_size has already been reduced by the BOM, so
    // starting at the BOM would cut the last 3 bytes off the JSON and fail the parse.
    const std::string fileContent((const char*)data_ptr, cert_size);

    // Parse contents
    fileData = parse_mqtt_json(fileContent);

    ESP_LOGI(TAG, "Endpoint: %s", fileData.endpoint.c_str());
    ESP_LOGI(TAG, "Port: %d", fileData.port);
    ESP_LOGI(TAG, "Client ID: %s", fileData.clientID.c_str());
    ESP_LOGI(TAG, "Username: %s", fileData.username.c_str());

    // The connect will be refused without these, but carrying on lets that failure follow the
    // normal path: the reading is stored and the failure is counted for the next uplink.
    if (fileData.clientID.empty() || fileData.username.empty() || fileData.password.empty()) {
        ESP_LOGE(TAG, "certfile.json has no CLIENT_ID, USERNAME or PASSWORD. The broker will refuse the connection.");
    }

    // Initialize MQTT
    snprintf(uri, sizeof(uri), "mqtts://%s:%d", fileData.endpoint.c_str(), fileData.port);

    // Doover broker: TLS verifies the broker only, and the device logs in with a username and
    // password (its agent ID and API token). No client certificate.
    mqttCfg.broker.address.uri = uri;
    mqttCfg.broker.verification.certificate = fileData.rootCA.c_str();
    mqttCfg.credentials.client_id = fileData.clientID.c_str();
    mqttCfg.credentials.username = fileData.username.c_str();
    mqttCfg.credentials.authentication.password = fileData.password.c_str();

    mqttCfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;

    client = esp_mqtt_client_init(&mqttCfg);
    esp_mqtt_client_register_event(client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, this);
}

void MqttManager::connect() {
    mqttConnectStartUs = esp_timer_get_time();
    esp_mqtt_client_start(client);
}

void MqttManager::disconnect() {
    // Disconnect
    if (client) {
        esp_mqtt_client_stop(client);
    }
}

MqttManager::~MqttManager() {
    if (client) {
        esp_mqtt_client_destroy(client);
    }
}

void MqttManager::publish_data(const std::string message) {
    if (!client) {
        ESP_LOGW(TAG, "Client not initialized");
        return;
    }

    // Backstop. Callers already reject empty packets, so reaching this means a bug upstream.
    // Not publishing means the caller's PUBACK wait times out and the cycle is recorded as a
    // failure, which is the safe outcome: nothing meaningless is delivered as a reading.
    if (message.empty()) {
        ESP_LOGE(TAG, "Refusing to publish an empty message");
        return;
    }

    ESP_LOGI(TAG, "MQTT Publish Message: %s", message.c_str());

    // Send
    esp_mqtt_client_publish(client, fileData.publishTopic.c_str(), message.c_str(), message.length(), 1, 0);
}

std::string MqttManager::construct_mqtt_packet(const std::vector<struct SensorItem> data) {
    // Root obj
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "d_id", fileData.deviceName.c_str());

    // Status obj
    cJSON *status = cJSON_CreateObject();
    cJSON_AddNumberToObject(status, "sleep_min", sleepTimeMin);
    cJSON_AddNumberToObject(status, "batt_v", batteryVolt);
    cJSON_AddItemToObject(root, "status", status);

    // Connectivity obj
    // Every key is always present so the packet keeps one shape across success and failure.
    // Anything not measured this cycle is published as null rather than a stand-in number, since
    // any plausible RSSI or timing is also a legal reading. "type" identifies the transport for
    // consumers that also handle cellular units, and is never null.
    cJSON *net = cJSON_CreateObject();
    cJSON_AddStringToObject(net, "type", "wifi");

    if (linkInfo.hasRssi) {
        cJSON_AddNumberToObject(net, "rssi", linkInfo.rssi);
    } else {
        cJSON_AddNullToObject(net, "rssi");
    }

    if (linkInfo.hasBssid) {
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 linkInfo.bssid[0], linkInfo.bssid[1], linkInfo.bssid[2],
                 linkInfo.bssid[3], linkInfo.bssid[4], linkInfo.bssid[5]);
        cJSON_AddStringToObject(net, "bssid", bssid);
    } else {
        cJSON_AddNullToObject(net, "bssid");
    }

    if (linkInfo.hasChannel) {
        cJSON_AddNumberToObject(net, "ch", linkInfo.channel);
    } else {
        cJSON_AddNullToObject(net, "ch");
    }

    if (linkInfo.hasConnMs) {
        cJSON_AddNumberToObject(net, "t_conn", linkInfo.connMs);
    } else {
        cJSON_AddNullToObject(net, "t_conn");
    }

    // Null on any packet built before the broker answered, which includes every packet written to
    // storage because Wi-Fi or the broker itself was the thing that failed.
    if (hasMqttConnectMs) {
        cJSON_AddNumberToObject(net, "t_mqtt", mqttConnectMs);
    } else {
        cJSON_AddNullToObject(net, "t_mqtt");
    }

    cJSON_AddNumberToObject(net, "fails", netFails);
    cJSON_AddNumberToObject(net, "reason", netReason);

    cJSON_AddItemToObject(root, "net", net);

    // Packet creation
    time_t now;
    time(&now);
    cJSON_AddNumberToObject(root, "timestamp", (long long)now);

    // Sensors obj
    cJSON *sensors = cJSON_CreateArray();

    for (auto& dataPoint : data) {
        cJSON* sensor = cJSON_CreateObject();
        cJSON_AddNumberToObject(sensor, "s_id", dataPoint.sensor_id);
        cJSON_AddNumberToObject(sensor, "rh%", dataPoint.humidity);
        cJSON_AddNumberToObject(sensor, "temp_c", dataPoint.temperature);

        cJSON_AddItemToArray(sensors, sensor);
    }

    cJSON_AddItemToObject(root, "sensors", sensors);

    // cJSON hands back a buffer it allocated and expects the caller to release it. Assigning
    // straight into a std::string copies the text and then loses the only pointer to that
    // buffer, so it could never be freed. Held in a variable, copied, then released.
    char* printed = cJSON_PrintUnformatted(root);

    cJSON_Delete(root); // Independent of the printed buffer, so released either way

    if (printed == NULL) {
        // Only reachable if an allocation failed while building or printing the tree. Returning
        // empty rather than constructing a std::string from NULL, which is undefined behaviour.
        // Callers must treat an empty result as a failed cycle, never as a packet to send.
        ESP_LOGE(TAG, "Failed to serialise packet");
        return std::string();
    }

    std::string jsonString(printed);
    cJSON_free(printed); // cJSON_free, not free: cJSON may hold custom allocators

    return trim_precision(jsonString); // Trim int values in string
}