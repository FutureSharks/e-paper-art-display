#include "builtin_led.h"

#include <Arduino.h>
#include <esp_timer.h>

static esp_timer_handle_t flash_timer = nullptr;
static bool led_on = false;

static void set(bool on)
{
  led_on = on;
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
}

static void toggle(void *)
{
  set(!led_on);
}

/* Stops and frees any running flash timer. Safe to call when none is running. */
static void stop_timer()
{
  if (flash_timer == nullptr)
    return;
  esp_timer_stop(flash_timer);
  esp_timer_delete(flash_timer);
  flash_timer = nullptr;
}

void builtin_led_begin()
{
  pinMode(LED_BUILTIN, OUTPUT);
  set(true);
}

void builtin_led_flash(uint32_t period_ms)
{
  stop_timer();

  const esp_timer_create_args_t args = {
    .callback = &toggle,
    .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "builtin_led",
  };
  esp_timer_create(&args, &flash_timer);
  set(true);
  esp_timer_start_periodic(flash_timer, (uint64_t)period_ms * 1000ULL / 2);
}

void builtin_led_off()
{
  stop_timer();
  set(false);
}
