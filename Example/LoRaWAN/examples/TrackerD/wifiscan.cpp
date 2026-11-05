#include "wifiscan.h"
#include "WiFi.h"

// Ergebnis des WLAN-Scans (wird in do_send() als Port-8-Uplink verschickt):
//   sys.databuf   = genau 6 Zeichen SSID (letzte 6 Zeichen, kuerzere SSIDs mit 'c' aufgefuellt)
//   sys.wifi_rssi = RSSI des gewaehlten Access Points
//   sys.ble_flag  = 1: Ergebnis liegt vor, 2: nichts gefunden im Hybridmodus (-> GPS)
// Nutzlast ist identisch zum Dragino-Original.

#define WIFI_SSID_LEN   6
#define WIFI_MIN_RSSI   (-90)
#define WIFI_LED_PIN    2

void wifi_set_empty(void)
{
  memset(sys.databuf, 'f', WIFI_SSID_LEN);
  sys.databuf[WIFI_SSID_LEN] = '\0';
  sys.wifi_rssi = 0;
}

static void wifi_store(const char *ssid, int32_t rssi)
{
  size_t len = strlen(ssid);
  if (len > WIFI_SSID_LEN)
  {
    memcpy(sys.databuf, ssid + (len - WIFI_SSID_LEN), WIFI_SSID_LEN);
  }
  else
  {
    memset(sys.databuf, 'c', WIFI_SSID_LEN);
    memcpy(sys.databuf, ssid, len);
  }
  sys.databuf[WIFI_SSID_LEN] = '\0';
  sys.wifi_rssi = (int8_t)constrain(rssi, -128, 127);
}

static bool is_ascii(const char *s)
{
  for (; *s; s++)
  {
    if (*s & 0x80) return false;
  }
  return true;
}

void wifi_scan(void)
{
  if (sys.lon == 1)
  {
    pinMode(WIFI_LED_PIN, OUTPUT);
    digitalWrite(WIFI_LED_PIN, HIGH);
  }

  // wifimask_data ist im EEPROM nicht zwingend nullterminiert -> begrenzte Kopie
  char mask[11];
  strncpy(mask, sys.wifimask_data, sizeof(mask) - 1);
  mask[sizeof(mask) - 1] = '\0';
  bool use_mask = !(strlen(mask) < 6 || strncmp(mask, "000000", 6) == 0);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  Serial.println("Scanning...");
  int n = WiFi.scanNetworks();
  if (n < 0)
  {
    Serial.printf("WiFi scan failed (%d)\r\n", n);
    n = 0;
  }
  Serial.printf("%d networks found\r\n", n);

  int best = -1;
  int32_t best_rssi = INT32_MIN;
  for (int i = 0; i < n; i++)
  {
    String ssid = WiFi.SSID(i);
    int32_t rssi = WiFi.RSSI(i);
    if (sys.showid == 1)
    {
      Serial.printf("%d WIFI: %s: %d\r\n", i + 1, ssid.c_str(), rssi);
    }
    if (!is_ascii(ssid.c_str()))
      continue;
    if (use_mask)
    {
      if (strstr(ssid.c_str(), mask) == NULL)
        continue;
    }
    else if (rssi < WIFI_MIN_RSSI)
    {
      continue;
    }
    if (rssi > best_rssi)
    {
      best = i;
      best_rssi = rssi;
    }
  }

  if (best >= 0)
  {
    wifi_store(WiFi.SSID(best).c_str(), best_rssi);
    sys.ble_flag = 1;
  }
  else if (sys.sensor_mode == 3)
  {
    sys.ble_flag = 2;       // Hybridmodus: kein WLAN -> alarm_state() startet GPS
  }
  else
  {
    wifi_set_empty();
    sys.ble_flag = 1;
    Serial.println("no matching networks found");
  }
  if (sys.showid == 1 && sys.ble_flag == 1)
  {
    Serial.printf("WiFi: %s %d\r\n", sys.databuf, sys.wifi_rssi);
  }

  WiFi.scanDelete();
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  digitalWrite(WIFI_LED_PIN, LOW);
}
