#include "../includes/SensorManager.hpp"

static const char *TAG = "SensorManager";

namespace {
    uint8_t decToBcd(int val) {
        return (uint8_t)((val / 10 << 4) | (val % 10));
    }

    int bcdToDec(uint8_t val) {
        return (int)((val >> 4) * 10 + (val & 0x0F));
    }
}

SensorManager::SensorManager() {
    // Initialize I2C
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)SDA_PIN,
        .scl_io_num = (gpio_num_t)SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, // Ignore glitches
        .flags = {.enable_internal_pullup = true},
    };

    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    this->master_bus_handle = bus_handle;
}

SensorManager::~SensorManager() {
    // Remove devices
    for (std::pair<uint8_t, i2c_master_dev_handle_t>& sensor : sensors) {
        i2c_master_bus_rm_device(sensor.second);
    }
    // Remove RTC
    if (rtc_device.first) {
        i2c_master_bus_rm_device(rtc_device.second);
    }
    // Delete bus
    if (master_bus_handle) {
        i2c_del_master_bus(master_bus_handle);
    }
}

int SensorManager::scan_i2c_bus() {
    int count = 0;

    // remove old items
    for (auto& sensor : sensors) {
        i2c_master_bus_rm_device(sensor.second);
    }
    sensors.clear(); // Empty vector

    // Find devices on bus
    for (uint8_t i = 3; i < 0x78; i++) {
        esp_err_t ret = i2c_master_probe(this->master_bus_handle, i, 100);
        if (ret == ESP_OK) {
            i2c_master_dev_handle_t dev;

            // Initialize the dev
            i2c_device_config_t devConfig = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = i,
                .scl_speed_hz = 50000,
            };
            ESP_ERROR_CHECK(i2c_master_bus_add_device(master_bus_handle, &devConfig, &dev));
            
            if (i == RTC_ADDRESS) {
                ESP_LOGI(TAG, "RTC Device found on: 0x%02x", i);
                rtc_device.first = true;
                rtc_device.second = dev;

                continue;
            }

            sensors.push_back({i, dev});

            // Add to log
            ESP_LOGI(TAG, "Found device at: 0x%02x", i);
            count++;
        }
    }

    return count;
}

std::vector<struct SensorItem> SensorManager::collect_sensor_data() {
    uint8_t cmd = COMMAND;
    uint8_t data[6];
    std::vector<struct SensorItem> sensorData;

    for (std::pair<uint8_t, i2c_master_dev_handle_t>& sensor : sensors) {
        // Send command
        esp_err_t err = i2c_master_transmit(sensor.second, &cmd, 1, 100);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Transmission failed for a sensor, err: %s", esp_err_to_name(err));
            continue;
        }

        // Wait for measurement to complete (8.3ms on datasheet)
        // NOTE: If using a different precision (medium or low) this delay can be shorter
        vTaskDelay(pdMS_TO_TICKS(30));

        // Read Result
        err = i2c_master_receive(sensor.second, data, 6, 100);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Receive failed for a sensor, err: %s", esp_err_to_name(err));
            continue;
        }

        // Convert data
        uint16_t t_ticks = (data[0] << 8) | data[1];
        uint16_t rh_ticks = (data[3] << 8) | data[4];
        
        float temperature = -45.0f + 175.0f * (float)t_ticks / 65535.0f;
        float humidity = -6.0f + 125.0f * (float)rh_ticks / 65535.0f;

        // Humidity clipping
        if (humidity > 100) humidity = 100;
        if (humidity < 0) humidity = 0;

        struct SensorItem data = {
            .sensor_id = sensor.first,
            .humidity = humidity,
            .temperature = temperature,
        };
        sensorData.push_back(data);

        ESP_LOGI(TAG, "ID: %d, T: %.2f C, RH: %.2f", sensor.first, temperature, humidity);
    }

    return sensorData;
}

esp_err_t SensorManager::DS1337_SetTime(struct tm *t) {
    if (!rtc_device.first) {
        ESP_LOGE(TAG, "RTC device not connected...");
        return -1;
    }

    uint8_t data[8];
    data[0] = 0x00; // Start at address 00h (Seconds)

    data[1] = decToBcd(t->tm_sec);        // Seconds
    data[2] = decToBcd(t->tm_min);        // Minutes
    data[3] = decToBcd(t->tm_hour);       // Hours (24h)
    data[4] = decToBcd(t->tm_wday + 1);   // Day
    data[5] = decToBcd(t->tm_mday);       // Date
    data[6] = decToBcd(t->tm_mon + 1);    // Month
    data[7] = decToBcd(t->tm_year % 100); // Year (00-99)

    return i2c_master_transmit(rtc_device.second, data, 8, 100);
}

esp_err_t SensorManager::DS1337_GetTime(struct tm *t) {
    uint8_t reg_addr = 0x00;
    uint8_t data[7];

    // 1. Set address pointer to 00h, then read 7 bytes [cite: 586, 591]
    esp_err_t ret = i2c_master_transmit_receive(rtc_device.second, &reg_addr, 1, data, 7, 100);
    if (ret != ESP_OK) return ret;

    // 2. Convert BCD to Decimal 
    t->tm_sec  = bcdToDec(data[0] & 0x7F);
    t->tm_min  = bcdToDec(data[1] & 0x7F);
    t->tm_hour = bcdToDec(data[2] & 0x3F); // Assuming 24h mode [cite: 616]
    t->tm_wday = bcdToDec(data[3] & 0x07) - 1;
    t->tm_mday = bcdToDec(data[4] & 0x3F);
    t->tm_mon  = bcdToDec(data[5] & 0x1F) - 1;
    t->tm_year = bcdToDec(data[6]) + 100; // Offset for 2000s

    return ESP_OK;
}