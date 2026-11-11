#pragma once

#include <string>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"

#include "esp_http_server.h"

#include "NVS.hpp"
#include "../config/Config.hpp"

class ProvManager {
public:
    void start_ap();

    void start_webserver();

    void start_dns_server();
};