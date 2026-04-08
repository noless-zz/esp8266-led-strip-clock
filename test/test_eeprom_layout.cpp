/*
 * test_eeprom_layout.cpp
 *
 * Tests that validate the EEPROM address layout defined in src/config.h and
 * the data-validation logic from src/storage.cpp:
 *
 *   - No address ranges overlap
 *   - All ranges fit inside EEPROM_SIZE
 *   - SSID character-validity check (printable ASCII gate)
 *   - Timezone offset range guard
 *   - Fade-duration range guard
 *
 * No hardware dependencies; runs on the native host via `pio test -e native`.
 */

#include <unity.h>
#include <stdint.h>
#include <string.h>  /* strlen */

/* =========================================================================
 * EEPROM layout constants — from src/config.h
 * ========================================================================= */

static const int EEPROM_SIZE              = 512;
static const int EEPROM_MAGIC_ADDR        = 0;
static const int EEPROM_SSID_ADDR         = 1;
static const int EEPROM_PASS_ADDR         = 65;
static const int EEPROM_BRIGHTNESS_ADDR   = 129;
static const int EEPROM_TZ_OFFSET_ADDR    = 130;   /* 4 bytes */
static const int EEPROM_DISPLAY_MODE_ADDR = 134;
static const int EEPROM_MODE_CFG_MAGIC_ADDR = 135;
static const int EEPROM_RGBW_ADDR         = 136;
static const int EEPROM_REVERSED_ADDR     = 137;
static const int EEPROM_MODE_CFG_BASE_ADDR= 160;
static const int EEPROM_DBG_MAGIC_ADDR    = 279;
static const int EEPROM_DBG_ENABLED_ADDR  = 280;
static const int EEPROM_DBG_IP_ADDR       = 281;   /* 16 bytes */
static const int EEPROM_DBG_PORT_ADDR     = 297;   /* 2 bytes */
static const int EEPROM_FADE_MS_ADDR      = 299;   /* 2 bytes */
static const int EEPROM_AUTO_BRIGHT_ADDR  = 301;   /* 6 bytes */
static const int EEPROM_BTN_MAGIC_ADDR    = 307;
static const int EEPROM_W_BRIGHT_ADDR     = 308;
static const int EEPROM_LEDS_OFF_ADDR     = 309;
static const int MAX_SSID_LEN             = 32;
static const int MAX_PASS_LEN             = 64;
static const int DISPLAY_MAX              = 9;

/* sizeof(ModeDisplayConfig) = 9 colour bytes + 3 width bytes + 1 spectrum = 13 */
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
 * Helper: SSID validation logic — copied verbatim from loadEEPROMSettings()
 * in src/storage.cpp (character-validity loop only).
 * ========================================================================= */

static bool validateSsid(const char *data, int maxLen, char *outBuf) {
    outBuf[0] = '\0';
    int outLen = 0;
    for (int i = 0; i < maxLen; i++) {
        char c = data[i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return false;
        outBuf[outLen++] = c;
    }
    outBuf[outLen] = '\0';
    return (outLen > 0);
}

/* =========================================================================
 * Unity boilerplate
 * ========================================================================= */

void setUp(void)    {}
void tearDown(void) {}

/* =========================================================================
 * EEPROM layout: address ordering
 * ========================================================================= */

void test_magic_byte_is_first(void) {
    TEST_ASSERT_EQUAL_INT(0, EEPROM_MAGIC_ADDR);
}

void test_ssid_starts_after_magic(void) {
    TEST_ASSERT_EQUAL_INT(1, EEPROM_SSID_ADDR);
}

void test_ssid_does_not_overlap_pass(void) {
    TEST_ASSERT_TRUE(EEPROM_SSID_ADDR + MAX_SSID_LEN <= EEPROM_PASS_ADDR);
}

void test_pass_does_not_overlap_brightness(void) {
    TEST_ASSERT_TRUE(EEPROM_PASS_ADDR + MAX_PASS_LEN <= EEPROM_BRIGHTNESS_ADDR);
}

void test_tz_offset_size_is_4_bytes(void) {
    /* TZ offset is stored as a big-endian int32_t */
    TEST_ASSERT_EQUAL_INT(EEPROM_DISPLAY_MODE_ADDR, EEPROM_TZ_OFFSET_ADDR + 4);
}

void test_mode_cfg_size_matches_struct(void) {
    /* ModeDisplayConfig must be exactly 13 bytes (all uint8_t, no padding) */
    TEST_ASSERT_EQUAL_INT(13, (int)sizeof(ModeDisplayConfig));
}

void test_mode_cfg_block_fits_before_dbg(void) {
    /* 9 modes × 13 bytes = 117 bytes starting at 160 → ends at 277 */
    int end = EEPROM_MODE_CFG_BASE_ADDR + DISPLAY_MAX * (int)sizeof(ModeDisplayConfig);
    TEST_ASSERT_EQUAL_INT(277, end);
    TEST_ASSERT_TRUE(end <= EEPROM_DBG_MAGIC_ADDR);
}

void test_dbg_block_sequential_layout(void) {
    /* DBG_ENABLED immediately follows DBG_MAGIC */
    TEST_ASSERT_EQUAL_INT(EEPROM_DBG_MAGIC_ADDR + 1, EEPROM_DBG_ENABLED_ADDR);
    /* IP block (16 bytes) followed immediately by PORT */
    TEST_ASSERT_EQUAL_INT(EEPROM_DBG_IP_ADDR + 16, EEPROM_DBG_PORT_ADDR);
    /* PORT (2 bytes) followed immediately by FADE_MS */
    TEST_ASSERT_EQUAL_INT(EEPROM_DBG_PORT_ADDR + 2, EEPROM_FADE_MS_ADDR);
    /* FADE_MS (2 bytes) followed immediately by AUTO_BRIGHT */
    TEST_ASSERT_EQUAL_INT(EEPROM_FADE_MS_ADDR + 2, EEPROM_AUTO_BRIGHT_ADDR);
    /* AUTO_BRIGHT (6 bytes) followed immediately by BTN_MAGIC */
    TEST_ASSERT_EQUAL_INT(EEPROM_AUTO_BRIGHT_ADDR + 6, EEPROM_BTN_MAGIC_ADDR);
    /* BTN section layout */
    TEST_ASSERT_EQUAL_INT(EEPROM_BTN_MAGIC_ADDR + 1, EEPROM_W_BRIGHT_ADDR);
    TEST_ASSERT_EQUAL_INT(EEPROM_BTN_MAGIC_ADDR + 2, EEPROM_LEDS_OFF_ADDR);
}

void test_all_addresses_within_eeprom_size(void) {
    /* Last byte written is LEDS_OFF_ADDR (1 byte) */
    TEST_ASSERT_TRUE(EEPROM_LEDS_OFF_ADDR + 1 <= EEPROM_SIZE);
    /* Mode config block end */
    int modeEnd = EEPROM_MODE_CFG_BASE_ADDR + DISPLAY_MAX * (int)sizeof(ModeDisplayConfig);
    TEST_ASSERT_TRUE(modeEnd <= EEPROM_SIZE);
}

/* =========================================================================
 * SSID validation tests (from loadEEPROMSettings logic)
 * ========================================================================= */

void test_ssid_valid_simple_name(void) {
    char out[33];
    TEST_ASSERT_TRUE(validateSsid("MyNetwork", 32, out));
    TEST_ASSERT_EQUAL_INT(9, (int)strlen(out));
}

void test_ssid_empty_data_is_invalid(void) {
    char out[33];
    char data[32] = {0};
    TEST_ASSERT_FALSE(validateSsid(data, 32, out));
    TEST_ASSERT_EQUAL_INT(0, (int)strlen(out));
}

void test_ssid_with_spaces_is_valid(void) {
    char out[33];
    TEST_ASSERT_TRUE(validateSsid("Home Network", 32, out));
}

void test_ssid_with_control_char_is_invalid(void) {
    char out[33];
    char data[] = {'v', 'a', 'l', 0x01, 'i', 'd', 0};
    TEST_ASSERT_FALSE(validateSsid(data, 32, out));
}

void test_ssid_with_del_0x7F_is_invalid(void) {
    char out[33];
    char data[] = {'h', 'i', 0x7F, 0};
    TEST_ASSERT_FALSE(validateSsid(data, 32, out));
}

void test_ssid_boundary_chars_0x20_and_0x7E_valid(void) {
    char out[33];
    char data[] = {0x20, 0x7E, 0};   /* space and tilde */
    TEST_ASSERT_TRUE(validateSsid(data, 32, out));
    TEST_ASSERT_EQUAL_INT(2, (int)strlen(out));
}

void test_ssid_extended_ascii_0x80_is_invalid(void) {
    char out[33];
    char data[] = {'A', (char)0x80, 'B', 0};
    TEST_ASSERT_FALSE(validateSsid(data, 32, out));
}

void test_ssid_reads_at_most_max_len_bytes(void) {
    char out[34];
    char data[34];
    /* Fill 33 chars with 'A', no null within maxLen=32 */
    for (int i = 0; i < 33; i++) data[i] = 'A';
    data[33] = '\0';
    TEST_ASSERT_TRUE(validateSsid(data, 32, out));
    TEST_ASSERT_EQUAL_INT(32, (int)strlen(out));  /* capped at maxLen */
}

/* =========================================================================
 * Timezone offset validation (from loadEEPROMSettings)
 * Condition:  tzOff != 0 && tzOff >= -43200 && tzOff <= 43200
 * ========================================================================= */

static bool tzOffsetAccepted(int32_t tzOff) {
    return (tzOff != 0 && tzOff >= -43200 && tzOff <= 43200);
}

void test_tz_offset_zero_rejected(void) {
    TEST_ASSERT_FALSE(tzOffsetAccepted(0));
}

void test_tz_offset_positive_12h_accepted(void) {
    TEST_ASSERT_TRUE(tzOffsetAccepted(43200));
}

void test_tz_offset_negative_12h_accepted(void) {
    TEST_ASSERT_TRUE(tzOffsetAccepted(-43200));
}

void test_tz_offset_just_over_range_rejected(void) {
    TEST_ASSERT_FALSE(tzOffsetAccepted(43201));
    TEST_ASSERT_FALSE(tzOffsetAccepted(-43201));
}

void test_tz_offset_typical_values_accepted(void) {
    TEST_ASSERT_TRUE(tzOffsetAccepted(3600));   /* UTC+1 */
    TEST_ASSERT_TRUE(tzOffsetAccepted(-18000)); /* UTC-5 */
    TEST_ASSERT_TRUE(tzOffsetAccepted(19800));  /* UTC+5:30 */
}

/* =========================================================================
 * Fade-duration validation (from loadEEPROMSettings)
 * Condition:  fms <= 2000  (0 = disabled)
 * ========================================================================= */

static bool fadeMsAccepted(uint16_t fms) {
    return (fms <= 2000);
}

void test_fade_ms_0_accepted_as_disabled(void) {
    TEST_ASSERT_TRUE(fadeMsAccepted(0));
}

void test_fade_ms_2000_accepted(void) {
    TEST_ASSERT_TRUE(fadeMsAccepted(2000));
}

void test_fade_ms_2001_rejected(void) {
    TEST_ASSERT_FALSE(fadeMsAccepted(2001));
}

void test_fade_ms_max_uint16_rejected(void) {
    TEST_ASSERT_FALSE(fadeMsAccepted(65535));
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_magic_byte_is_first);
    RUN_TEST(test_ssid_starts_after_magic);
    RUN_TEST(test_ssid_does_not_overlap_pass);
    RUN_TEST(test_pass_does_not_overlap_brightness);
    RUN_TEST(test_tz_offset_size_is_4_bytes);
    RUN_TEST(test_mode_cfg_size_matches_struct);
    RUN_TEST(test_mode_cfg_block_fits_before_dbg);
    RUN_TEST(test_dbg_block_sequential_layout);
    RUN_TEST(test_all_addresses_within_eeprom_size);

    RUN_TEST(test_ssid_valid_simple_name);
    RUN_TEST(test_ssid_empty_data_is_invalid);
    RUN_TEST(test_ssid_with_spaces_is_valid);
    RUN_TEST(test_ssid_with_control_char_is_invalid);
    RUN_TEST(test_ssid_with_del_0x7F_is_invalid);
    RUN_TEST(test_ssid_boundary_chars_0x20_and_0x7E_valid);
    RUN_TEST(test_ssid_extended_ascii_0x80_is_invalid);
    RUN_TEST(test_ssid_reads_at_most_max_len_bytes);

    RUN_TEST(test_tz_offset_zero_rejected);
    RUN_TEST(test_tz_offset_positive_12h_accepted);
    RUN_TEST(test_tz_offset_negative_12h_accepted);
    RUN_TEST(test_tz_offset_just_over_range_rejected);
    RUN_TEST(test_tz_offset_typical_values_accepted);

    RUN_TEST(test_fade_ms_0_accepted_as_disabled);
    RUN_TEST(test_fade_ms_2000_accepted);
    RUN_TEST(test_fade_ms_2001_rejected);
    RUN_TEST(test_fade_ms_max_uint16_rejected);

    return UNITY_END();
}
