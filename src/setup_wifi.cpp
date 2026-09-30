#include <ArduinoLog.h>
#include <WiFi.h>

#include "secrets.h"
#include "setup_wifi.h"

// Give up associating after this long.
static const uint32_t WIFI_TIMEOUT_MS = 30000;

// Where the last failed wifi_connect() got to, for wifi_failure_reason().
static wl_status_t failed_status = WL_IDLE_STATUS;

bool wifi_connect()
{
  Log.notice("Connecting to WiFi: %s" CR, WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUT_MS)
  {
    delay(500);
    Log.trace("." CR);
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    failed_status = WiFi.status();
    Log.error("WiFi connection failed after %d ms, status %d" CR, (int)(millis() - start),
              (int)failed_status);
    return false;
  }

  Log.notice("Connected in %d ms, IP: %s, RSSI: %d dBm" CR, (int)(millis() - start),
             WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  return true;
}

const char *wifi_failure_reason()
{
  switch (failed_status)
  {
  case WL_NO_SSID_AVAIL:   return "network not found";
  case WL_CONNECT_FAILED:  return "connection refused - wrong password?";
  case WL_CONNECTION_LOST: return "connection lost";
  default:                 return "timed out waiting to connect";
  }
}

// Tear down WiFi between attempts so the next wifi_connect() starts clean, and
// before the refresh - the radio is the expensive part of the wake cycle.
void wifi_reset()
{
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(500);
}
