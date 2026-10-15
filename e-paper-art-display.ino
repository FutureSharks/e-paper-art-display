/* e-paper-art-display: an e-paper art frame.
 *
 * The device wakes, fetches a pre-processed image from artwork/processed/ over
 * HTTPS, streams it straight into the panel, and sleeps again. The frame is
 * never held in memory, so this runs on a C6 with no PSRAM. When that fails -
 * no WiFi, no clock, a failed download, or a flat battery - it shows an error
 * image built into the firmware instead (src/error_screen.h).
 *
 * Hardware: XIAO ESP32-C6 + Pimoroni Inky Impression 13.3" (EL133UF1).
 * Wiring:   libs/el133/src/el133_pins.h
 * Settings: src/config.h, credentials in src/secrets.h
 * Images:   tools/imgproc turns artwork/*.jpg into the panel's byte format.
 */

#include <Arduino.h>
#include <ArduinoLog.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "artfeed.h"
#include "el133.h"
#include "src/battery.h"
#include "src/builtin_led.h"
#include "src/config.h"
#include "src/error_screen.h"
#include "src/power_rail.h"
#include "src/secrets.h"
#include "src/setup_time.h"
#include "src/setup_wifi.h"
#include "src/status_line.h"

#ifndef ART_BASE_URL
#error "ART_BASE_URL is not set: copy it from src/secrets_template.h into src/secrets.h"
#endif

/* A secrets.h made before ART_AUTH_TOKEN existed: no token, public artwork. */
#ifndef ART_AUTH_TOKEN
#define ART_AUTH_TOKEN ""
#endif

static const uint64_t US_PER_SECOND = 1000000ULL;
static const uint32_t MS_PER_SECOND = 1000UL;
static const uint32_t SECONDS_PER_MINUTE = 60;

/* For sleep_for_minutes(): no timer, so only a reset wakes the board. */
static const uint32_t SLEEP_FOREVER = 0;

/* SLEEP_MINUTES 0 skips sleep entirely: pause this long after each image (or
 * failure), then restart straight into the next. Overrides DISPLAY_UPDATE_TIME. */
static constexpr bool NO_SLEEP = SLEEP_MINUTES == 0;
static const uint32_t NO_SLEEP_PAUSE_S = 5;

/* DISPLAY_UPDATE_TIME as minutes past midnight, NO_UPDATE_TIME for "", or
 * BAD_UPDATE_TIME if it is not a 24 h "HH:MM". Parsed at compile time so a
 * typo fails the build instead of the frame. */
static const int NO_UPDATE_TIME = -1;
static const int BAD_UPDATE_TIME = -2;

static constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

static constexpr int parse_update_time(const char *s)
{
  if (s[0] == '\0')
    return NO_UPDATE_TIME;
  if (!is_digit(s[0]) || !is_digit(s[1]) || s[2] != ':' ||
      !is_digit(s[3]) || !is_digit(s[4]) || s[5] != '\0')
    return BAD_UPDATE_TIME;
  const int hour = (s[0] - '0') * 10 + (s[1] - '0');
  const int minute = (s[3] - '0') * 10 + (s[4] - '0');
  if (hour > 23 || minute > 59)
    return BAD_UPDATE_TIME;
  return hour * 60 + minute;
}

static constexpr int UPDATE_TIME = parse_update_time(DISPLAY_UPDATE_TIME);
static_assert(UPDATE_TIME != BAD_UPDATE_TIME,
              "DISPLAY_UPDATE_TIME must be a 24 h \"HH:MM\", or \"\" to use SLEEP_MINUTES");
/* SLEEP_MINUTES 0 wins over a daily time: the point of it is to cycle. */
static constexpr bool DAILY_UPDATE = UPDATE_TIME != NO_UPDATE_TIME && !NO_SLEEP;

/* After a daily refresh, the next one is at least this far off. The RTC drifts
 * over a day of deep sleep, so the frame can wake a little before the set time;
 * without this it would see the set time minutes away and refresh twice. */
static const uint32_t DAILY_MIN_GAP_S = 60 * SECONDS_PER_MINUTE;

/* Attempts at WiFi + NTP before showing the error screen, and the wait
 * between them. */
static const int MAX_INIT_ATTEMPTS = 5;
static const uint32_t INIT_RETRY_DELAY_MS = 5000;

/* Built-in LED flash periods: slower once WiFi + NTP are up, faster while the
 * manifest/image are downloading. */
static const uint32_t LED_FLASH_WAITING_MS = 600;
static const uint32_t LED_FLASH_DOWNLOADING_MS = 150;

/* Why the chip started. Worth logging every boot: with USB CDC, any reset
 * drops the serial port, so a crash mid-refresh looks identical to unplugging
 * the cable. ESP_RST_BROWNOUT here means the 3V3 rail collapsed - almost
 * certainly the panel's boost converters at the start of a refresh. */
static const char *reset_reason_name(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_POWERON:  return "power-on";
  case ESP_RST_EXT:      return "external reset";
  case ESP_RST_SW:       return "software restart";
  case ESP_RST_PANIC:    return "panic / exception";
  case ESP_RST_INT_WDT:  return "interrupt watchdog";
  case ESP_RST_TASK_WDT: return "task watchdog";
  case ESP_RST_WDT:      return "other watchdog";
  case ESP_RST_DEEPSLEEP: return "deep sleep wake";
  case ESP_RST_BROWNOUT: return "BROWNOUT - the 3V3 rail collapsed";
  case ESP_RST_SDIO:     return "SDIO";
  default:               return "unknown";
  }
}

/* Sleep until the next attempt, or until reset with SLEEP_FOREVER. Never
 * returns: a timer wake reboots the chip, and with DEEP_SLEEP off that is faked
 * with a busy wait plus an explicit restart, so the cycle still exercises the
 * same cold-start path. */
static void sleep_for_seconds(uint32_t seconds, const char *why)
{
  if (seconds == SLEEP_FOREVER)
    Log.notice("%s; sleeping until reset" CR, why);
  else
    Log.notice("%s; sleeping %dh %02dm %02ds" CR, why, (int)(seconds / 3600),
               (int)(seconds / 60 % 60), (int)(seconds % 60));
  Serial.flush();

  builtin_led_off();

#if !DEEP_SLEEP
  if (seconds == SLEEP_FOREVER)
  {
    Log.notice("DEEP_SLEEP off: staying awake until reset" CR);
    for (;;)
      delay(1000);
  }

  /* Stay powered so the log can be read, then restart to get the clean slate
   * a deep-sleep wake would have given. */
  Log.notice("DEEP_SLEEP off: staying awake, then restarting" CR);
  delay(seconds * MS_PER_SECOND);
  Serial.flush();
  esp_restart();
#else
  if (seconds != SLEEP_FOREVER)
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * US_PER_SECOND);
  /* Drop the STEMMA QT / I2C pull-up regulator and latch it off for the
   * duration of the sleep. Nothing on that rail is needed while the frame is
   * idle, and battery_begin() turns it back on at the next boot. */
  power_rail_off_for_deep_sleep();
  esp_deep_sleep_start();
#endif
}

static void sleep_for_minutes(uint32_t minutes, const char *why)
{
  sleep_for_seconds(minutes * SECONDS_PER_MINUTE, why);
}

/* Wait out SLEEP_MINUTES before the next image or retry. With NO_SLEEP that is
 * a short pause and a restart, staying awake. Never returns. */
static void sleep_until_next_cycle(const char *why)
{
  if (!NO_SLEEP)
    sleep_for_minutes(SLEEP_MINUTES, why);

  Log.notice("%s; SLEEP_MINUTES is 0, restarting in %u s" CR, why,
             (unsigned)NO_SLEEP_PAUSE_S);
  builtin_led_off();
  delay(NO_SLEEP_PAUSE_S * MS_PER_SECOND);
  Serial.flush();
  esp_restart();
}

/* Put the error screen up, then either sleep_until_next_cycle() to try again
 * or, if `retry` is false, sleep until reset. `what` is the headline;
 * `detail`, if given, is one or more lines of specifics under it - the URL, the
 * HTTP status - and the last line says when, if the clock is known, and what
 * happens next. Turn the radio off first. Never returns. */
static void fail(const char *what, const char *detail, bool retry)
{
  char when[24] = "";
  if (time_valid())
  {
    /* Set here too because the failure may have come before time_set(). */
    time_zone_set();
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    strftime(when, sizeof(when), "%d %b %H:%M - ", &local);
  }

  const uint32_t minutes = SLEEP_MINUTES;
  char next[40];
  if (!retry)
    snprintf(next, sizeof(next), "stopped, press reset to restart");
  else if (NO_SLEEP)
    snprintf(next, sizeof(next), "trying again in %u s", (unsigned)NO_SLEEP_PAUSE_S);
  else if (minutes >= 120 && minutes % 60 == 0)
    snprintf(next, sizeof(next), "trying again in %u h", (unsigned)(minutes / 60));
  else
    snprintf(next, sizeof(next), "trying again in %u min", (unsigned)minutes);

  char message[512];
  const bool has_detail = detail != nullptr && detail[0] != '\0';
  snprintf(message, sizeof(message), "%s\n%s%s%s%s", what, has_detail ? detail : "",
           has_detail ? "\n" : "", when, next);

  if (!error_screen_show(message))
    Log.error("The error screen did not refresh either" CR);
  if (retry)
    sleep_until_next_cycle(what);
  sleep_for_minutes(SLEEP_FOREVER, what);
}

/* Set once the frame has downloaded and the panel is refreshing, so a failed
 * artfeed_show() can be put down to the network or to the panel. */
static bool refresh_started = false;

static void on_artfeed_stage(ArtfeedStage stage)
{
  switch (stage)
  {
  case ARTFEED_STAGE_MANIFEST:
  case ARTFEED_STAGE_DOWNLOAD:
    builtin_led_flash(LED_FLASH_DOWNLOADING_MS);
    break;
  case ARTFEED_STAGE_REFRESH:
    refresh_started = true;
    builtin_led_off();
    /* Start recording the cell voltage: this is the load that browns the
     * board out, and the result is only readable on the next boot. */
    battery_monitor_start();
    break;
  }
}

/* The specifics of an artfeed failure for the error screen: the reason, then
 * the URL split after its last '/', so the file name gets a line to itself
 * rather than being wrapped part-way through. */
static void artfeed_detail(char *detail, size_t len)
{
  const ArtfeedError &err = artfeed_last_error();
  const char *file = strrchr(err.url, '/');
  if (file == nullptr)
    snprintf(detail, len, "%s%s%s", err.reason, err.url[0] ? "\n" : "", err.url);
  else
    snprintf(detail, len, "%s\n%.*s\n%s", err.reason, (int)(file + 1 - err.url), err.url,
             file + 1);
}

/* One attempt at the network-dependent part of startup: WiFi association then
 * the clock, which TLS needs to check certificate dates. Returns what failed,
 * for the error screen, with the specifics in `detail`, or nullptr. */
static const char *init_network(char *detail, size_t detail_len)
{
  if (!wifi_connect())
  {
    snprintf(detail, detail_len, "Network \"%s\": %s", WIFI_SSID, wifi_failure_reason());
    return "Could not connect to WiFi";
  }

  /* Resync every wake on a daily schedule: a day of RTC drift would otherwise
   * accumulate and walk the refresh away from DISPLAY_UPDATE_TIME. */
  if (!time_set(DAILY_UPDATE))
  {
    snprintf(detail, detail_len, "No answer from %s", NTP_SERVER);
    return "Could not set the clock (NTP)";
  }

  return nullptr;
}

void setup()
{
  builtin_led_begin();

  Serial.begin(115200);
  unsigned long serial_start = millis();
  while (!Serial && (millis() - serial_start) < 3000)
    delay(10);

  Log.begin(LOG_LEVEL, &Serial);
  Log.notice("=== e-paper-art-display ===" CR);

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_BROWNOUT)
    Log.error("Last reset: %s" CR, reset_reason_name(reason));
  else
    Log.notice("Last reset: %s" CR, reset_reason_name(reason));

  el133_begin();

  /* Check the cell before doing anything expensive. A refresh is ~35 s of the
   * panel's boost converters at full tilt; browning out during one leaves a
   * half-written frame on the glass. */
  battery_begin();
  battery_log();

  /* What happened during the last refresh, which could not be watched live:
   * attaching USB to read the log also supplies the board well enough to hide
   * the fault. */
  battery_report_previous(reason == ESP_RST_BROWNOUT);

  /* Stop on a low cell unless it is on a charger, which shows up as a
   * terminal voltage above anything the cell reaches at rest. The error screen
   * costs one more refresh, but tells whoever looks at the frame to charge it;
   * after that nothing runs until the board is reset. */
  BatteryStatus battery;
  const bool battery_ok = battery_read(battery);
  if (battery_ok && battery.present && battery.percent <= BATTERY_LOW_PERCENT &&
      battery.voltage < BATTERY_CHARGING_VOLTAGE)
  {
    char what[40];
    snprintf(what, sizeof(what), "Battery low (%d%%) - please charge",
             (int)(battery.percent + 0.5f));
    fail(what, nullptr, false);
    return;
  }

  const char *network_error = nullptr;
  char detail[384] = "";
  for (int attempt = 1; attempt <= MAX_INIT_ATTEMPTS; attempt++)
  {
    Log.notice("Startup attempt %d of %d" CR, attempt, MAX_INIT_ATTEMPTS);
    network_error = init_network(detail, sizeof(detail));
    if (network_error == nullptr)
      break;

    wifi_reset();
    if (attempt < MAX_INIT_ATTEMPTS)
      delay(INIT_RETRY_DELAY_MS);
  }

  if (network_error != nullptr)
  {
    fail(network_error, detail, true);
    return;
  }

  builtin_led_flash(LED_FLASH_WAITING_MS);

  ArtfeedConfig cfg = artfeed_default_config();
  cfg.base_url = ART_BASE_URL;
  cfg.auth_token = ART_AUTH_TOKEN;
  cfg.on_stage = on_artfeed_stage;

  char name[128];
  uint32_t position = 0, count = 0;
  if (!artfeed_pick(cfg, name, sizeof(name), &position, &count))
  {
    wifi_reset();
    artfeed_detail(detail, sizeof(detail));
    fail("Could not load the image list", detail, true);
    return;
  }

  /* The refresh is the heaviest load in the whole cycle, so bracket it with
   * battery readings: a big drop across these two lines points at the supply
   * rather than at the panel or the download. */
  battery_log();

  /* Optional one-line caption, painted over the top of the frame as it
   * streams. Uses the at-rest battery reading from before WiFi came up. */
  status_line_begin(position, count, name, battery_ok ? &battery : nullptr);

  /* Streams, then drops the radio before the refresh. On failure the panel
   * keeps the image it already had. */
  bool shown = artfeed_show(cfg, name);
  battery_monitor_stop(shown);
  status_line_end();

  if (!shown && !refresh_started)
  {
    wifi_reset();
    artfeed_detail(detail, sizeof(detail));
    fail("Could not download the image", detail, true);
    return;
  }

  if (!shown)
  {
    /* The frame arrived but the panel would not refresh, so it would not take
     * the error screen either. Try the whole cycle again soon. */
    sleep_for_minutes(RETRY_MINUTES, "The panel did not refresh");
    return;
  }

  battery_log();
  Log.notice("Showing \"%s\"" CR, name);

  if (DAILY_UPDATE)
  {
    Log.notice("Next refresh at %s" CR, DISPLAY_UPDATE_TIME);
    sleep_for_seconds(seconds_until_local_time(UPDATE_TIME / 60, UPDATE_TIME % 60,
                                               DAILY_MIN_GAP_S),
                      "Image painted");
  }
  else
    sleep_until_next_cycle("Image painted");
}

void loop()
{
  /* Never reached: sleep_for_seconds() either deep sleeps or restarts. */
  delay(1000);
}
