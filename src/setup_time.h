#ifndef SETUP_TIME_H
#define SETUP_TIME_H

#include <stdint.h>

/* Set the clock from NTP. Skipped when the clock already survived deep sleep,
 * unless `resync`: the RTC drifts, which only matters when waking at a set
 * time of day. A resync NTP fails to answer keeps the old clock. */
bool time_set(bool resync = false);
void print_local_time();

/* Apply the local timezone. time_set() does this too; call it on its own to
 * format a time when NTP never ran. */
void time_zone_set();

/* Whether the clock has been set since power-on - by NTP this wake, or by one
 * before it, since the RTC keeps running through deep sleep. */
bool time_valid();

/* Seconds from now until the next hour:minute local time that is at least
 * min_gap_s away. Needs a valid clock. */
uint32_t seconds_until_local_time(int hour, int minute, uint32_t min_gap_s);

#endif
