#ifndef POWER_RAIL_H
#define POWER_RAIL_H

/*
 * The Feather ESP32-C6 has a second 3V3 regulator, separate from the one the
 * EN pin controls, feeding the STEMMA QT connector and the onboard NeoPixel.
 * It is switched by GPIO20 (PIN_NEOPIXEL_I2C_POWER) and is enabled by default
 * by the Arduino core, so it draws current on every boot whether or not
 * anything on it is used.
 *
 * Nothing the frame needs in normal operation sits on this rail: the MAX17048
 * gauge is on the unswitched side, and the NeoPixel is only lit in DEBUG_MODE.
 * So it is dropped before deep sleep, and held down through it - GPIO20 is a
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
