// Feste Konfiguration: Pinbelegung (Schaltplan TrackerD v1.2) und Standardwerte
#pragma once

#include <stdint.h>

#define ROVER_FW_VERSION "0.1.2"

namespace pins {
constexpr int GPS_RX = 9;      // ESP RX  <- L76K TXD
constexpr int GPS_TX = 10;     // ESP TX  -> L76K RXD
constexpr int GPS_POWER = 12;  // HIGH = GNSS-Versorgung an (Strapping-Pin, 10k Pulldown auf der Platine)
// IO25 ist je nach Board-Revision der GNSS-Reset (Schaltplan v1.2, HIGH = Reset) oder die
// Taste (neuere Geräte, HIGH = gedrückt). Er wird deshalb nur als Eingang mit Pulldown
// betrieben: als Reset bleibt er inaktiv, als Taste ist er lesbar.

constexpr int LORA_SCK = 5;
constexpr int LORA_MISO = 19;
constexpr int LORA_MOSI = 27;
constexpr int LORA_NSS = 18;
constexpr int LORA_RST = 23;
constexpr int LORA_DIO0 = 26;
constexpr int LORA_DIO1 = 33;

constexpr int LED_RED = 13;
constexpr int LED_BLUE = 2;
constexpr int LED_GREEN = 15;

constexpr int BUTTON = 0;       // Taste ältere Geräte: active-low, auch Boot-Pin
constexpr int BUTTON_HIGH = 25; // Taste neuere Geräte: active-high (siehe Hinweis bei GPS_POWER)
constexpr int BATTERY = 35;    // Teiler 100k / 470k
constexpr int BUZZER = 4;
}  // namespace pins

namespace defaults {
constexpr uint8_t DEVICE_ID = 1;
constexpr uint8_t GNSS_HZ = 5;             // 1, 2, 4, 5 oder 10
constexpr uint16_t TX_INTERVAL_MS = 300;   // Ziel-Sendeabstand bei Fix
constexpr int8_t TX_POWER_DBM = 17;        // 2..20 dBm (PA_BOOST)
constexpr uint32_t FREQ_HZ = 869525000UL;  // Mitte des Bands 869.40-869.65 MHz (10 % Duty Cycle)
constexpr uint8_t SF = 7;
constexpr uint32_t BW_HZ = 250000UL;
constexpr bool LEDS = true;
constexpr uint8_t LOG_HZ = 5;              // Lograte: 0 (aus), 1, 2, 4, 5, 10
}  // namespace defaults

namespace timing {
constexpr uint32_t NO_FIX_TX_INTERVAL_MS = 2000;  // Lebenszeichen ohne Fix
constexpr uint32_t FIX_TIMEOUT_MS = 2000;         // Fix gilt als veraltet
constexpr uint32_t LOG_FLUSH_MS = 5000;
constexpr uint32_t LONG_PRESS_MS = 3000;          // Ausschalten
constexpr uint32_t WAKE_HOLD_MS = 1000;           // Einschalten aus dem Sleep
constexpr uint32_t LED_BEAT_MS = 2000;            // Status-Blitz alle 2 s
constexpr uint32_t LED_FLASH_MS = 15;             // kurzer Blitz (Fix / kein Fix / gesendet)
constexpr uint32_t LED_LONG_MS = 100;             // keine Daten vom GNSS-Modul
constexpr uint16_t BATTERY_EMPTY_MV = 3300;
constexpr uint32_t BATTERY_EMPTY_HOLD_MS = 10000;
}  // namespace timing

constexpr uint8_t LORA_SYNC_WORD = 0x34;  // öffentliches LoRaWAN-Sync-Word, damit das SX1303-Gateway empfängt
