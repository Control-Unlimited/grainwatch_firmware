#include "../includes/NVS.hpp"

static const char* TAG = "Non-Volatile Storage Handler";

bool initialized = false;

int init_NVS() {
    if (initialized) {
        ESP_LOGW(TAG, "Non-Volatile Storage has already been initialized...");
        return -1;
    }


    ESP_LOGI(TAG, "Initializing Non-Volatile Storage"); 

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    initialized = true;
    return FUNCTION_SUCCESS;
}

int write_creds_to_NVS(const std::string ssid, const std::string pass) {
    if (!initialized) {
        ESP_LOGW(TAG, "Non-Volatile Storage has not yet been initialized...");
        return -1;
    }


    ESP_LOGI(TAG, "Writing to Non-Volatile Storage. SSID: %s", ssid.c_str());

    esp_err_t err;
    std::unique_ptr<nvs::NVSHandle> handle = nvs::open_nvs_handle("storage", NVS_READWRITE, &err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return -2;
    }

    err = handle->set_string(SSID_NVS_KEY, ssid.c_str());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) while writing SSID to NVS", esp_err_to_name(err));
        return -3;
    }

    err = handle->set_string(PASS_NVS_KEY, pass.c_str());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) while writing SSID to NVS", esp_err_to_name(err));
        return -4;
    }

    err = handle->commit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) while commiting NVS changes", esp_err_to_name(err));
        return -5;
    }

    return FUNCTION_SUCCESS;
}

int seed_default_creds_to_NVS() {
#ifndef SEED_DEFAULT_WIFI_CREDS
    return FUNCTION_SUCCESS;
#else
    if (!initialized) {
        ESP_LOGW(TAG, "Non-Volatile Storage has not yet been initialized...");
        return -1;
    }

    esp_err_t err;
    std::unique_ptr<nvs::NVSHandle> handle = nvs::open_nvs_handle("storage", NVS_READWRITE, &err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return -2;
    }

    // Only seed a factory-fresh device. Anything already stored belongs to the customer.
    size_t requiredSize;
    if (handle->get_item_size(nvs::ItemType::SZ, SSID_NVS_KEY, requiredSize) == ESP_OK) {
        ESP_LOGI(TAG, "Credentials already present in NVS, skipping default seed");
        return FUNCTION_SUCCESS;
    }

    ESP_LOGI(TAG, "No credentials in NVS. Seeding compile-time defaults. SSID: %s", DEFAULT_WIFI_SSID);
    return write_creds_to_NVS(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
#endif
}

std::pair<int, struct NVSData> read_creds_from_NVS() {
    struct NVSData returnData;

    if (!initialized) {
        ESP_LOGW(TAG, "Non-Volatile Storage has not yet been initialized...");
        return {-1, returnData};
    }


    ESP_LOGI(TAG, "Reading from Non-Volatile Storage");

    esp_err_t err;
    std::unique_ptr<nvs::NVSHandle> handle = nvs::open_nvs_handle("storage", NVS_READWRITE, &err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return {-2, returnData};
    }

    size_t requiredSize;

    err = handle->get_item_size(nvs::ItemType::SZ, SSID_NVS_KEY, requiredSize);
    if (err == ESP_OK) {
        std::vector<char> buf(requiredSize);
        handle->get_string(SSID_NVS_KEY, buf.data(), requiredSize);
        returnData.ssid = std::string(buf.data());
    } else {
        ESP_LOGW(TAG, "SSID not found or error: %s", esp_err_to_name(err));
        return {-3, returnData};
    }

    err = handle->get_item_size(nvs::ItemType::SZ, PASS_NVS_KEY, requiredSize);
    if (err == ESP_OK) {
        std::vector<char> buf(requiredSize);
        handle->get_string(PASS_NVS_KEY, buf.data(), requiredSize);
        returnData.pass = std::string(buf.data());
    } else {
        ESP_LOGW(TAG, "Pass not found or error: %s", esp_err_to_name(err));
        return {-3, returnData};
    }

    return {FUNCTION_SUCCESS, returnData};
}

int store_packet(std::string packet) {
    if (!initialized) {
        ESP_LOGW(TAG, "Non-Volatile Storage has not yet been initialized...");
        return -1;
    }

    ESP_LOGI(TAG, "Writing packet to Non-Volatile Storage.");

    esp_err_t err;
    std::unique_ptr<nvs::NVSHandle> handle = nvs::open_nvs_handle("storage", NVS_READWRITE, &err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return -2;
    }

    // Get current stored values
    uint32_t count = 0;
    err = handle->get_item(PACKET_COUNT_KEY, count);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return -3;
    }

    // Dump if too many packets stored
    if (count >= MAX_PACKETS) {
        ESP_LOGW(TAG, "Too many messages already stored...");
        return -6;
    }

    // Create unique key
    std::string key = PACKET_KEY_PREFIX + std::to_string(count);

    // Store the packet string
    err = handle->set_string(key.c_str(), packet.c_str());
    if (err != ESP_OK) {
        // Called out separately because the partition filling up is the realistic failure here,
        // not a corrupt write. MAX_PACKETS is a count, so it does not bound the bytes used.
        if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
            ESP_LOGE(TAG, "NVS full at %" PRIu32 " stored packets. Dropping this one.", count);
        } else {
            ESP_LOGW(TAG, "Error (%s) while writing packet to NVS", esp_err_to_name(err));
        }
        return -4;
    }

    // Increment counter
    count++;
    handle->set_item(PACKET_COUNT_KEY, count);

    err = handle->commit();
    return (err == ESP_OK) ? 0 : -5;
}

std::pair<int, std::vector<std::string>> read_stored_packets() {
    std::vector<std::string> packets;
    if (!initialized) return {-1, packets};

    esp_err_t err;
    std::unique_ptr<nvs::NVSHandle> handle = nvs::open_nvs_handle("storage", NVS_READWRITE, &err);
    if (err != ESP_OK) return {-2, packets};

    // Get the total count
    uint32_t count = 0;
    err = handle->get_item(PACKET_COUNT_KEY, count);
    
    if (err == ESP_ERR_NVS_NOT_FOUND || count == 0) {
        return {0, packets}; // Nothing to read
    }

    // Loop through and collect all packets
    for (uint32_t i = 0; i < count; i++) {
        std::string key = PACKET_KEY_PREFIX + std::to_string(i);
        size_t requiredSize;
        
        if (handle->get_item_size(nvs::ItemType::SZ, key.c_str(), requiredSize) == ESP_OK) {
            std::vector<char> buf(requiredSize);
            handle->get_string(key.c_str(), buf.data(), requiredSize);
            packets.push_back(std::string(buf.data()));
            
            // Invalidate/Erase the message after reading
            handle->erase_item(key.c_str());
        }
    }

    // Reset counter to 0 and commit
    handle->set_item(PACKET_COUNT_KEY, (uint32_t)0);
    handle->commit();

    return {0, packets};
}

struct NetFailureState read_net_failures_from_NVS() {
    struct NetFailureState state;

    if (!initialized) {
        ESP_LOGW(TAG, "Non-Volatile Storage has not yet been initialized...");
        return state;
    }

    esp_err_t err;
    std::unique_ptr<nvs::NVSHandle> handle = nvs::open_nvs_handle("storage", NVS_READWRITE, &err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return state;
    }

    // A missing key leaves the value alone, so the zeroed defaults stand for a device that has
    // never failed. Nothing to report is indistinguishable from nothing recorded, by design.
    handle->get_item(NET_FAILS_NVS_KEY, state.fails);
    handle->get_item(NET_REASON_NVS_KEY, state.reason);

    return state;
}

int record_net_failure_to_NVS(uint8_t reason) {
    if (!initialized) {
        ESP_LOGW(TAG, "Non-Volatile Storage has not yet been initialized...");
        return -1;
    }

    esp_err_t err;
    std::unique_ptr<nvs::NVSHandle> handle = nvs::open_nvs_handle("storage", NVS_READWRITE, &err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return -2;
    }

    uint32_t count = 0;
    handle->get_item(NET_FAILS_NVS_KEY, count);

    // Saturate rather than wrap. A device that has failed four billion cycles in a row has a
    // problem the exact figure will not help with, but a wrap to zero would read as recovery.
    if (count < UINT32_MAX) {
        count++;
    }

    err = handle->set_item(NET_FAILS_NVS_KEY, count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) while writing failure count to NVS", esp_err_to_name(err));
        return -3;
    }

    err = handle->set_item(NET_REASON_NVS_KEY, reason);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) while writing failure reason to NVS", esp_err_to_name(err));
        return -4;
    }

    ESP_LOGI(TAG, "Recorded failed cycle %" PRIu32 " (reason %d)", count, reason);

    err = handle->commit();
    return (err == ESP_OK) ? FUNCTION_SUCCESS : -5;
}

int clear_net_failures_in_NVS() {
    if (!initialized) {
        ESP_LOGW(TAG, "Non-Volatile Storage has not yet been initialized...");
        return -1;
    }

    esp_err_t err;
    std::unique_ptr<nvs::NVSHandle> handle = nvs::open_nvs_handle("storage", NVS_READWRITE, &err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return -2;
    }

    uint32_t count = 0;
    uint8_t reason = 0;
    handle->get_item(NET_FAILS_NVS_KEY, count);
    handle->get_item(NET_REASON_NVS_KEY, reason);

    // A healthy device runs this path on every wake. Writing only when there is something to
    // clear keeps a working unit from spending a flash write per cycle for no change.
    if (count == 0 && reason == 0) {
        return FUNCTION_SUCCESS;
    }

    ESP_LOGI(TAG, "Clearing %" PRIu32 " recorded failures", count);

    handle->set_item(NET_FAILS_NVS_KEY, (uint32_t)0);
    handle->set_item(NET_REASON_NVS_KEY, (uint8_t)0);

    err = handle->commit();
    return (err == ESP_OK) ? FUNCTION_SUCCESS : -3;
}