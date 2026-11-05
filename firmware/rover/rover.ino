/*
 * SurfCam Rover – Firmware für den Dragino TrackerD mit Quectel L76K
 *
 * Liest Positionen vom GNSS-Modul (bis 10 Hz), loggt jede gültige Position im Flash
 * und sendet die jeweils neueste Position per LoRa Punkt-zu-Punkt an das Gateway
 * (869.525 MHz, SF7, 250 kHz, max. 10 % Duty Cycle -> bis ca. 3.9 Pakete/s).
 *
 * Bedienung:
 *   Einschalten     Taste 1 s halten (aus dem Sleep) bzw. Akku anschliessen / Reset
 *   Ausschalten     Taste 3 s halten -> rote LED 1 s -> Deep Sleep
 *   Kurz drücken    Status auf USB, Akkustand als 1-4 grüne Blinker
 *
 * LEDs (jede Sekunde ein kurzer Blinker):
 *   grün            GNSS-Fix
 *   rot kurz        kein Fix
 *   rot lang        keine Daten vom GNSS-Modul
 *   blau            Funkpaket gesendet (höchstens 1x pro Sekunde angezeigt)
 *
 * Details, AT-Befehle und Paketformat: README.md
 */
#include "app.h"

void setup() { appSetup(); }

void loop() { appLoop(); }
