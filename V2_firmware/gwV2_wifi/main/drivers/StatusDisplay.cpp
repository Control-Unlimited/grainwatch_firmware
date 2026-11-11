#include "../includes/StatusDisplay.hpp"

void initialize_status_display() {
    ledc_timer_config_t timerConf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timerConf);

    ledc_channel_config_t chanConf = {
        .gpio_num = RED_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&chanConf);

    chanConf.gpio_num = GREEN_PIN;
    chanConf.channel = LEDC_CHANNEL_1;
    ledc_channel_config(&chanConf);

    chanConf.gpio_num = BLUE_PIN;
    chanConf.channel = LEDC_CHANNEL_2;
    ledc_channel_config(&chanConf);
}

void set_status_display(Status status) {
    auto it = StatusRegistry.find(status);

    if (it != StatusRegistry.end()) {
        Colour c = it->second;

        // Apply red (Channel 0)
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, c.red);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

        // Apply green (Channel 1)
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, c.green);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);

        // Apply blue (Channel 2)
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, c.blue);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    }
}