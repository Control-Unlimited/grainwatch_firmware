#include "../includes/GPIO.hpp"

static void IRAM_ATTR button_isr_handler(void* arg) {
    TaskHandle_t target = (TaskHandle_t)arg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(target, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

void init_button(TaskHandle_t taskToNotify) {
    // GPIO configuration
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO), // Bitmask of the pins
        .mode = GPIO_MODE_INPUT,               // Set as input mode
        .pull_up_en = GPIO_PULLUP_ENABLE,      // Enable pull-up resistor
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // Disable pull-down
        .intr_type = GPIO_INTR_NEGEDGE         // Trigger on falling edge (Press)
    };
    
    gpio_config(&io_conf);

    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);

    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, (void*) taskToNotify);
}

void enable_3v3_pow() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << POW_3V3),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,  
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);

    gpio_set_level(POW_3V3, 1);
}

void disable_3v3_pow() {
    gpio_set_level(POW_3V3, 0);
}

void buzz_buzzer() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUZZER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // 2. Short buzz logic
    gpio_set_level(BUZZER, 1);           // Turn buzzer ON
    vTaskDelay(pdMS_TO_TICKS(100));      // Buzz for 100ms
    gpio_set_level(BUZZER, 0);           // Turn buzzer OFF
}

static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc1_cali_handle = NULL;
static bool do_cali = false;

void config_battery_pin() {
adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,    // Added to satisfy missing initializer
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    // Channel config - ORDER MATTERS IN C++
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,             // atten comes first!
        .bitwidth = ADC_BITWIDTH_DEFAULT,     // bitwidth comes second
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config));

    // Initialize Calibration
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    
    // This checks if the chip has calibration values burnt into eFuse
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, &adc1_cali_handle);
    if (ret == ESP_OK) {
        do_cali = true;
    }
}

int read_battery_mvol() {
    int adc_raw;
    
    // Get raw reading
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &adc_raw));
    
    ESP_LOGI("ADC", "Raw ADC: %d", adc_raw);

    return adc_raw * 0.76; 
}