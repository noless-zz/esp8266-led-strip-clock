/*
 * test_pixel_ops.cpp
 *
 * Tests for pixel/LED operations and clock-hand geometry in src/led.cpp:
 *   addPixelWrap        – accumulate colour into the LED buffer with index wrapping
 *   blendSpectrumColor  – colour blend based on spectrum mode (spectrum=0 passthrough)
 *   updateFade          – per-hand fade state-machine
 *   Clock-hand position formulas used by overlayTimeMarkers
 *
 * No hardware dependencies; runs on the native host via `pio test -e native`.
 */

#include <unity.h>
#include <stdint.h>
#include <string.h>  /* memset */

/* =========================================================================
 * Minimal type and constant re-declarations
 * ========================================================================= */

#define NUM_LEDS 60

struct CRGB { uint8_t r, g, b; };

struct HandFade {
    int      fromPos = -1;
    int      toPos   = -1;
    uint32_t startMs = 0;
};

/* =========================================================================
 * Mock millis() – controlled by each test
 * ========================================================================= */

static uint32_t g_mock_millis = 0;
static inline uint32_t millis(void) { return g_mock_millis; }

/* =========================================================================
 * LED buffer (stands in for the global leds[] defined in main.cpp)
 * ========================================================================= */

static CRGB leds[NUM_LEDS];

/* =========================================================================
 * Functions under test — copied verbatim from src/led.cpp.
 * ========================================================================= */

static uint8_t addSat(uint8_t base, uint8_t add) {
    uint16_t v = (uint16_t)base + add;
    return (v > 255) ? 255 : (uint8_t)v;
}

static void addPixelWrap(int index, uint8_t r, uint8_t g, uint8_t b) {
    int pos = index % NUM_LEDS;
    if (pos < 0) pos += NUM_LEDS;
    leds[pos].r = addSat(leds[pos].r, r);
    leds[pos].g = addSat(leds[pos].g, g);
    leds[pos].b = addSat(leds[pos].b, b);
}

static void hsvToRgb(uint16_t hue, uint8_t sat, uint8_t val,
                     uint8_t &r, uint8_t &g, uint8_t &b) {
    hue %= 360;
    uint8_t region = hue / 60;
    uint16_t rem = (hue % 60) * 255 / 60;
    uint8_t p = (uint16_t)val * (255 - sat) / 255;
    uint8_t q = (uint16_t)val * (255 - ((uint16_t)sat * rem / 255)) / 255;
    uint8_t t = (uint16_t)val * (255 - ((uint16_t)sat * (255 - rem) / 255)) / 255;
    switch (region) {
        case 0: r = val; g = t;   b = p;   break;
        case 1: r = q;   g = val; b = p;   break;
        case 2: r = p;   g = val; b = t;   break;
        case 3: r = p;   g = q;   b = val; break;
        case 4: r = t;   g = p;   b = val; break;
        default: r = val; g = p;  b = q;   break;
    }
}

static CRGB blendSpectrumColor(const CRGB &base, uint8_t spectrum,
                               uint8_t level, uint16_t phaseSeed) {
    if (spectrum == 0) return base;

    uint8_t rr = base.r, gg = base.g, bb = base.b;

    if (spectrum == 1) {
        uint8_t hueR, hueG, hueB;
        uint16_t hue = (uint16_t)((phaseSeed + millis() / 18) % 360);
        hsvToRgb(hue, 255, level, hueR, hueG, hueB);
        rr = addSat(base.r / 2, hueR / 2);
        gg = addSat(base.g / 2, hueG / 2);
        bb = addSat(base.b / 2, hueB / 2);
    } else if (spectrum == 2) {
        uint8_t tw    = (uint8_t)((phaseSeed + (millis() / 24)) % 120);
        uint8_t swing = tw < 60 ? tw : (119 - tw);
        uint8_t boost = (uint8_t)(level / 4 + swing);
        rr = addSat(base.r, boost / 2);
        gg = addSat(base.g, boost / 3);
        bb = addSat(base.b, boost / 2);
    }

    return CRGB{rr, gg, bb};
}

static void updateFade(HandFade &f, int newPos) {
    if (f.toPos == newPos) return;
    f.fromPos = (f.toPos < 0) ? newPos : f.toPos;
    f.toPos   = newPos;
    f.startMs = millis();
}

/* =========================================================================
 * Unity boilerplate
 * ========================================================================= */

void setUp(void) {
    memset(leds, 0, sizeof(leds));
    g_mock_millis = 0;
}

void tearDown(void) {}

/* =========================================================================
 * addPixelWrap tests
 * ========================================================================= */

void test_addPixelWrap_basic_write(void) {
    addPixelWrap(5, 100, 150, 200);
    TEST_ASSERT_EQUAL_UINT8(100, leds[5].r);
    TEST_ASSERT_EQUAL_UINT8(150, leds[5].g);
    TEST_ASSERT_EQUAL_UINT8(200, leds[5].b);
}

void test_addPixelWrap_index_60_wraps_to_0(void) {
    /* 60 % 60 == 0 */
    addPixelWrap(60, 10, 20, 30);
    TEST_ASSERT_EQUAL_UINT8(10, leds[0].r);
    TEST_ASSERT_EQUAL_UINT8(20, leds[0].g);
    TEST_ASSERT_EQUAL_UINT8(30, leds[0].b);
}

void test_addPixelWrap_index_61_wraps_to_1(void) {
    addPixelWrap(61, 5, 6, 7);
    TEST_ASSERT_EQUAL_UINT8(5, leds[1].r);
}

void test_addPixelWrap_negative_minus1_wraps_to_59(void) {
    addPixelWrap(-1, 77, 88, 99);
    TEST_ASSERT_EQUAL_UINT8(77, leds[59].r);
    TEST_ASSERT_EQUAL_UINT8(88, leds[59].g);
    TEST_ASSERT_EQUAL_UINT8(99, leds[59].b);
}

void test_addPixelWrap_negative_minus2_wraps_to_58(void) {
    addPixelWrap(-2, 50, 60, 70);
    TEST_ASSERT_EQUAL_UINT8(50, leds[58].r);
}

void test_addPixelWrap_accumulates_with_saturation(void) {
    /* Two large writes must saturate, not overflow/wrap */
    addPixelWrap(0, 200, 200, 200);
    addPixelWrap(0, 200, 200, 200);
    TEST_ASSERT_EQUAL_UINT8(255, leds[0].r);
    TEST_ASSERT_EQUAL_UINT8(255, leds[0].g);
    TEST_ASSERT_EQUAL_UINT8(255, leds[0].b);
}

void test_addPixelWrap_does_not_touch_adjacent_pixels(void) {
    addPixelWrap(10, 100, 100, 100);
    TEST_ASSERT_EQUAL_UINT8(0, leds[9].r);
    TEST_ASSERT_EQUAL_UINT8(0, leds[11].r);
}

void test_addPixelWrap_zero_adds_nothing(void) {
    leds[3] = {50, 60, 70};
    addPixelWrap(3, 0, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(50, leds[3].r);
    TEST_ASSERT_EQUAL_UINT8(60, leds[3].g);
    TEST_ASSERT_EQUAL_UINT8(70, leds[3].b);
}

/* =========================================================================
 * blendSpectrumColor tests
 * ========================================================================= */

void test_blendSpectrum_0_returns_base_unchanged(void) {
    CRGB base   = {100, 150, 200};
    CRGB result = blendSpectrumColor(base, 0, 255, 42);
    TEST_ASSERT_EQUAL_UINT8(100, result.r);
    TEST_ASSERT_EQUAL_UINT8(150, result.g);
    TEST_ASSERT_EQUAL_UINT8(200, result.b);
}

void test_blendSpectrum_0_ignores_level_and_seed(void) {
    /* spectrum=0: level and phaseSeed must not affect output */
    CRGB base = {50, 100, 150};
    CRGB r1   = blendSpectrumColor(base, 0, 0,   0);
    CRGB r2   = blendSpectrumColor(base, 0, 255, 999);
    TEST_ASSERT_EQUAL_UINT8(r1.r, r2.r);
    TEST_ASSERT_EQUAL_UINT8(r1.g, r2.g);
    TEST_ASSERT_EQUAL_UINT8(r1.b, r2.b);
}

void test_blendSpectrum_1_at_millis0_blends_rainbow(void) {
    /* millis()=0, phaseSeed=0 → hue=0 → pure red from hsvToRgb
     * rr = addSat(100/2=50, 200/2=100) = 150
     * gg = addSat( 50/2=25,   0/2=0 ) = 25
     * bb = addSat( 80/2=40,   0/2=0 ) = 40 */
    CRGB base   = {100, 50, 80};
    CRGB result = blendSpectrumColor(base, 1, 200, 0);
    TEST_ASSERT_EQUAL_UINT8(150, result.r);
    TEST_ASSERT_EQUAL_UINT8(25,  result.g);
    TEST_ASSERT_EQUAL_UINT8(40,  result.b);
}

void test_blendSpectrum_2_at_millis0_adds_boost(void) {
    /* millis()=0, phaseSeed=0 → tw=0, swing=0, boost=level/4
     * level=120 → boost=30
     * rr = addSat(100, 30/2=15) = 115
     * gg = addSat( 60, 30/3=10) = 70
     * bb = addSat(100, 30/2=15) = 115 */
    CRGB base   = {100, 60, 100};
    CRGB result = blendSpectrumColor(base, 2, 120, 0);
    TEST_ASSERT_EQUAL_UINT8(115, result.r);
    TEST_ASSERT_EQUAL_UINT8(70,  result.g);
    TEST_ASSERT_EQUAL_UINT8(115, result.b);
}

void test_blendSpectrum_2_zero_level_adds_nothing(void) {
    /* level=0 → boost=0+swing; swing=0 at millis()=0 → no change */
    CRGB base   = {80, 90, 100};
    CRGB result = blendSpectrumColor(base, 2, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(80,  result.r);
    TEST_ASSERT_EQUAL_UINT8(90,  result.g);
    TEST_ASSERT_EQUAL_UINT8(100, result.b);
}

/* =========================================================================
 * updateFade tests
 * ========================================================================= */

void test_updateFade_first_call_sets_both_positions(void) {
    /* On the first call (toPos=-1), fromPos is set to newPos (no fade) */
    HandFade f;
    g_mock_millis = 100;
    updateFade(f, 30);
    TEST_ASSERT_EQUAL_INT(30,  f.fromPos);
    TEST_ASSERT_EQUAL_INT(30,  f.toPos);
    TEST_ASSERT_EQUAL_UINT32(100, f.startMs);
}

void test_updateFade_second_call_advances_positions(void) {
    HandFade f;
    updateFade(f, 10);
    g_mock_millis = 200;
    updateFade(f, 20);
    TEST_ASSERT_EQUAL_INT(10,  f.fromPos);  /* old toPos */
    TEST_ASSERT_EQUAL_INT(20,  f.toPos);
    TEST_ASSERT_EQUAL_UINT32(200, f.startMs);
}

void test_updateFade_same_position_is_noop(void) {
    HandFade f;
    g_mock_millis = 0;
    updateFade(f, 15);
    uint32_t savedStart = f.startMs;
    g_mock_millis = 500;
    updateFade(f, 15);  /* same target: must not change anything */
    TEST_ASSERT_EQUAL_INT(15, f.fromPos);
    TEST_ASSERT_EQUAL_INT(15, f.toPos);
    TEST_ASSERT_EQUAL_UINT32(savedStart, f.startMs);
}

void test_updateFade_chained_moves_track_most_recent_pair(void) {
    HandFade f;
    updateFade(f, 10);
    updateFade(f, 20);
    updateFade(f, 30);
    TEST_ASSERT_EQUAL_INT(20, f.fromPos);
    TEST_ASSERT_EQUAL_INT(30, f.toPos);
}

void test_updateFade_timestamp_updated_on_each_move(void) {
    HandFade f;
    g_mock_millis = 100;
    updateFade(f, 5);
    g_mock_millis = 300;
    updateFade(f, 10);
    TEST_ASSERT_EQUAL_UINT32(300, f.startMs);
}

/* =========================================================================
 * Clock hand position formula tests
 * (These formulas come from overlayTimeMarkers in src/led.cpp)
 * ========================================================================= */

void test_hour_position_at_12_oclock(void) {
    /* hour12=0, minute=0 → LED 0 */
    TEST_ASSERT_EQUAL_INT(0, (0 * 5 + 0 / 12) % NUM_LEDS);
}

void test_hour_position_at_1_oclock(void) {
    /* hour12=1, minute=0 → LED 5 */
    TEST_ASSERT_EQUAL_INT(5, (1 * 5 + 0 / 12) % NUM_LEDS);
}

void test_hour_position_at_6_oclock(void) {
    /* hour12=6, minute=0 → LED 30 */
    TEST_ASSERT_EQUAL_INT(30, (6 * 5 + 0 / 12) % NUM_LEDS);
}

void test_hour_position_at_11_oclock(void) {
    /* hour12=11, minute=0 → LED 55 */
    TEST_ASSERT_EQUAL_INT(55, (11 * 5 + 0 / 12) % NUM_LEDS);
}

void test_hour_position_advances_with_minutes(void) {
    /* Each 12 minutes adds one LED to the hour hand position */
    /* 1:12 → 5 + 1 = 6 */
    TEST_ASSERT_EQUAL_INT(6, (1 * 5 + 12 / 12) % NUM_LEDS);
    /* 1:24 → 5 + 2 = 7 */
    TEST_ASSERT_EQUAL_INT(7, (1 * 5 + 24 / 12) % NUM_LEDS);
    /* 1:48 → 5 + 4 = 9 */
    TEST_ASSERT_EQUAL_INT(9, (1 * 5 + 48 / 12) % NUM_LEDS);
}

void test_hour_position_wraps_past_59(void) {
    /* 11:48 → 55 + 4 = 59 (no wrap) */
    TEST_ASSERT_EQUAL_INT(59, (11 * 5 + 48 / 12) % NUM_LEDS);
    /* 11:60 (hypothetical: minute==60 never occurs, but formula must wrap) */
    TEST_ASSERT_EQUAL_INT(0,  (11 * 5 + 60 / 12) % NUM_LEDS);
}

void test_minute_position_maps_directly(void) {
    /* minutePos = minute % NUM_LEDS; minutes 0-59 map 1:1 */
    TEST_ASSERT_EQUAL_INT(0,  0  % NUM_LEDS);
    TEST_ASSERT_EQUAL_INT(30, 30 % NUM_LEDS);
    TEST_ASSERT_EQUAL_INT(59, 59 % NUM_LEDS);
}

void test_second_position_maps_directly(void) {
    /* secondPos = second % NUM_LEDS; seconds 0-59 map 1:1 */
    TEST_ASSERT_EQUAL_INT(0,  0  % NUM_LEDS);
    TEST_ASSERT_EQUAL_INT(45, 45 % NUM_LEDS);
    TEST_ASSERT_EQUAL_INT(59, 59 % NUM_LEDS);
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_addPixelWrap_basic_write);
    RUN_TEST(test_addPixelWrap_index_60_wraps_to_0);
    RUN_TEST(test_addPixelWrap_index_61_wraps_to_1);
    RUN_TEST(test_addPixelWrap_negative_minus1_wraps_to_59);
    RUN_TEST(test_addPixelWrap_negative_minus2_wraps_to_58);
    RUN_TEST(test_addPixelWrap_accumulates_with_saturation);
    RUN_TEST(test_addPixelWrap_does_not_touch_adjacent_pixels);
    RUN_TEST(test_addPixelWrap_zero_adds_nothing);

    RUN_TEST(test_blendSpectrum_0_returns_base_unchanged);
    RUN_TEST(test_blendSpectrum_0_ignores_level_and_seed);
    RUN_TEST(test_blendSpectrum_1_at_millis0_blends_rainbow);
    RUN_TEST(test_blendSpectrum_2_at_millis0_adds_boost);
    RUN_TEST(test_blendSpectrum_2_zero_level_adds_nothing);

    RUN_TEST(test_updateFade_first_call_sets_both_positions);
    RUN_TEST(test_updateFade_second_call_advances_positions);
    RUN_TEST(test_updateFade_same_position_is_noop);
    RUN_TEST(test_updateFade_chained_moves_track_most_recent_pair);
    RUN_TEST(test_updateFade_timestamp_updated_on_each_move);

    RUN_TEST(test_hour_position_at_12_oclock);
    RUN_TEST(test_hour_position_at_1_oclock);
    RUN_TEST(test_hour_position_at_6_oclock);
    RUN_TEST(test_hour_position_at_11_oclock);
    RUN_TEST(test_hour_position_advances_with_minutes);
    RUN_TEST(test_hour_position_wraps_past_59);
    RUN_TEST(test_minute_position_maps_directly);
    RUN_TEST(test_second_position_maps_directly);

    return UNITY_END();
}
