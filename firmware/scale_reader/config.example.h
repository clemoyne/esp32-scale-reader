// Copy to config.h (not committed) and fill in.
#pragma once

#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-password"

// Network name: http://scale-reader.local/ and the OTA target.
#define HOSTNAME "scale-reader"
#define OTA_PASSWORD "change-me"

// The scale's public address, from scan_scale.py. "" = first device named SCALE_NAME.
#define SCALE_ADDRESS "aa:bb:cc:dd:ee:ff"
#define SCALE_NAME "QN-Scale"

// Display unit the handshake sets on the scale: 1 = lb, 0 = kg.
// Reported weights are always sent in both.
#define DISPLAY_UNIT_LB 1

// Where stable readings are POSTed as JSON. .local names are resolved by mDNS.
#define REPORT_HOST "raspberrypi.local"
#define REPORT_PORT 8091
#define REPORT_PATH "/scale"

// POSIX TZ string for the log timestamps (this one is US/Canada Eastern).
#define TIMEZONE "EST5EDT,M3.2.0,M11.1.0"

// Status LED: GPIO 2 on most ESP32 dev boards (the red power LED can't be driven).
#define LED_PIN 2
#define LED_ON_LEVEL HIGH

// BLE link. Linux BlueZ's default 420 ms supervision timeout dropped the handshake.
#define SUPERVISION_TIMEOUT_MS 4000
#define CONNECT_TIMEOUT_MS 5000
// Disconnect after this long with no weigh-in, so the scale can sleep.
#define IDLE_S 600
