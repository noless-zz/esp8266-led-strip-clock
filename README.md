# ESP8266 WS2812 LED Controller

Captive portal WiFi hotspot for controlling a 65-LED WS2812 strip, with OTA firmware updates.

## Hardware
- **Board:** Wemos D1 Mini (ESP8266)
- **LEDs:** WS2812 chain × 5 on pin **D4**
- **Power:** Supply 5V to LEDs separately (65 LEDs ≈ 3.5A max)

## Features
- **Captive Portal** – auto-opens on any device connecting to the AP
- **8 LED Effects** – Solid, Rainbow, Chase, Breathe, Fire, Twinkle, Wave, Gradient
- **Dual Color Picker** – Primary + Secondary colors for blending effects
- **Brightness & Speed** – Real-time sliders
- **OTA Update** – Upload `.bin` via web UI or PlatformIO OTA
- **Progress LEDs** – LED strip shows upload progress during firmware updates

## Quick Start

```bash
# 1. Build & flash via USB
pio run -t upload

# 2. Connect to WiFi "LED-Controller" (open network)
# 3. Captive portal opens automatically, or go to http://192.168.4.1
```

## OTA Update

### Via Web UI
1. Build firmware: `pio run`
2. Find `.pio/build/d1_mini/firmware.bin`
3. Upload through the web interface "Firmware Update" card

### Via PlatformIO CLI
Uncomment the OTA lines in `platformio.ini`, then:
```bash
pio run -t upload --upload-port 192.168.4.1
```

## Configuration
Edit defines in `platformio.ini` build_flags:
| Flag | Default | Description |
|------|---------|-------------|
| `NUM_LEDS` | 5 | Number of LEDs |
| `LED_PIN` | D4 | Data pin |
| `HOSTNAME` | LedController | mDNS / OTA name |

AP name & OTA password are in `src/main.cpp` top constants.

## Wiring
```
D1 Mini        WS2812
─────────      ──────
D4  ──────────  DIN
GND ──────────  GND
(5V from separate PSU to LED VCC & GND)
```
> Add a 330Ω resistor on the data line and a 1000µF capacitor across LED power.
