#ifndef CONFIG_H
#define CONFIG_H

#include <ArduinoLog.h>

// The local timezone is set by the POSIX TZ rule in src/setup_time.cpp,
// not here - change it there.

// ===== Artwork Source =====
// Packed frames are served as static files. Anything reachable over HTTPS
// works; this points at the repo itself. The trailing slash matters, and
// manifest.txt is expected alongside the images.
#define ART_BASE_URL "https://raw.githubusercontent.com/FutureSharks/e-paper-art-display/main/artwork/processed/"

// ===== Display Cycle =====
#define CYCLE_ALL_IMAGES true // Step through the manifest in order; false = picks at random
#define AVOID_RECENT 8        // In random mode, how many recent images not to repeat
// Choose one schedule: a fixed local time each day, or a fixed interval.
#define DISPLAY_UPDATE_TIME "05:00" // Refresh once a day at this local time, 24 h "HH:MM".
                                    // Set to "" to refresh every SLEEP_MINUTES instead
#define SLEEP_MINUTES 5       // Sleep between image refresh when DISPLAY_UPDATE_TIME is "".
                              // Also the wait before retrying a failed WiFi, NTP or download
#define RETRY_MINUTES 1       // Sleep after the panel fails to refresh, before trying again
#define DEEP_SLEEP true      // Deep sleep between images. false stays awake and restarts instead; set to true unless debugging

// ===== On-screen Status Line =====
// One line of small text in the top-left corner of the image.
#define SHOW_BATTERY_PERCENTAGE true // Battery level, e.g. "Battery 87%"
#define SHOW_IMAGE_INDEX true        // Which image is displayed, e.g. "Image 3/14"
#define SHOW_REFRESH_TIME true       // When the image was drawn, e.g. "28 Sep 14:05"

// ===== Battery Configuration =====
// MAX17048 fuel gauge on I2C at 0x36. A refresh takes ~35 s and drives the
// panel's boost converters hard, so below the low threshold the frame paints
// the error screen one last time and sleeps until it is reset.
#define BATTERY_LOW_PERCENT 10        // At or below this, stop: charge, then press reset
#define BATTERY_CHARGING_VOLTAGE 4.0f // Above this the cell is on a charger, so a low reading is safe to refresh through

// ===== Network Configuration =====
#define NTP_SERVER "pool.ntp.org" // SNTP server

// ===== Logging Configuration =====
#define DEBUG_MODE false // Verbose logging and the status LED

#if DEBUG_MODE
#define LOG_LEVEL LOG_LEVEL_VERBOSE
#else
#define LOG_LEVEL LOG_LEVEL_NOTICE
#endif

#endif
