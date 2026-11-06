// Gemeinsame Objekte und Funktionen des Hauptprogramms (rover.ino)
#pragma once

#include <Arduino.h>

#include "gnss.h"
#include "motion.h"
#include "radio.h"
#include "settings.h"
#include "tracklog.h"

extern Gnss gnss;
extern Radio radio;
extern TrackLog trackLog;
extern Motion motion;

uint16_t batteryMv();
bool buttonPressed();              // IO0 LOW oder IO25 HIGH
const BandProfile* currentBand();  // passendes AT+BAND-Profil oder nullptr
uint32_t txIntervalMs();           // wirksamer Ziel-Sendeabstand bei Fix (auto oder fest)
void printStatus(Print& out);
void printJson(Print& out);         // Einstellungen und Status als JSON-Zeile (AT+JSON)
bool applyRadioSettings();
void powerOff(const char* reason);  // Deep Sleep, Aufwecken per Taste
void appSetup();
void appLoop();
