# LED Strip Clock - Display Modes & Persistent HMS Customization

## Current Display Modes

The clock supports 9 display modes (`0..8`):

1. `0` - **Marker Ring** (rainbow orbit + HMS markers)
2. `1` - **Simple HMS** (clean markers only)
3. `2` - **Pulse** (subtle background pulse + HMS)
4. `3` - **Binary Clock** (60 LEDs stretched bits + HMS overlay)
5. `4` - **Hour Beacon** (minute progress + HMS)
6. `5` - **Flame HMS** (optimized warm flicker + HMS)
7. `6` - **Pastel HMS** (soft color scheme)
8. `7` - **Neon HMS** (vivid color scheme)
9. `8` - **Comet Trails** (animated HMS tails)

---

## Persistence

### 1) Selected Display Mode (persistent)
- Selected mode is stored in EEPROM and restored on boot.
- Saving happens when calling:
  - `GET /api/display?mode=<0..8>`

### 2) Per-Mode HMS Visual Config (persistent)
Each mode has its own persisted HMS visual profile in EEPROM.

Per-mode config fields:
- **Hour color**: `hour.r`, `hour.g`, `hour.b` (0..255)
- **Minute color**: `minute.r`, `minute.g`, `minute.b` (0..255)
- **Second color**: `second.r`, `second.g`, `second.b` (0..255)
- **Widths (radius)**:
  - `width.hour` (0..10)
  - `width.minute` (0..10)
  - `width.second` (0..12)
- **Spectrum mode**:
  - `0` = static
  - `1` = rainbow blend
  - `2` = pulse glow

---

## API

## `GET /api/display`
- Returns current mode + available mode IDs.
- If `mode` query parameter is provided, changes mode and persists it.

Example:
```bash
curl "http://192.168.0.138/api/display?mode=5"
```

## `GET /api/display/config`
Reads or writes per-mode HMS visual config.

### Read config
```bash
curl "http://192.168.0.138/api/display/config?mode=5"
```

### Save config
Use `set=1` and pass fields you want to update:
- Colors: `hr hg hb mr mg mb sr sg sb`
- Widths: `hw mw sw`
- Spectrum: `sp`

```bash
curl "http://192.168.0.138/api/display/config?set=1&mode=5&hr=255&hg=80&hb=0&mr=0&mg=255&mb=80&sr=0&sg=120&sb=255&hw=2&mw=1&sw=4&sp=1"
```

Response includes the final stored config for that mode.

---

## Settings Page

`/settings.html` now includes per-mode HMS controls:
- Hour / Minute / Second color pickers
- Hour / Minute / Second width sliders
- Spectrum select (Static / Rainbow blend / Pulse glow)
- Save button: **Save Mode Visuals**

Behavior:
- Changing display mode loads that mode’s stored HMS profile.
- Saving visuals stores only that selected mode profile.

---

## Definitions

- **HMS marker**: Hour / Minute / Second indicator drawn on the ring.
- **Width**: Marker radius around its center position.
  - Width `0` = center LED only.
  - Width `1` = 3 LEDs (`-1..+1`), etc.
- **Spectrum**: Dynamic color modulation on top of base color.

---

## Notes

- Brightness still applies globally via `/api/brightness`.
- Flame mode remains optimized to reduce update load.
- If EEPROM config is missing or invalid, defaults are auto-generated and saved.
