#pragma once

/* ---- Configuration Start ---- */
// Credentials used to log into the Wireless Access-Point during configuration.
#define SSID_ACCESS_POINT "GrainWatch"   // AP name the customer sees to connect too
#define PASS_ACCESS_POINT "Password"     // Password they use to log in


// Credentials written to NVS on the very first boot, before the customer has provisioned the
// device. They let a factory-fresh unit join a known network for testing. Once the customer
// submits their own details through the provisioning page these are overwritten and never used
// again. Pressing the button always returns the device to provisioning mode.
// NOTE: These are stored in the firmware as plain text and can be recovered from the flash of any
//       physical device. Comment out SEED_DEFAULT_WIFI_CREDS for customer builds.
#define SEED_DEFAULT_WIFI_CREDS
#define DEFAULT_WIFI_SSID "Control Unlimited WIFI"
#define DEFAULT_WIFI_PASS "ControlUnlimited"


// How many times Wi-Fi should re-attempt to connect before a fatal error (Fatal errors cause the device to display an error LED & reset)
#define MAXIMUM_WIFI_RETRY_ATTEMPTS 5


// The signal strength reported in the uplink is an average of this many readings. A single
// beacon's RSSI swings by several dB, so one reading is not worth trending. Each extra sample
// costs RSSI_SAMPLE_INTERVAL_MS of awake time with the radio already on.
#define RSSI_SAMPLE_COUNT 3
#define RSSI_SAMPLE_INTERVAL_MS 100


// This time is ideally sufficiently long enough so that during a fatal error (e.g., no internet) the customer can see the LED
#define TIME_TO_STAY_ACTIVE_AFTER_ERROR_SECONDS 60
// This is the amount of time to reboot and retry program after an error has occured
#define TIME_TO_SLEEP_FOR_AFTER_ERROR_SECONDS 21600 // 6 hours


// This value dictates the precision of values after the decimal place in the MQTT message.
// EXAMPLE: With a value of 2 a number that was 3.14159 will become 3.14
// NOTE: This value cannot be 0, it MUST be 1 or more
#define MQTT_VALUE_PRECISION 2


// How long to wait for a successful MQTT connection before displaying fatal error and resetting
#define MAX_MQTT_CONNECTION_WAIT_SECONDS 150

// How long to wait for a MQTT Subscribe message to come in before displaying fatal error and resetting
#define MAX_MQTT_SUB_RECEIVE_WAIT_SECONDS 150

// Sleep time (minutes) used until a downlink sets one. After that the device keeps to the
// downlink's value on every wake, with no upper limit. The value lives in RTC memory, so a
// battery pull, reset or reflash brings it back to this default.
#define DEFAULT_SLEEP_TIME_MIN 360

// How long to wait for a MQTT puback (Confirmation that the broker has received our publish) before displaying fatal error and resetting
#define MAX_MQTT_PUBACK_WAIT_SECONDS 10

/*

    The colours used to display status on the RGB LED can be customized from within the StatusDisplay.hpp file
    located in the same directory as this file. Ensure you read the comment left in that file.

*/

/* ----  Configuration End  ---- */



/* ----  Do Not Edit These  ---- */
#define SSID_NVS_KEY "SSID"
#define PASS_NVS_KEY "PASS"

#define MAX_PACKETS 100
#define PACKET_COUNT_KEY "PKT_CNT"
#define PACKET_KEY_PREFIX "P" // P0, P1, P2, ...

// Consecutive failed cycles and the Wi-Fi disconnect reason from the most recent one. These live
// in NVS rather than RTC memory: RTC data is re-initialized on any reset that is not a deep sleep
// wake, so a brownout would erase the count of exactly the failures that caused it.
#define NET_FAILS_NVS_KEY "NET_FAILS"
#define NET_REASON_NVS_KEY "NET_REASON"

#define FUNCTION_SUCCESS 0
/* ----  Do Not Edit These  ---- */