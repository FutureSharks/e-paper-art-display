#include <Arduino.h>
#include <ArduinoLog.h>

#include "config.h"
#include "esp_sntp.h"
#include "setup_time.h"
#include "time.h"

// TimeZone rule for Europe/Berlin including daylight adjustment rules
const char *time_zone = "CET-1CEST,M3.5.0,M10.5.0/3";

// The clock only has to be roughly right: it exists so TLS can check
// certificate validity dates, which are measured in months.
static const time_t CLOCK_SANE = 1700000000; // 2023-11-14

static const unsigned long NTP_TIMEOUT_MS = 15000;

// Set by the SNTP callback, from the network task.
static volatile bool ntp_synced = false;

void print_local_time()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Log.warning("No time available (yet)" CR);
    return;
  }
  char time_str[64];
  strftime(time_str, sizeof(time_str), "%A, %B %d %Y %H:%M:%S", &timeinfo);
  Log.notice("%s" CR, time_str);
}

void time_available(struct timeval *t)
{
  ntp_synced = true;
  Log.notice("Got time adjustment from NTP!" CR);
  print_local_time();
}

void time_zone_set()
{
  // The zone lives in the environment, which does not survive a restart or a
  // deep sleep wake, so set it every boot - even when NTP is skipped. Without
  // it local time is UTC.
  setenv("TZ", time_zone, 1);
  tzset();
}

bool time_valid()
{
  return time(nullptr) >= CLOCK_SANE;
}

bool time_set(bool resync)
{
  time_zone_set();

  // The RTC keeps running through deep sleep, so after the first sync the
  // clock is usually still good and the whole round trip can be skipped.
  if (!resync && time_valid())
  {
    Log.notice("Clock survived deep sleep, skipping NTP" CR);
    print_local_time();
    return true;
  }

  Log.notice("Syncing time from NTP: %s" CR, NTP_SERVER);

  ntp_synced = false;
  sntp_set_time_sync_notification_cb(time_available);
  configTzTime(time_zone, NTP_SERVER);

  // Wait for the callback rather than for a plausible clock: on a resync the
  // clock is already plausible before NTP has said anything.
  unsigned long start = millis();
  while (!ntp_synced && millis() - start < NTP_TIMEOUT_MS)
    delay(10);

  if (ntp_synced)
    return true;

  if (time_valid())
  {
    Log.warning("NTP did not answer, keeping the clock from before deep sleep" CR);
    return true;
  }

  Log.error("Failed to sync time from NTP" CR);
  return false;
}

uint32_t seconds_until_local_time(int hour, int minute, uint32_t min_gap_s)
{
  time_zone_set();
  time_t now = time(nullptr);

  struct tm target;
  localtime_r(&now, &target);
  time_t next;
  // Today's slot, or tomorrow's if today's is past or too close. mktime()
  // normalises the fields in place - rolling the month over, and moving a time
  // that falls in a DST gap - so they are set afresh on each pass.
  for (;;)
  {
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_sec = 0;
    target.tm_isdst = -1; // Let mktime() work out whether DST applies that day
    next = mktime(&target);
    if (next - now >= (time_t)min_gap_s)
      break;
    target.tm_mday += 1;
  }

  return (uint32_t)(next - now);
}
