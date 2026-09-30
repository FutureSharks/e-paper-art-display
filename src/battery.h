#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

/*
 * MAX17048 LiPo fuel gauge, on the Feather's I2C bus at 0x36, driven by
 * Adafruit's Adafruit_MAX1704X library.
 *
 * Unlike an ADC divider this is a real gauge: it models the cell and reports
 * state of charge directly, so the reading does not swing with load the way a
 * bare terminal-voltage measurement does. That matters here because the panel's
 * boost converters pull hard during a refresh.
 */

struct BatteryStatus
{
  bool present;      // gauge answered and the reading looks like a real cell
  float voltage;     // volts
  float percent;     // 0-100, from the gauge's own model
};

/* Bring up I2C and confirm the gauge is there. Safe to call every wake: it
 * does not reset the gauge, which keeps tracking charge through deep sleep. */
bool battery_begin();

/* Read the gauge. Returns false if it did not answer; `status.present` is
 * false when it answered but no cell appears to be attached - running from USB
 * with no battery, say - which is not an error. */
bool battery_read(BatteryStatus &status);

/* Log the current state at notice level. */
void battery_log();

/* ---- Refresh-time sag recording -------------------------------------
 *
 * The refresh is the load that browns the board out, and it cannot be watched
 * over USB because attaching USB supplies the board well enough to hide the
 * fault. So the minimum cell voltage is sampled in the background, written to
 * NVS as it falls, and reported on the NEXT boot - by which time you can have
 * the serial monitor attached.
 *
 * Caveat: the MAX17048 filters VCELL and updates it roughly every 250 ms, so
 * this records the sustained sag, not the microsecond transient that actually
 * trips the brownout detector. It tells you how much DC headroom is left at a
 * given charge and temperature, and it correlates failures with state of
 * charge. Catching the fast dip needs a scope on the 3V3 rail. */

/* Begin sampling in the background, and mark a refresh as in progress so an
 * attempt that never returns is still visible next boot. */
void battery_monitor_start();

/* Stop sampling and record the outcome. Returns the lowest cell voltage seen,
 * or 0 if nothing was sampled. */
float battery_monitor_stop(bool completed);

/* Report what the previous run recorded. Call once at startup. */
void battery_report_previous(bool was_brownout);

#endif
