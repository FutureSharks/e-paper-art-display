#include "battery.h"

#include <Adafruit_MAX1704X.h>
#include <Arduino.h>
#include <ArduinoLog.h>
#include <Wire.h>

#include "config.h"

// Below this the gauge is reading a floating pin rather than a cell.
static const float BATTERY_MIN_PLAUSIBLE_V = 2.5f;

/* Adafruit_MAX17048::begin() soft-resets the gauge, which throws away its
 * state-of-charge model: VCELL and SOC both read 0 until it has re-measured,
 * and the fresh SOC is only an estimate from the cell voltage at that moment -
 * badly wrong while the cell is on a charger. The gauge runs from the cell
 * whether the ESP32 is awake or not, so it has been tracking charge the whole
 * time; there is nothing to reset. This attaches without the reset. */
class Gauge : public Adafruit_MAX17048
{
public:
  bool attach(TwoWire *wire)
  {
    delete i2c_dev;
    delete status_reg;
    status_reg = nullptr;

    i2c_dev = new Adafruit_I2CDevice(MAX17048_I2CADDR_DEFAULT, wire);
    if (!i2c_dev->begin() || !isDeviceReady())
      return false;

    status_reg = new Adafruit_BusIO_Register(i2c_dev, MAX1704X_STATUS_REG);
    enableSleep(false);
    sleep(false);
    return true;
  }
};

static Gauge gauge;
static bool gauge_ready = false;

/* Without the soft reset the registers are normally valid straight away. They
 * are not when the gauge itself has just powered up - a cell plugged in a
 * moment ago - where both read 0 until its first measurement, which
 * battery_read() would take for "no battery" or "flat". Wait that out. A cell
 * that really is at 0% costs a one-second wait, nothing more. */
static void wait_for_first_conversion()
{
  unsigned long start = millis();
  while (millis() - start < 1000)
  {
    float v = gauge.cellVoltage();
    float pct = gauge.cellPercent();
    if (!isnan(v) && v > 0.0f && !isnan(pct) && pct > 0.0f)
    {
      Log.verbose("Gauge reading valid after %d ms" CR, (int)(millis() - start));
      return;
    }
    delay(20);
  }
  Log.verbose("Gauge still reading 0 after 1000 ms" CR);
}

bool battery_begin()
{
  Wire.begin();

  if (gauge.attach(&Wire))
  {
    Log.verbose("MAX17048 found, chip ID 0x%x" CR, gauge.getChipID());
    wait_for_first_conversion();
    gauge_ready = true;
    return true;
  }

#ifdef PIN_NEOPIXEL_I2C_POWER
  /* GPIO20 switches the STEMMA QT rail on this board. The onboard gauge should
   * sit on the unswitched side, so this is only a fallback - left off by
   * default because it also powers the NeoPixel, which costs current we would
   * rather not spend on a battery device. */
  Log.verbose("Gauge silent; enabling the switched I2C rail and retrying" CR);
  pinMode(PIN_NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(PIN_NEOPIXEL_I2C_POWER, HIGH);
  delay(50);

  if (gauge.attach(&Wire))
  {
    Log.verbose("MAX17048 found, chip ID 0x%x" CR, gauge.getChipID());
    wait_for_first_conversion();
    gauge_ready = true;
    return true;
  }
#endif

  Log.warning("No MAX17048 on I2C; battery monitoring disabled" CR);
  gauge_ready = false;
  return false;
}

bool battery_read(BatteryStatus &status)
{
  status = BatteryStatus{};

  if (!gauge_ready)
    return false;

  float voltage = gauge.cellVoltage();
  if (isnan(voltage))
    return false;

  status.voltage = voltage;
  status.percent = gauge.cellPercent();

  /* The gauge answers whether or not a cell is attached; on USB power with no
   * battery it reports a floating pin. Treat that as "no battery" rather than
   * as a flat one, so the frame still updates. */
  status.present = status.voltage >= BATTERY_MIN_PLAUSIBLE_V;

  if (status.percent > 100.0f)
    status.percent = 100.0f;

  return true;
}

void battery_log()
{
  BatteryStatus status;
  if (!battery_read(status))
  {
    Log.notice("Battery: gauge not responding" CR);
    return;
  }

  if (!status.present)
  {
    Log.notice("Battery: none detected (%F V), running from USB" CR, status.voltage);
    return;
  }

  Log.notice("Battery: %F%%, %F V" CR, status.percent, status.voltage);
}

/* ---- Refresh-time sag recording ------------------------------------- */

#include <Preferences.h>

static TaskHandle_t monitor_task = nullptr;
static volatile bool monitor_run = false;
static volatile uint16_t monitor_min_mv = 0;
static uint16_t monitor_persisted_mv = 0;

/* Only write when the minimum drops meaningfully, so a 35 s refresh costs a
 * handful of NVS writes rather than one per sample. */
static const uint16_t PERSIST_STEP_MV = 20;

/* Cell voltage sampling interval during a refresh. */
static const uint32_t MONITOR_INTERVAL_MS = 100;

static void persist_min(uint16_t mv)
{
  Preferences prefs;
  prefs.begin("battery", false);
  prefs.putUShort("minmv", mv);
  prefs.end();
  monitor_persisted_mv = mv;
}

static void monitor_loop(void *)
{
  while (monitor_run)
  {
    float v = gauge.cellVoltage();
    if (!isnan(v))
    {
      uint16_t mv = (uint16_t)(v * 1000.0f);
      if (monitor_min_mv == 0 || mv < monitor_min_mv)
      {
        monitor_min_mv = mv;
        if (monitor_persisted_mv == 0 || mv + PERSIST_STEP_MV <= monitor_persisted_mv)
          persist_min(mv);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));
  }
  monitor_task = nullptr;
  vTaskDelete(nullptr);
}

void battery_monitor_start()
{
  if (!gauge_ready || monitor_task != nullptr)
    return;

  monitor_min_mv = 0;
  monitor_persisted_mv = 0;

  /* Record the starting state before the risky part, so a refresh that ends in
   * a brownout still leaves a usable trace. */
  BatteryStatus status;
  Preferences prefs;
  prefs.begin("battery", false);
  prefs.putUChar("inprog", 1);
  prefs.putUShort("startpct", battery_read(status) ? (uint16_t)(status.percent * 100) : 0);
  prefs.putUShort("minmv", 0);
  prefs.end();

  monitor_run = true;
  xTaskCreate(monitor_loop, "batmon", 3072, nullptr, 1, &monitor_task);
}

float battery_monitor_stop(bool completed)
{
  if (!monitor_run)
    return 0.0f;

  monitor_run = false;
  /* Give the task a cycle to notice and exit before touching NVS again. */
  delay(MONITOR_INTERVAL_MS * 2);

  uint16_t mv = monitor_min_mv;

  Preferences prefs;
  prefs.begin("battery", false);
  prefs.putUChar("inprog", 0);
  prefs.putUChar("done", completed ? 1 : 0);
  if (mv > 0)
    prefs.putUShort("minmv", mv);
  prefs.end();

  if (mv > 0)
    Log.notice("Refresh low-water mark: %F V" CR, mv / 1000.0f);

  return mv / 1000.0f;
}

void battery_report_previous(bool was_brownout)
{
  Preferences prefs;
  prefs.begin("battery", false);
  uint8_t in_progress = prefs.getUChar("inprog", 0);
  uint8_t completed = prefs.getUChar("done", 1);
  uint16_t min_mv = prefs.getUShort("minmv", 0);
  uint16_t start_pct = prefs.getUShort("startpct", 0);
  uint32_t brownouts = prefs.getUInt("bocount", 0);

  if (was_brownout)
  {
    brownouts++;
    prefs.putUInt("bocount", brownouts);
  }

  /* A refresh still flagged in progress means the last run never came back -
   * the board reset part way through. */
  if (in_progress)
    prefs.putUChar("inprog", 0);
  prefs.end();

  /* Report the running brownout tally first: it is meaningful even when there
   * is no refresh record yet, which is the case on the first boot after
   * flashing firmware that did not write one. */
  if (brownouts > 0)
    Log.notice("Brownout resets so far: %d" CR, (int)brownouts);

  if (min_mv == 0 && !in_progress)
  {
    Log.notice("No previous refresh recorded" CR);
    return;
  }

  Log.notice("Previous refresh: started at %F%%, low-water %F V, %s" CR,
             start_pct / 100.0f, min_mv / 1000.0f,
             in_progress ? "DID NOT FINISH" : (completed ? "completed" : "failed"));
}
