#pragma once

#include "config.h"

// ============================================================================
// LED hardware
// ============================================================================

void setupLEDs();
void showLEDs();

// ============================================================================
// Display helpers (used internally and in display modes)
// ============================================================================

uint8_t addSat(uint8_t base, uint8_t add);
void    addPixelWrap(int index, uint8_t r, uint8_t g, uint8_t b);
void    hsvToRgb(uint16_t hue, uint8_t sat, uint8_t val,
                 uint8_t& r, uint8_t& g, uint8_t& b);
void    applyBrightnessAndShow();
bool    extractClock(int& hour12, int& hour24,
                     int& minute, int& second, int& daySeconds);
void    overlayTimeMarkers(int hour12, int minute, int second, int secTrailLen);

// ============================================================================
// Display mode dispatcher + all individual modes
// ============================================================================

void displayClock();
void displayClock_Solid();
void displayMode_Simple();
void displayMode_Pulse();
void displayMode_Pastel();
void displayMode_Binary();
void displayMode_HourMarker();
void displayMode_Neon();
void displayMode_Comet();
void displayMode_Flame();
