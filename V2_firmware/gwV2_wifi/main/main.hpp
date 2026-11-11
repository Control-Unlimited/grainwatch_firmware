#pragma once

#include <stdio.h>
#include <utility> // For std::pair
#include <vector>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "includes/NVS.hpp"
#include "config/Config.hpp"
#include "includes/GPIO.hpp"
#include "includes/StatusDisplay.hpp"

#include "includes/WorkTask.hpp"

extern "C" {
    void app_main();
}