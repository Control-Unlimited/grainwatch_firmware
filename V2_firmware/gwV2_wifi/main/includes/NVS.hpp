#pragma once

#include <stdio.h>
#include <inttypes.h>
#include <string>
#include <utility> // For std::pair
#include <vector>
#include <sstream>
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_handle.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "../config/Config.hpp"

struct NVSData {
    std::string ssid;
    std::string pass;
};

/**
 * @brief Cycles that failed to get an uplink through, and the Wi-Fi disconnect reason from the
 *        most recent one. Reported in the next successful uplink so silent failures are visible.
 */
struct NetFailureState {
    uint32_t fails = 0;
    uint8_t reason = 0;
};

int init_NVS();

int write_creds_to_NVS(const std::string ssid, const std::string pass);

int seed_default_creds_to_NVS();

std::pair<int, struct NVSData> read_creds_from_NVS();

int store_packet(std::string packet);

std::pair<int, std::vector<std::string>> read_stored_packets();

/**
 * @brief Read the failure counters. Absent keys are the normal case on a device that has never
 *        failed, and read back as zero.
 */
struct NetFailureState read_net_failures_from_NVS();

/**
 * @brief Count one failed cycle and record the Wi-Fi disconnect reason behind it.
 *
 * @param reason Disconnect reason seen this cycle, or 0 if Wi-Fi was never the problem.
 */
int record_net_failure_to_NVS(uint8_t reason);

/**
 * @brief Reset the failure counters after an uplink is acknowledged. Writes nothing when they are
 *        already zero, which is the usual case, so a healthy device does not touch flash.
 */
int clear_net_failures_in_NVS();