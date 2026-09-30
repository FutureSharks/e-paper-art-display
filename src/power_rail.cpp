#include "power_rail.h"

#include <Arduino.h>

#ifdef PIN_NEOPIXEL_I2C_POWER
#include <driver/gpio.h>

static const gpio_num_t RAIL_GPIO = (gpio_num_t)PIN_NEOPIXEL_I2C_POWER;

void power_rail_on()
{
  /* A hold survives a deep-sleep wake, so it has to be released before the pad
   * will take a new level. Harmless when no hold is set. */
  gpio_hold_dis(RAIL_GPIO);
  pinMode(PIN_NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(PIN_NEOPIXEL_I2C_POWER, HIGH);
  delay(10);
}

void power_rail_off()
{
  gpio_hold_dis(RAIL_GPIO);
  pinMode(PIN_NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(PIN_NEOPIXEL_I2C_POWER, LOW);
}

void power_rail_off_for_deep_sleep()
{
  power_rail_off();
  /* On the C6 a digital pad's hold is latched in the always-on domain and
   * survives deep sleep on its own; there is no separate deep-sleep hold
   * enable as there is on the S2/S3. */
  gpio_hold_en(RAIL_GPIO);
}

#else // board has no switched rail

void power_rail_on() {}
void power_rail_off() {}
void power_rail_off_for_deep_sleep() {}

#endif
