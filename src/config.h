#ifndef CONFIG_H
#define CONFIG_H

#include <ArduinoLog.h>

// The artwork source, ART_BASE_URL, is set in src/secrets.h.

// ===== Time Zone =====
// A POSIX TZ rule, including the daylight saving dates. This one is
// Europe/Berlin. For other zones see the second column of
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#define TIME_ZONE "CET-1CEST,M3.5.0,M10.5.0/3"

// ===== Display Cycle =====
// Choose one schedule: a fixed local time each day, or a fixed interval.
#define DISPLAY_UPDATE_TIME "05:00" // Refresh once a day at this local time, 24 h "HH:MM".
                                    // Set to "" to refresh every SLEEP_MINUTES instead
#define SLEEP_MINUTES 30      // Sleep between image refresh when DISPLAY_UPDATE_TIME is "".
                              // Also the wait before retrying a failed WiFi, NTP or download.
                              // 0 = never sleep: pause 5 s, then straight on to the next image,
                              // ignoring DISPLAY_UPDATE_TIME
#define RETRY_MINUTES 10      // Sleep after the panel fails to refresh, before trying again
#define DEEP_SLEEP true       // Deep sleep between images. false stays awake and restarts instead; set to true unless debugging

// ===== On-screen Status Line =====
// One line of small text in the top-left corner of the image.
#define SHOW_BATTERY_PERCENTAGE false  // Battery level, e.g. "Battery 87%"
#define SHOW_IMAGE_INDEX false         // Which image is displayed, e.g. "Image 3/14"
#define SHOW_FILE_NAME false           // The displayed file's name, e.g. "0vcc_155_hilma-af-klint_portrait.bin"
#define SHOW_REFRESH_TIME false        // When the image was drawn, e.g. "28 Sep 14:05"

// ===== Battery Configuration =====
// MAX17048 fuel gauge on I2C at 0x36. A refresh takes ~35 s and drives the
// panel's boost converters hard, so below the low threshold the frame paints
// the error screen one last time and sleeps until it is reset.
#define BATTERY_LOW_PERCENT 10        // At or below this, stop: charge, then press reset
#define BATTERY_CHARGING_VOLTAGE 4.0f // Above this the cell is on a charger, so a low reading is safe to refresh through

// ===== Network Configuration =====
#define NTP_SERVER "pool.ntp.org" // SNTP server

// ===== Logging Configuration =====
#define DEBUG_MODE false // Verbose logging

#if DEBUG_MODE
#define LOG_LEVEL LOG_LEVEL_VERBOSE
#else
#define LOG_LEVEL LOG_LEVEL_NOTICE
#endif

#endif
