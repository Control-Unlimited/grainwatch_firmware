#include "../includes/ProvManager.hpp"

static const char* TAG = "Provisioning Manager";

// index.html
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// save.html
extern const uint8_t save_html_start[]  asm("_binary_save_html_start");
extern const uint8_t save_html_end[]    asm("_binary_save_html_end");

// style.css
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");

namespace {
    int hexCharToInt(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    }

    void decode_http_encoding(char* str) {
        int read = 0;
        int write = 0;

        while (str[read] != '\0') {
            if (str[read] == '+') {
                str[write++] = ' ';
                read++;
            } 
            else if (str[read] == '%' && str[read+1] != '\0' && str[read+2] != '\0') {
                str[write++] = (char)((hexCharToInt(str[read + 1]) << 4) | 
                                    hexCharToInt(str[read + 2]));
                read += 3;
            } 
            else {
                // Copy character as-is
                str[write++] = str[read++];
            }
        }
        
        str[write] = '\0';
    }

    static void reset_timer_callback(void* arg) {
        ESP_LOGI("RESET", "Timer expired. Rebooting now...");
        esp_restart();
    }

    static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "station join, AID = %d", event->aid);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(TAG, "station leave, AID=%d, reason = %d", event->aid, event->reason);
        }
    }

    static esp_err_t get_main_handler(httpd_req_t *req) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, (const char*)index_html_start, (index_html_end - index_html_start));
    }

    static esp_err_t get_style_handler(httpd_req_t *req) {
        const size_t style_css_size = (style_css_end - style_css_start);
        httpd_resp_set_type(req, "text/css");
        httpd_resp_send(req, (const char *)style_css_start, style_css_size);
        return ESP_OK;
    }

    static esp_err_t post_handler(httpd_req_t *req) {
        char buf[128];
        int ret = httpd_req_recv(req, buf, sizeof(buf));
        if (ret <= 0) return ESP_FAIL;
        buf[ret] = '\0';

        // This arr lengths account for if all values are special characters
        char ssid_val[96];  // 3 * real length
        char pass_val[192]; // 3 * real length

        // Helper to extract values from the form-encoded string
        if (httpd_query_key_value(buf, "ssid", ssid_val, sizeof(ssid_val)) == ESP_OK &&
            httpd_query_key_value(buf, "pass", pass_val, sizeof(pass_val)) == ESP_OK) {
            
            ESP_LOGI("WEB", "Extracted SSID: %s", ssid_val);
            ESP_LOGI("WEB", "Extracted Password: %s", pass_val);

            // Decode the values
            decode_http_encoding(ssid_val);
            decode_http_encoding(pass_val);

            ESP_LOGI("WEB", "Decoded SSID: %s", ssid_val);
            ESP_LOGI("WEB", "Decoded Password: %s", pass_val);

            // Ensure values are correct length
            if (strlen(ssid_val) > 32 || strlen(pass_val) > 64) {
                ESP_LOGW("WEB", "Values given are too long to be a valid login");
            }

            // Logic to save these to NVS or try connecting goes here
            ESP_LOGD(TAG, "Saving extracted credentials");
            ret = write_creds_to_NVS(ssid_val, pass_val);
            if (ret != FUNCTION_SUCCESS) {
                ESP_LOGE(TAG, "Failed to save creds to NVS...");
            }

            const esp_timer_create_args_t reset_timer_args = {
                .callback = &reset_timer_callback,
                .name = "reset_timer"
            };
            esp_timer_handle_t reset_timer;
            esp_timer_create(&reset_timer_args, &reset_timer);
            esp_timer_start_once(reset_timer, 2000000);
        }

        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, (const char*)save_html_start, (save_html_end - save_html_start));
    }

    void dns_server_task(void *pvParameters) {
        uint8_t data[128];
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(53); // DNS Port

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE("DNS", "Failed to create socket");
            vTaskDelete(NULL);
        }

        bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

        while (true) {
            struct sockaddr_in source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, data, sizeof(data), 0, (struct sockaddr *)&source_addr, &socklen);

            if (len > 12) {
                // DNS Response: QR=1, AA=1
                data[2] |= 0x80; data[3] |= 0x80;
                // Set Answer Count to 1
                data[6] = 0; data[7] = 1;

                int idx = len;
                data[idx++] = 0xc0; data[idx++] = 0x0c; // Name offset
                data[idx++] = 0x00; data[idx++] = 0x01; // Type A
                data[idx++] = 0x00; data[idx++] = 0x01; // Class IN
                data[idx++] = 0x00; data[idx++] = 0x00; data[idx++] = 0x00; data[idx++] = 0x3c; // TTL 60s
                data[idx++] = 0x00; data[idx++] = 0x04; // Length 4
                // Respond with 192.168.4.1
                data[idx++] = 192; data[idx++] = 168; data[idx++] = 4; data[idx++] = 1;

                sendto(sock, data, idx, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
            }
        }
    }
}

void ProvManager::start_ap() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_AP");

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    memcpy(wifi_config.ap.ssid, SSID_ACCESS_POINT, std::min(strlen(SSID_ACCESS_POINT), sizeof(wifi_config.ap.ssid)));
    wifi_config.ap.ssid_len = strlen(SSID_ACCESS_POINT);
    memcpy(wifi_config.ap.password, PASS_ACCESS_POINT, std::min(strlen(PASS_ACCESS_POINT), sizeof(wifi_config.ap.password)));
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA3_PSK;
    wifi_config.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_config.ap.pmf_cfg.required = true;
    wifi_config.ap.bss_max_idle_cfg.period = WIFI_AP_DEFAULT_MAX_IDLE_PERIOD;
    wifi_config.ap.bss_max_idle_cfg.protected_keep_alive = 1;
    wifi_config.ap.gtk_rekey_interval = true;

    if (strlen(PASS_ACCESS_POINT) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wifi Init finished.");
}

void ProvManager::start_dns_server() {
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "DNS Server Task Started");
}

void ProvManager::start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t index_uri = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = get_main_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t style_uri = {
            .uri      = "/style.css",
            .method   = HTTP_GET,
            .handler  = get_style_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &style_uri);

        httpd_uri_t post_uri = {
            .uri      = "/save",
            .method   = HTTP_POST,
            .handler  = post_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &post_uri);

        httpd_uri_t captive = { 
            .uri="/*", 
            .method=HTTP_GET, 
            .handler=get_main_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &captive);

        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, [](httpd_req_t *req, httpd_err_code_t err) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        });
        
        ESP_LOGI(TAG, "Web server started on 192.168.4.1");
    }
}