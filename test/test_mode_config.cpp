/*
 * test_mode_config.cpp
 *
 * Tests for display-mode configuration helpers in src/led.cpp:
 *   isModeConfigValid    – validates a ModeDisplayConfig struct
 *   defaultModeConfigFor – returns the built-in defaults for each display mode
 *
 * No hardware dependencies; runs on the native host via `pio test -e native`.
 */

#include <unity.h>
#include <stdint.h>

/* =========================================================================
 * Minimal type and constant re-declarations (from src/config.h)
 * ========================================================================= */

enum DisplayMode {
    DISPLAY_SOLID       = 0,
    DISPLAY_SIMPLE      = 1,
    DISPLAY_PULSE       = 2,
    DISPLAY_BINARY      = 3,
    DISPLAY_HOUR_MARKER = 4,
    DISPLAY_FLAME       = 5,
    DISPLAY_PASTEL      = 6,
    DISPLAY_NEON        = 7,
    DISPLAY_COMET       = 8,
    DISPLAY_MAX         = 9
};

struct ModeDisplayConfig {
    uint8_t hourR,   hourG,   hourB;
    uint8_t minuteR, minuteG, minuteB;
    uint8_t secondR, secondG, secondB;
    uint8_t hourWidth;
    uint8_t minuteWidth;
    uint8_t secondWidth;
    uint8_t spectrum;
};

/* =========================================================================
 * Functions under test — copied verbatim from src/led.cpp.
 * ========================================================================= */

static bool isModeConfigValid(const ModeDisplayConfig &cfg) {
    if (cfg.hourWidth   < 1 || cfg.hourWidth   > 21) return false;
    if (cfg.minuteWidth < 1 || cfg.minuteWidth > 21) return false;
    if (cfg.secondWidth < 1 || cfg.secondWidth > 30) return false;
    if (cfg.spectrum > 2)                            return false;
    return true;
}

static ModeDisplayConfig defaultModeConfigFor(uint8_t mode) {
    ModeDisplayConfig cfg = {
        255, 0,   0,
        0,   255, 0,
        0,   0,   255,
        5,
        3,
        3,
        0
    };

    if (mode == DISPLAY_SIMPLE) {
        cfg.hourWidth   = 3;
        cfg.minuteWidth = 3;
        cfg.secondWidth = 3;
    } else if (mode == DISPLAY_COMET) {
        cfg.hourWidth   = 7;
        cfg.minuteWidth = 5;
        cfg.secondWidth = 10;
        cfg.spectrum    = 1;
    } else if (mode == DISPLAY_PASTEL) {
        cfg = {190, 95, 150, 110, 210, 170, 110, 170, 220, 3, 3, 3, 0};
    } else if (mode == DISPLAY_NEON) {
        cfg = {255, 0, 220, 0, 255, 220, 255, 255, 0, 3, 3, 3, 2};
    }

    return cfg;
}

/* =========================================================================
 * Unity boilerplate
 * ========================================================================= */

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * isModeConfigValid tests
 * ========================================================================= */

void test_valid_default_configs(void) {
    /* Every built-in default must pass validation */
    for (int m = 0; m < DISPLAY_MAX; m++) {
        ModeDisplayConfig cfg = defaultModeConfigFor((uint8_t)m);
        TEST_ASSERT_TRUE_MESSAGE(isModeConfigValid(cfg),
            "Built-in default config must always be valid");
    }
}

void test_hour_width_zero_is_invalid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.hourWidth = 0;
    TEST_ASSERT_FALSE(isModeConfigValid(cfg));
}

void test_hour_width_1_is_valid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.hourWidth = 1;
    TEST_ASSERT_TRUE(isModeConfigValid(cfg));
}

void test_hour_width_21_is_valid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.hourWidth = 21;
    TEST_ASSERT_TRUE(isModeConfigValid(cfg));
}

void test_hour_width_22_is_invalid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.hourWidth = 22;
    TEST_ASSERT_FALSE(isModeConfigValid(cfg));
}

void test_minute_width_zero_is_invalid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.minuteWidth = 0;
    TEST_ASSERT_FALSE(isModeConfigValid(cfg));
}

void test_minute_width_21_is_valid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.minuteWidth = 21;
    TEST_ASSERT_TRUE(isModeConfigValid(cfg));
}

void test_minute_width_22_is_invalid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.minuteWidth = 22;
    TEST_ASSERT_FALSE(isModeConfigValid(cfg));
}

void test_second_width_zero_is_invalid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.secondWidth = 0;
    TEST_ASSERT_FALSE(isModeConfigValid(cfg));
}

void test_second_width_30_is_valid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.secondWidth = 30;
    TEST_ASSERT_TRUE(isModeConfigValid(cfg));
}

void test_second_width_31_is_invalid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.secondWidth = 31;
    TEST_ASSERT_FALSE(isModeConfigValid(cfg));
}

void test_spectrum_2_is_valid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.spectrum = 2;
    TEST_ASSERT_TRUE(isModeConfigValid(cfg));
}

void test_spectrum_3_is_invalid(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    cfg.spectrum = 3;
    TEST_ASSERT_FALSE(isModeConfigValid(cfg));
}

/* =========================================================================
 * defaultModeConfigFor tests — verify expected colours and widths
 * ========================================================================= */

void test_solid_default_colours(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    /* hour = red */
    TEST_ASSERT_EQUAL_UINT8(255, cfg.hourR);
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.hourG);
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.hourB);
    /* minute = green */
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.minuteR);
    TEST_ASSERT_EQUAL_UINT8(255, cfg.minuteG);
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.minuteB);
    /* second = blue */
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.secondR);
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.secondG);
    TEST_ASSERT_EQUAL_UINT8(255, cfg.secondB);
}

void test_solid_default_widths(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SOLID);
    TEST_ASSERT_EQUAL_UINT8(5, cfg.hourWidth);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.minuteWidth);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.secondWidth);
    TEST_ASSERT_EQUAL_UINT8(0, cfg.spectrum);
}

void test_simple_default_widths(void) {
    /* Simple mode narrows all hands to 3 */
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SIMPLE);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.hourWidth);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.minuteWidth);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.secondWidth);
}

void test_simple_inherits_base_colours(void) {
    /* Simple only overrides widths, not colours */
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_SIMPLE);
    TEST_ASSERT_EQUAL_UINT8(255, cfg.hourR);
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.hourG);
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.hourB);
}

void test_comet_default_widths(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_COMET);
    TEST_ASSERT_EQUAL_UINT8(7,  cfg.hourWidth);
    TEST_ASSERT_EQUAL_UINT8(5,  cfg.minuteWidth);
    TEST_ASSERT_EQUAL_UINT8(10, cfg.secondWidth);
}

void test_comet_uses_rainbow_spectrum(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_COMET);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.spectrum);
}

void test_pastel_default_colours(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_PASTEL);
    TEST_ASSERT_EQUAL_UINT8(190, cfg.hourR);
    TEST_ASSERT_EQUAL_UINT8(95,  cfg.hourG);
    TEST_ASSERT_EQUAL_UINT8(150, cfg.hourB);
    TEST_ASSERT_EQUAL_UINT8(110, cfg.minuteR);
    TEST_ASSERT_EQUAL_UINT8(210, cfg.minuteG);
    TEST_ASSERT_EQUAL_UINT8(170, cfg.minuteB);
    TEST_ASSERT_EQUAL_UINT8(110, cfg.secondR);
    TEST_ASSERT_EQUAL_UINT8(170, cfg.secondG);
    TEST_ASSERT_EQUAL_UINT8(220, cfg.secondB);
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.spectrum);
}

void test_neon_default_colours(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_NEON);
    TEST_ASSERT_EQUAL_UINT8(255, cfg.hourR);
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.hourG);
    TEST_ASSERT_EQUAL_UINT8(220, cfg.hourB);
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.minuteR);
    TEST_ASSERT_EQUAL_UINT8(255, cfg.minuteG);
    TEST_ASSERT_EQUAL_UINT8(220, cfg.minuteB);
    TEST_ASSERT_EQUAL_UINT8(255, cfg.secondR);
    TEST_ASSERT_EQUAL_UINT8(255, cfg.secondG);
    TEST_ASSERT_EQUAL_UINT8(0,   cfg.secondB);
}

void test_neon_uses_twinkle_spectrum(void) {
    ModeDisplayConfig cfg = defaultModeConfigFor(DISPLAY_NEON);
    TEST_ASSERT_EQUAL_UINT8(2, cfg.spectrum);
}

void test_modes_without_special_case_use_base_defaults(void) {
    /* PULSE, BINARY, HOUR_MARKER, FLAME fall through to the base defaults */
    const uint8_t fallthrough_modes[] = {
        DISPLAY_PULSE, DISPLAY_BINARY, DISPLAY_HOUR_MARKER, DISPLAY_FLAME
    };
    for (int i = 0; i < 4; i++) {
        ModeDisplayConfig cfg = defaultModeConfigFor(fallthrough_modes[i]);
        TEST_ASSERT_EQUAL_UINT8(5, cfg.hourWidth);
        TEST_ASSERT_EQUAL_UINT8(3, cfg.minuteWidth);
        TEST_ASSERT_EQUAL_UINT8(3, cfg.secondWidth);
        TEST_ASSERT_EQUAL_UINT8(0, cfg.spectrum);
    }
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_valid_default_configs);
    RUN_TEST(test_hour_width_zero_is_invalid);
    RUN_TEST(test_hour_width_1_is_valid);
    RUN_TEST(test_hour_width_21_is_valid);
    RUN_TEST(test_hour_width_22_is_invalid);
    RUN_TEST(test_minute_width_zero_is_invalid);
    RUN_TEST(test_minute_width_21_is_valid);
    RUN_TEST(test_minute_width_22_is_invalid);
    RUN_TEST(test_second_width_zero_is_invalid);
    RUN_TEST(test_second_width_30_is_valid);
    RUN_TEST(test_second_width_31_is_invalid);
    RUN_TEST(test_spectrum_2_is_valid);
    RUN_TEST(test_spectrum_3_is_invalid);

    RUN_TEST(test_solid_default_colours);
    RUN_TEST(test_solid_default_widths);
    RUN_TEST(test_simple_default_widths);
    RUN_TEST(test_simple_inherits_base_colours);
    RUN_TEST(test_comet_default_widths);
    RUN_TEST(test_comet_uses_rainbow_spectrum);
    RUN_TEST(test_pastel_default_colours);
    RUN_TEST(test_neon_default_colours);
    RUN_TEST(test_neon_uses_twinkle_spectrum);
    RUN_TEST(test_modes_without_special_case_use_base_defaults);

    return UNITY_END();
}
