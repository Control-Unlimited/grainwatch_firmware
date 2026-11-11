#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define BUTTON_GPIO GPIO_NUM_36

#define POW_3V3 GPIO_NUM_38
#define BUZZER GPIO_NUM_45
#define BATTERY GPIO_NUM_1

void init_button(TaskHandle_t taskToNotify);

void enable_3v3_pow();

void disable_3v3_pow();

void buzz_buzzer();

void config_battery_pin();

int read_battery_mvol();