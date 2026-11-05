// Gemeinsame Objekte und Funktionen des Hauptprogramms (Rover.ino)
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
void printStatus(Print& out);
bool applyRadioSettings();
void powerOff(const char* reason);  // Deep Sleep, Aufwecken per Taste
void appSetup();
void appLoop();
