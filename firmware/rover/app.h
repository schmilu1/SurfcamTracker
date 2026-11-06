// Gemeinsame Objekte und Funktionen des Hauptprogramms (rover.ino)
#pragma once

#include <Arduino.h>

#include "gnss.h"
#include "radio.h"
#include "settings.h"
#include "tracklog.h"

extern Gnss gnss;
extern Radio radio;
extern TrackLog trackLog;

uint16_t batteryMv();
bool buttonPressed();
const BandProfile* currentBand();  // passendes AT+BAND-Profil oder nullptr
uint32_t txIntervalMs();           // wirksamer Ziel-Sendeabstand bei Fix (auto oder fest)  // IO0 LOW oder IO25 HIGH
void printStatus(Print& out);
bool applyRadioSettings();
void powerOff(const char* reason);  // Deep Sleep, Aufwecken per Taste
void appSetup();
void appLoop();
