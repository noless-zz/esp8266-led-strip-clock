# LED Strip Clock - Display Modes Guide

## Available Display Modes

Your ESP8266 LED clock now supports 4 different display modes! Switch between them to customize your clock's appearance.

### 1. **SOLID** (Mode 0) - Classic Overlaid Display
The original display mode that shows time overlapping on the LED strip:
- **Green LED**: Current minute position
- **Red LEDs (3)**: Hour hand (advances 5 LEDs per hour)
- **Blue LED**: Current second position
- **Best for**: Classic analog-style visualization

**API**: `/api/display?mode=0`

---

### 2. **BINARY** (Mode 1) - Binary Clock
Displays time in binary format using LED positions as bits:
- **Row 1 (LEDs 0-4)**: Hours in binary (0-23)
  - Color: Orange/Yellow
- **Row 2 (LEDs 5-7)**: Minutes tens in binary (0-5)
  - Color: Cyan/Light Green
- **Row 3 (LEDs 10-13)**: Minutes ones in binary (0-9)
  - Color: Blue

**Example**: 14:45 (2:45 PM)
- Binary hours: 1110 (14 = 00001110)
- Binary min tens: 100 (4 = 100)
- Binary min ones: 0101 (5 = 0101)

**API**: `/api/display?mode=1`

**Best for**: Tech enthusiasts, makers, coding fans

---

### 3. **SEGMENT** (Mode 2) - Digital Segment Display
Simulates a 7-segment digital clock display:
- Arranges LEDs into 4 digit positions (HH:MM)
- Each digit encoded with variable LED count
- Color-coded by position:
  - **Hour tens**: Orange
  - **Hour ones**: Yellow-Green
  - **Minute tens**: Cyan
  - **Minute ones**: Blue

**API**: `/api/display?mode=2`

**Best for**: Digital clock lovers, modern minimalist look

---

### 4. **CHASE** (Mode 3) - Animated Chase Effect
Dynamic animation with moving LEDs:
- **White LED**: Chasing LED that sweeps across the strip following seconds
  - Moves 0→59 as seconds progress
- **Orange base**: Hour indicator (underlying position)
  - Position 0→59 as hours advance
- **Cyan base**: Minute indicator (underlying position)
  - Position 0→59 as minutes advance

**API**: `/api/display?mode=3`

**Best for**: Visual appeal, eye-catching animation

---

## How to Switch Modes

### Via REST API
```bash
# Switch to Binary mode
curl "http://192.168.0.138/api/display?mode=1"

# Switch to Chase mode
curl "http://192.168.0.138/api/display?mode=3"

# Get current mode
curl "http://192.168.0.138/api/display"
```

### Response Format
```json
{
  "current_mode": 0,
  "available_modes": {
    "SOLID": 0,
    "BINARY": 1,
    "SEGMENT": 2,
    "CHASE": 3
  }
}
```

### Via JavaScript (Settings Page)
Add to your frontend to provide a UI dropdown:
```javascript
async function setDisplayMode(mode) {
  const response = await fetch(`/api/display?mode=${mode}`);
  const data = await response.json();
  console.log(`Mode changed to: ${data.new_mode}`);
}

// Usage
setDisplayMode(1);  // Switch to Binary
setDisplayMode(3);  // Switch to Chase
```

---

## Persistence

Your selected display mode is saved to EEPROM and restored when the device boots. No need to reconfigure after a reboot!

**Storage location**: EEPROM byte 134 (reserved for future use)

---

## Brightness Control

All display modes respect your brightness setting. Adjust brightness via:
```bash
curl "http://192.168.0.138/api/brightness?value=50"  # 50%
```

---

## Technical Details

### Color Channels
- **RGB format** for all modes (GRBWNeoPixel)
- White channel set to 0 (not used)
- Colors automatically scaled by brightness level

### Performance
- Real-time updates (called every loop iteration)
- No blocking operations
- Memory efficient: < 5KB added code per mode

### LED Count
Optimized for 60-LED WS2812B strips. Modes adapt gracefully to fewer LEDs:
- Binary: Uses first 15 LEDs
- Segment: Uses first 27 LEDs (~4-5 per digit)
- Chase: Uses full strip

---

## Tips & Tricks

1. **Binary Too Complex?** 
   - Keep a reference card: Binary is HH (2 groups of 5) + MM (3+4)
   - Great for learning binary!

2. **Chase Mode for Motion Detection**
   - The moving white LED is very eye-catching
   - Great for visibility from across a room

3. **Segment Mode for Guests**
   - Most recognizable format
   - Easy to read at a glance

4. **Mix & Match**
   - Change modes based on time of day (via IFTTT or automation)
   - Morning: Solid, Afternoon: Chase, Evening: Binary

---

## Troubleshooting

**LEDs not showing the right pattern?**
1. Check `/api/status` to confirm time is synced (NTP okay)
2. Verify timezone is correct for your location
3. Test with Solid mode first to confirm basic functionality

**Want more display modes?**
Check [IMPROVEMENTS.md](IMPROVEMENTS.md) for additional planned features!
