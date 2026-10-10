#ifndef BUILTIN_LED_H
#define BUILTIN_LED_H

#include <stdint.h>

/*
 * Coarse progress feedback on the board's built-in LED. Unlike status_led.h
 * (the NeoPixel, DEBUG_MODE only), this runs in every build: it's a single
 * GPIO with no extra rail to power.
 *
 * Flashing is driven by an esp_timer so it keeps going through the blocking
 * WiFi/NTP/HTTPS calls in setup() rather than only updating between them.
 */

/* Solid on. Call once at the very start of setup(). */
void builtin_led_begin();

/* Flash, toggling every period_ms / 2. Replaces any flash already running. */
void builtin_led_flash(uint32_t period_ms);

/* Stop flashing and turn off. */
void builtin_led_off();

#endif
