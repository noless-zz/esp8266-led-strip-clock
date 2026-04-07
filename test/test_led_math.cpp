/*
 * test_led_math.cpp
 *
 * Tests for the pure arithmetic helpers that live in src/led.cpp:
 *   addSat    – saturating uint8_t addition
 *   hsvToRgb  – HSV → RGB colour conversion
 *
 * These functions have no hardware dependencies and can be compiled and
 * executed on any host platform via `pio test -e native`.
 */

#include <unity.h>
#include <stdint.h>
#include <math.h>

/* =========================================================================
 * Minimal type re-declaration needed to compile the function under test.
 * ========================================================================= */

struct CRGB { uint8_t r, g, b; };

/* =========================================================================
 * Functions under test — copied verbatim from src/led.cpp so that these
 * tests exercise the real algorithm.  If the production implementation
 * changes, update here and verify the expected values remain correct.
 * ========================================================================= */

static uint8_t addSat(uint8_t base, uint8_t add) {
    uint16_t v = (uint16_t)base + add;
    return (v > 255) ? 255 : (uint8_t)v;
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

/* =========================================================================
 * Unity boilerplate
 * ========================================================================= */

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * addSat tests
 * ========================================================================= */

void test_addSat_simple_addition(void) {
    TEST_ASSERT_EQUAL_UINT8(100, addSat(50, 50));
}

void test_addSat_no_overflow(void) {
    TEST_ASSERT_EQUAL_UINT8(200, addSat(100, 100));
}

void test_addSat_overflow_clamped_to_255(void) {
    TEST_ASSERT_EQUAL_UINT8(255, addSat(200, 100));
}

void test_addSat_at_exact_ceiling(void) {
    TEST_ASSERT_EQUAL_UINT8(255, addSat(128, 127));
}

void test_addSat_255_plus_0(void) {
    TEST_ASSERT_EQUAL_UINT8(255, addSat(255, 0));
}

void test_addSat_0_plus_any(void) {
    TEST_ASSERT_EQUAL_UINT8(42, addSat(0, 42));
}

void test_addSat_255_plus_255(void) {
    TEST_ASSERT_EQUAL_UINT8(255, addSat(255, 255));
}

void test_addSat_1_below_ceiling(void) {
    /* 254 + 1 = 255, no saturation needed */
    TEST_ASSERT_EQUAL_UINT8(255, addSat(254, 1));
}

void test_addSat_just_over_ceiling(void) {
    /* 254 + 2 = 256, must saturate */
    TEST_ASSERT_EQUAL_UINT8(255, addSat(254, 2));
}

/* =========================================================================
 * hsvToRgb tests – primary hue axes at full saturation and value
 * ========================================================================= */

void test_hsvToRgb_red_hue_0(void) {
    /* H=0, S=255, V=255 → pure red */
    uint8_t r, g, b;
    hsvToRgb(0, 255, 255, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(255, r);
    TEST_ASSERT_EQUAL_UINT8(0,   g);
    TEST_ASSERT_EQUAL_UINT8(0,   b);
}

void test_hsvToRgb_yellow_hue_60(void) {
    /* H=60, S=255, V=255 → yellow (R+G) */
    uint8_t r, g, b;
    hsvToRgb(60, 255, 255, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(255, r);
    TEST_ASSERT_EQUAL_UINT8(255, g);
    TEST_ASSERT_EQUAL_UINT8(0,   b);
}

void test_hsvToRgb_green_hue_120(void) {
    /* H=120, S=255, V=255 → pure green */
    uint8_t r, g, b;
    hsvToRgb(120, 255, 255, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0,   r);
    TEST_ASSERT_EQUAL_UINT8(255, g);
    TEST_ASSERT_EQUAL_UINT8(0,   b);
}

void test_hsvToRgb_cyan_hue_180(void) {
    /* H=180, S=255, V=255 → cyan (G+B) */
    uint8_t r, g, b;
    hsvToRgb(180, 255, 255, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0,   r);
    TEST_ASSERT_EQUAL_UINT8(255, g);
    TEST_ASSERT_EQUAL_UINT8(255, b);
}

void test_hsvToRgb_blue_hue_240(void) {
    /* H=240, S=255, V=255 → pure blue */
    uint8_t r, g, b;
    hsvToRgb(240, 255, 255, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0,   r);
    TEST_ASSERT_EQUAL_UINT8(0,   g);
    TEST_ASSERT_EQUAL_UINT8(255, b);
}

void test_hsvToRgb_magenta_hue_300(void) {
    /* H=300, S=255, V=255 → magenta (R+B) */
    uint8_t r, g, b;
    hsvToRgb(300, 255, 255, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(255, r);
    TEST_ASSERT_EQUAL_UINT8(0,   g);
    TEST_ASSERT_EQUAL_UINT8(255, b);
}

/* =========================================================================
 * hsvToRgb tests – boundary and special conditions
 * ========================================================================= */

void test_hsvToRgb_zero_value_is_black(void) {
    /* V=0 must produce black regardless of H and S */
    uint8_t r, g, b;
    hsvToRgb(120, 255, 0, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0, r);
    TEST_ASSERT_EQUAL_UINT8(0, g);
    TEST_ASSERT_EQUAL_UINT8(0, b);
}

void test_hsvToRgb_zero_saturation_is_grey(void) {
    /* S=0 means no colour — all three channels equal V */
    uint8_t r, g, b;
    hsvToRgb(0, 0, 200, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(200, r);
    TEST_ASSERT_EQUAL_UINT8(200, g);
    TEST_ASSERT_EQUAL_UINT8(200, b);
}

void test_hsvToRgb_zero_saturation_mid_hue_still_grey(void) {
    /* S=0 at H=90 should still give grey */
    uint8_t r, g, b;
    hsvToRgb(90, 0, 128, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(128, r);
    TEST_ASSERT_EQUAL_UINT8(128, g);
    TEST_ASSERT_EQUAL_UINT8(128, b);
}

void test_hsvToRgb_hue_360_wraps_to_0(void) {
    /* H=360 ≡ H=0 after modulo */
    uint8_t r1, g1, b1, r2, g2, b2;
    hsvToRgb(0,   255, 200, r1, g1, b1);
    hsvToRgb(360, 255, 200, r2, g2, b2);
    TEST_ASSERT_EQUAL_UINT8(r1, r2);
    TEST_ASSERT_EQUAL_UINT8(g1, g2);
    TEST_ASSERT_EQUAL_UINT8(b1, b2);
}

void test_hsvToRgb_hue_720_wraps_to_0(void) {
    /* H=720 ≡ H=0 */
    uint8_t r1, g1, b1, r2, g2, b2;
    hsvToRgb(0,   255, 180, r1, g1, b1);
    hsvToRgb(720, 255, 180, r2, g2, b2);
    TEST_ASSERT_EQUAL_UINT8(r1, r2);
    TEST_ASSERT_EQUAL_UINT8(g1, g2);
    TEST_ASSERT_EQUAL_UINT8(b1, b2);
}

void test_hsvToRgb_partial_saturation_reduces_chroma(void) {
    /* With S=127 (half), the minimum channel must be > 0 and the two
     * secondary channels (g and b at H=0) must be equal. */
    uint8_t r, g, b;
    hsvToRgb(0, 127, 255, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(255, r);
    TEST_ASSERT_TRUE(g > 0);
    TEST_ASSERT_TRUE(b > 0);
    TEST_ASSERT_EQUAL_UINT8(g, b);
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_addSat_simple_addition);
    RUN_TEST(test_addSat_no_overflow);
    RUN_TEST(test_addSat_overflow_clamped_to_255);
    RUN_TEST(test_addSat_at_exact_ceiling);
    RUN_TEST(test_addSat_255_plus_0);
    RUN_TEST(test_addSat_0_plus_any);
    RUN_TEST(test_addSat_255_plus_255);
    RUN_TEST(test_addSat_1_below_ceiling);
    RUN_TEST(test_addSat_just_over_ceiling);

    RUN_TEST(test_hsvToRgb_red_hue_0);
    RUN_TEST(test_hsvToRgb_yellow_hue_60);
    RUN_TEST(test_hsvToRgb_green_hue_120);
    RUN_TEST(test_hsvToRgb_cyan_hue_180);
    RUN_TEST(test_hsvToRgb_blue_hue_240);
    RUN_TEST(test_hsvToRgb_magenta_hue_300);
    RUN_TEST(test_hsvToRgb_zero_value_is_black);
    RUN_TEST(test_hsvToRgb_zero_saturation_is_grey);
    RUN_TEST(test_hsvToRgb_zero_saturation_mid_hue_still_grey);
    RUN_TEST(test_hsvToRgb_hue_360_wraps_to_0);
    RUN_TEST(test_hsvToRgb_hue_720_wraps_to_0);
    RUN_TEST(test_hsvToRgb_partial_saturation_reduces_chroma);

    return UNITY_END();
}
