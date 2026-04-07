#pragma once

#include "config.h"

// ============================================================================
// LED hardware
// ============================================================================

void setupLEDs();
void showLEDs();

// ============================================================================
// Display helpers
// ============================================================================

uint8_t addSat(uint8_t base, uint8_t add);
void addPixelWrap(int index, uint8_t r, uint8_t g, uint8_t b);
void hsvToRgb(uint16_t hue, uint8_t sat, uint8_t val, uint8_t &r, uint8_t &g, uint8_t &b);
uint8_t computeAutoBrightness();
void applyBrightnessAndShow();
bool extractClock(int &hour12, int &hour24, int &minute, int &second, int &daySeconds);
CRGB blendSpectrumColor(const CRGB &base, uint8_t spectrum, uint8_t level, uint16_t phaseSeed);
ModeDisplayConfig defaultModeConfigFor(uint8_t mode);
void setDefaultModeConfigs();
bool isModeConfigValid(const ModeDisplayConfig &cfg);
void applyModeConfigDefaultsIfInvalid(uint8_t mode);
void overlayTimeMarkers(int hour12, int minute, int second, const ModeDisplayConfig &cfg, int secTrailLen);
void drawWeightedMarker(int pos, int pixelCount, CRGB color, uint8_t weight, uint16_t seedBase, uint8_t spectrum);
void updateFade(HandFade &f, int newPos);

// ============================================================================
// Display mode functions
// ============================================================================

void displayMode_Simple();
void displayMode_Pulse();
void displayMode_Pastel();
void displayMode_Binary();
void displayMode_HourMarker();
void displayMode_Neon();
void displayMode_Comet();
void displayMode_Flame();
void displayBootAnimation();
void displayClock();
void displayClock_Solid();
