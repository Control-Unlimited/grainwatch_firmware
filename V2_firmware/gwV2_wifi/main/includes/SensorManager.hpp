/*
    RTC code & functionality is in this class for I2C bus simplicity.
*/

#pragma once

#include <vector>
#include <stdint.h>
#include <utility>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#define SDA_PIN 20
#define SCL_PIN 2
#define I2C_PORT 0

#define COMMAND 0xFD

#define RTC_ADDRESS 0x68

struct SensorItem {
    uint8_t sensor_id;
    float humidity;
    float temperature;
};

class SensorManager {
private:
    i2c_master_bus_handle_t master_bus_handle;
    std::vector<std::pair<uint8_t, i2c_master_dev_handle_t>> sensors;
    std::pair<bool, i2c_master_dev_handle_t> rtc_device;

public:
    SensorManager();
    ~SensorManager();

    int scan_i2c_bus();

    std::vector<struct SensorItem> collect_sensor_data();

    esp_err_t DS1337_SetTime(struct tm *t);
    esp_err_t DS1337_GetTime(struct tm *t);
};