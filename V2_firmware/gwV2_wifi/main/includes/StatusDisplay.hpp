#pragma once

#include <stdint.h>
#include <map>
#include "driver/ledc.h"

#define RED_PIN   5
#define GREEN_PIN 7
#define BLUE_PIN  6

struct Colour {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

enum class Status {
    WIFI_CONNECTION_FAIL,
    MQTT_CONNECTION_FAIL,
    I2C_BUS_FAIL,
    WIFI_ACTIVE,
    AP_ACTIVE,
    DEVICE_SETUP,
};


/*
        These colour values can be changed. The 3 values in a row represent Red, Green, Blue.
        In this case the maximum value is 255 and the minimum 0.
        If you want to create the colour purple you would want something similar to 192, 0, 255.
*/
const std::map<Status, Colour> StatusRegistry = {
    {
        Status::WIFI_CONNECTION_FAIL, {
            255, 0, 0 // Red
        }
    },
    {
        Status::MQTT_CONNECTION_FAIL, {
            255, 255, 0 // Yellow
        }
    },
    {
        Status::I2C_BUS_FAIL, {
            0, 0, 255 // Blue
        }
    },
    {
        Status::WIFI_ACTIVE, {
            0, 255, 0 // Green
        }
    },
    {
        Status::AP_ACTIVE, {
            0, 255, 255 // Cyan
        }
    },
    {
        Status::DEVICE_SETUP, {
            128, 255, 255 // White
        }
    },
};
// Dont change anything below this line

extern const std::map<Status, Colour> StatusRegistry;

void initialize_status_display();

void set_status_display(Status status);