#include "main.hpp"

static const char* TAG = "Main Program";
TaskHandle_t workTaskHandle = NULL;

void app_main() {
    ESP_LOGI(TAG, "ESP32-S2 Control Unlimited");

    /*
    // --- Start Test ---
    // * Remove this area *

    enable_3v3_pow();

    SensorManager sensorManager;

    while (1) {
        //enable_3v3_pow();

        int sensors = sensorManager.scan_i2c_bus();
        ESP_LOGI(TAG, "%d sensors", sensors);

        std::vector<struct SensorItem> sensorData = sensorManager.collect_sensor_data();

        //disable_3v3_pow();

        for (auto sensor : sensorData) {
            ESP_LOGI(TAG, "0x%02x) Humidity: %f | Temp: %f", sensor.sensor_id, sensor.humidity, sensor.temperature);
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    // * Remove this area *
    // --- End Test ---
    */

    // ---  Initialization  ---
    // Enable 3v3 pow
    enable_3v3_pow();

    // Startup Buzz
    buzz_buzzer();

    // Init ADC
    config_battery_pin();

    // Status RGB led
    initialize_status_display();
    set_status_display(Status::DEVICE_SETUP);
    
    // Net stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create main handle
    TaskHandle_t mainTaskHandle = xTaskGetCurrentTaskHandle();
    
    // Init button interrupt
    ESP_LOGD(TAG, "Initializing button interrupt");
    init_button(mainTaskHandle);
    
    // Init NVS
    ESP_LOGI(TAG, "Initializing NVS");
    int ret = init_NVS();
    ESP_LOGD(TAG, "Init NVS: %d", ret);

    // Write fallback credentials if this device has never been provisioned
    ret = seed_default_creds_to_NVS();
    ESP_LOGD(TAG, "Seed default creds: %d", ret);

    // --- Start Work Task ---
    ESP_LOGD(TAG, "Starting work task");
    xTaskCreate(work_task, "work_task", 16384, (void*)mainTaskHandle, 1, &workTaskHandle);

    // --- Wait Until Interrupted ---
    ESP_LOGD(TAG, "Waiting until notified...");
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Work Task Exited");

    // --- Delete Work Task ---
    ESP_LOGD(TAG, "Deleting work task");
    if (workTaskHandle != NULL) {
        vTaskDelete(workTaskHandle);
        workTaskHandle = NULL;
    }

    // Clean up hardware
    ESP_LOGD(TAG, "Cleaning up hardware");
    esp_err_t wifi_status = esp_wifi_stop();
    if (wifi_status != ESP_ERR_WIFI_NOT_INIT) {
        esp_wifi_deinit();
    }

    // --- Start AP Task ---
    ESP_LOGI(TAG, "Starting AP Task");
    set_status_display(Status::AP_ACTIVE);
    ProvManager provManager;

    buzz_buzzer();
    vTaskDelay(pdMS_TO_TICKS(100));
    buzz_buzzer();
    vTaskDelay(pdMS_TO_TICKS(100));
    buzz_buzzer();

    provManager.start_ap();
    provManager.start_dns_server();
    provManager.start_webserver();

    // Keep alive to so provManager doesnt go out of scope
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
