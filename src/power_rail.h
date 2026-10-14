#ifndef POWER_RAIL_H
#define POWER_RAIL_H

/*
 * The Feather ESP32-C6 has a second 3V3 regulator, separate from the one the
 * EN pin controls, feeding the STEMMA QT connector, the onboard NeoPixel and
 * the I2C pull-ups. It is switched by GPIO20 (PIN_NEOPIXEL_I2C_POWER).
 *
 * The MAX17048 gauge runs from the cell, but cannot be read without the
 * pull-ups, so battery_begin() enables the rail for the waking cycle. Nothing
 * on it is needed while the frame is idle, so it is dropped before deep sleep,
 * and held down through it - GPIO20 is a
 * plain digital pad on the C6, not an RTC GPIO, so without a hold it goes
 * hi-Z the moment the chip sleeps and the rail is at the mercy of whatever the
 * regulator's enable input floats to.
 */

/* Enable the rail and wait for it to come up. */
void power_rail_on();

/* Disable the rail. */
void power_rail_off();

/* Disable the rail and latch the pad low so it stays disabled while the chip
 * is in deep sleep. Call immediately before esp_deep_sleep_start(). */
void power_rail_off_for_deep_sleep();

#endif
