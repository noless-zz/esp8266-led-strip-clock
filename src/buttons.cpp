#include "buttons.h"
#include "globals.h"
#include "debug.h"
#include "storage.h"

void setupButtons() {
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);
  Serial.printf("[BTN] Button 1 on pin %d, Button 2 on pin %d\n", BTN1_PIN, BTN2_PIN);
}

// Poll both buttons and fire short/long-press actions.
// Call this every loop iteration (it uses millis(), not delay()).
void handleButtons() {
  unsigned long now = millis();

  // --- Button 1: power / mode ---
  {
    bool nowPressed = (digitalRead(BTN1_PIN) == LOW);
    if (nowPressed && !btn1State.wasPressed) {
      // Falling edge: button just pressed
      btn1State.pressedAt  = now;
      btn1State.actionFired = false;
    } else if (nowPressed && btn1State.wasPressed && !btn1State.actionFired) {
      // Held: fire long-press as soon as threshold is crossed
      if (now - btn1State.pressedAt >= BTN_LONG_PRESS_MS) {
        btn1State.actionFired = true;
        if (!ledsOff) {
          ledsOff = true;
          if (ledStrip) { ledStrip->clear(); ledStrip->show(); }
          saveButtonSettings();
          DLOGI("BTN", "Button 1 long press: LEDs OFF");
        }
      }
    } else if (!nowPressed && btn1State.wasPressed) {
      // Rising edge: button released
      if (!btn1State.actionFired && (now - btn1State.pressedAt >= BTN_DEBOUNCE_MS)) {
        // Short press
        if (ledsOff) {
          ledsOff = false;
          saveButtonSettings();
          DLOGI("BTN", "Button 1 short press: LEDs ON");
        } else {
          displayMode = (DisplayMode)((displayMode + 1) % DISPLAY_MAX);
          saveDisplayModeToEEPROM();
          DLOGI("BTN", "Button 1 short press: mode -> %d", (int)displayMode);
        }
      }
    }
    btn1State.wasPressed = nowPressed;
  }

  // --- Button 2: W-channel brightness ---
  {
    bool nowPressed = (digitalRead(BTN2_PIN) == LOW);
    if (nowPressed && !btn2State.wasPressed) {
      btn2State.pressedAt  = now;
      btn2State.actionFired = false;
    } else if (!nowPressed && btn2State.wasPressed) {
      if (!btn2State.actionFired && (now - btn2State.pressedAt >= BTN_DEBOUNCE_MS)) {
        // Short press: advance W brightness level (0→1→2→3→0)
        wBrightLevel = (wBrightLevel + 1) % W_BRIGHT_LEVEL_COUNT;
        saveButtonSettings();
        DLOGI("BTN", "Button 2 short press: W level -> %d (%d/255)", wBrightLevel, W_BRIGHT_LEVELS[wBrightLevel]);
      }
    }
    btn2State.wasPressed = nowPressed;
  }
}
