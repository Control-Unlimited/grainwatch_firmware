#pragma once

#include <stdio.h>
#include <utility> // For std::pair
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ProvManager.hpp"
#include "WifiManager.hpp"
#include "NVS.hpp"
#include "SensorManager.hpp"
#include "StatusDisplay.hpp"
#include "MqttManager.hpp"
#include "GPIO.hpp"

#include "../config/Config.hpp"

void work_task(void* pvParameters);