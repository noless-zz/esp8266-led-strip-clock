# ESP8266 LED Strip Clock

A minimalist time display using a 60-LED WS2812/SK6812 RGBW strip (1 LED per minute) with NTP synchronization, timezone support, and WiFi configuration via captive portal.

**Origin Project:** This project is based on [esp8266-led-portal](https://github.com/noless-zz/esp8266-led-portal), a generic LED effect controller with 8 programmable effects (solid, rainbow, chase, breathe, fire, twinkle, wave, gradient). This clock project pivots the codebase toward a single-purpose time visualization device, retaining the WiFi + OTA infrastructure but replacing the effect system with time display logic.

---

## Project Scope

### Overview
Display current time as an analog-style visualization on a linear 60-LED strip:
- **60 LEDs** → represent **60 minutes** (LED 0 = minute 0, LED 59 = minute 59)
- **Minute indicator** → single **green LED** at current minute position (0–59)
- **Hour indicator** → three consecutive **red LEDs** centered on the current hour position (wrapping across minutes; e.g., hour 3 = LEDs ~15, 16, 17)
- **Second indicator** → single **blue LED** that pulses/blinks at current second position (0–59)

### Features
- **NTP Time Sync** – fetches time from NTP server on boot and periodic updates
- **Timezone Support** – web UI allows selecting timezone (or UTC offset for manual entry)
- **Manual Time Override** – captive portal web interface for setting time manually if NTP unavailable
- **Captive Portal WiFi** – same flow as origin project; auto-opens on connection to AP
- **OTA Firmware Updates** – upload new `.bin` via web UI (powered down during update to reduce current)
- **RGBW LED Support** – uses all 4 channels (RGB + W) for color flexibility

### Hardware
- **Board:** Wemos D1 Mini (ESP8266)
- **LEDs:** WS2812/SK6812 RGBW strip, **60 LEDs** on pin **D4**
- **Power:** 5V supply (60 LEDs @ full brightness ≈ 3.5A max; reduce clock brightness to ~30% during normal display)

### LED Mapping

| Component | Color | Count | Position |
|-----------|-------|-------|----------|
| Minutes | Green | 1 | LED[current_minute] (0–59) |
| Hours | Red | 3 | LED[hour_position], LED[hour_position+1], LED[hour_position+2] (wrapping) |
| Seconds | Blue | 1 | LED[current_second] (0–59) |

**Example:** If the time is **14:32:45**
- Minute pointer: Green LED at position 32
- Hour pointer: 3 red LEDs around position 17–19 (14:00 ÷ 12 hours × 60 min = 70 min ÷ 60 = ~17)
- Second pointer: Blue LED at position 45

---

## Quick Start

### Build & Flash
```bash
# 1. Via USB connection
pio run -e d1_mini -t upload

# 2. Device starts as WiFi AP "LED-Monitor" (open network)
# 3. Captive portal auto-opens on connected phone/laptop, or visit http://192.168.4.1
# 4. Configure WiFi credentials, timezone, and view live clock
```

### OTA Update
Once on network, upload firmware updates without USB:

**Via Web UI:**
1. Build: `pio run -e d1_mini`
2. Locate: `.pio/build/d1_mini/firmware.bin`
3. Open device web UI → "Firmware Update" section → upload `.bin`

**Via PlatformIO CLI (if DNS mDNS available):**
```bash
pio run -e d1_mini -t upload --upload-port ledclock.local
```

### Web Interface
On the captive portal / main web page:
- **WiFi Settings** – enter SSID, password to join network (for NTP access)
- **Time Zone** – select from dropdown (UTC-12 to UTC+14) or enter manual UTC offset
- **Manual Time** – set hours, minutes, seconds if NTP sync fails
- **Current Status** – shows synced time, next NTP update, WiFi signal strength

---

## Configuration
Edit defines in `platformio.ini` `build_flags`:
| Flag | Default | Description |
|------|---------|-------------|
| `NUM_LEDS` | 60 | Number of LEDs (1 per minute) |
| `LED_PIN` | D4 | WS2812 data pin |
| `HOSTNAME` | LEDClock | mDNS hostname + OTA identifier |

AP name, OTA password, and NTP server are defined in `src/main.cpp` top-level constants.

---

## Wiring
```
D1 Mini        WS2812/SK6812
─────────      ──────────────
D4  ──────────  DIN (data in)
GND ──────────  GND (ground)
(5V PSU)       VCC (power)
```

**Recommended protections:**
- **330Ω resistor** on data line between D4 and LED DIN
- **1000µF capacitor** across LED power (VCC ↔ GND)
- **Separate 5V PSU** for LED strip (do NOT power from D1 Mini USB)

---

## Development Notes

### Time Display Algorithm
1. Fetch current time from NTP (or use manual override)
2. Extract hours (12h wrap), minutes (0–59), seconds (0–59)
3. Compute LED indices:
   - `minute_led = minutes` (direct mapping)
   - `hour_leds = (hours % 12) * 5` (5 LEDs per hour on 60-LED strip) with ±1 offset for 3-LED display
   - `second_led = seconds % 60`
4. Render: clear strip, place green @ minute, red @ hour (×3), blue @ second, show

### Hour Rollover (12-Hour Wrapping)

The hour indicator uses **12-hour format** to fit on the clock:
- **12 AM / 12 PM → hour index 0** (LEDs 0, 1, 2 at top of clock)
- **1 AM / 1 PM → hour index 5** (5 LEDs per hour × 1)
- **6 AM / 6 PM → hour index 30** (bottom of clock)
- **11 AM / 11 PM → hour index 55** (LEDs 55, 56, 57)

**Wrapping behavior:** Hour LEDs use `(hours % 12) * 5` to calculate the starting LED position. The three red LEDs always appear at consecutive indices, wrapping around if necessary:
- Hours 11:50 PM → red LEDs at positions [55, 56, 57]
- Hours 12:05 AM → red LEDs at positions [0, 1, 2] (wrapped from 59→0)

**Boundary case at hour 11:** If a 3-LED hour indicator extends beyond index 59, the overflow wraps to LED 0 using modulo arithmetic:
```
hour_index = 55
hour_leds = [55, 56, 57]  // all within bounds

// If using different math (e.g., 3.5 LEDs per hour):
overflow_index = 62 % 60 = 2  // wraps to LEDs [0, 1, 2]
```

### LED Overlap Handling (Color Blending)

When hours, minutes, or seconds point to overlapping positions, **blend the colors** instead of replacing them:

| Scenario | Overlap Positions | Result |
|----------|-------------------|--------|
| Minute + Hour | Green + Red | **Yellow** (R+G) |
| Minute + Second | Green + Blue | **Cyan** (G+B) |
| Hour + Second | Red + Blue | **Magenta** (R+B) |
| All three | Green + Red + Blue | **White** (R+G+B, full RGBW) |
| Minute ∩ Hour (3-LED) | Green overlaps one of 3 red | **Yellow** on overlap, red elsewhere |

**Implementation approach:**
1. Create an accumulator array for each LED position (RGBA channels, 16-bit per channel for precision)
2. For each indicator (hour, minute, second), **accumulate** (add) its color to overlapping positions
3. Clamp values to max 255 per channel
4. Output final blended color to NeoPixel

**Example:** Time **14:32:45**
- Minute (Green) @ LED 32
- Hour (Red) @ LEDs 20, 21, 22
- Second (Blue) @ LED 45
- **LED 32 displays pure green** (no overlap)
- **LEDs 20–22 display pure red** (no overlap)
- **LED 45 displays pure blue** (no overlap)
- **All other LEDs are off** (black/0x000000)

**Example:** Time **12:05:05** (overlapping minute & second)
- Minute (Green) @ LED 5
- Hour (Red) @ LEDs 0, 1, 2
- Second (Blue) @ LED 5
- **LED 5 displays cyan** (Green + Blue = 0x00FFFF)
- **LEDs 0–2 display red** (0xFF0000)
- **All other LEDs are off**

### Timezone Handling

**Three-tier timezone resolution** (in order of precedence):

1. **Auto-Detect via IP Geolocation** (on first boot or WiFi reconnect)
   - Query public geolocation API (e.g., `ip-api.com/json` or `ipapi.co/json`)
   - Extract timezone string (e.g., `"America/New_York"`) or UTC offset from response
   - Auto-apply and display in web UI
   - Gracefully fall back to UTC if API unreachable

2. **User-Selected Timezone** (via web UI dropdown)
   - Predefined list: UTC-12:00 to UTC+14:00 (or full IANA timezone names)
   - Overrides auto-detection
   - Persisted in EEPROM and applied on every reboot

3. **Manual UTC Offset** (fallback if neither above works)
   - Allow direct offset entry: e.g., "-05:00", "+09:30"
   - Useful if user prefers not to auto-detect or for offline operation
   - Also stored in EEPROM

**Behavior:**
- On first WiFi connection → auto-detect IP timezone and store in EEPROM
- On subsequent boots → apply stored timezone from EEPROM
- User can override auto-detect anytime via web UI
- If timezone lookup fails → default to UTC and prompt user to manually set

**Implementation notes:**
- Use lightweight HTTP client (built into ESP8266) for geolocation request
- Cache result in EEPROM to avoid repeated API calls
- Refresh timezone once per day (or on WiFi reconnect) to account for DST changes
- Display "Timezone: Auto-detected (America/New_York)" vs. "Timezone: Manual (UTC-5)" in web UI

### LED Brightness
- Default: 30% to reduce power draw and heat
- Configurable via web UI brightness slider (0–100%)
- Reduced to 5% during firmware OTA update to minimize current spikes

---

## Status
🚧 **WIP** – Initial architecture and NTP integration in progress
