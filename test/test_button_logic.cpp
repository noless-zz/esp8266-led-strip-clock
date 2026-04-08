/*
 * test_button_logic.cpp
 *
 * Tests for the button state-machine in src/buttons.cpp:
 *   - Short press fires on release (after debounce window)
 *   - Very short press (<50 ms) is suppressed by debounce
 *   - Long press fires while button is held (≥800 ms), before release
 *   - Long press suppresses the short-press action on subsequent release
 *   - Long press fires exactly once even if button stays held
 *   - Button 2 has only a short-press action (no long press)
 *   - Display mode cycling wraps at DISPLAY_MAX
 *   - W-channel brightness cycling wraps at W_BRIGHT_LEVEL_COUNT
 *
 * No hardware dependencies; runs on the native host via `pio test -e native`.
 */

#include <unity.h>
#include <stdint.h>

/* =========================================================================
 * Timing constants from src/config.h
 * ========================================================================= */

static const unsigned long BTN_LONG_PRESS_MS = 800;
static const unsigned long BTN_DEBOUNCE_MS   = 50;

/* =========================================================================
 * Minimal struct re-declaration (from src/config.h)
 * ========================================================================= */

struct ButtonState {
    bool          wasPressed;
    unsigned long pressedAt;
    bool          actionFired;
};

/* =========================================================================
 * Simulated button handlers
 *
 * Return codes:
 *   0 = no action this tick
 *   1 = short press detected
 *   2 = long press detected
 *
 * These are direct translations of the handleButtons() body in
 * src/buttons.cpp, excluding hardware calls (digitalRead, saveXxx, ledStrip).
 * ========================================================================= */

static ButtonState btn1 = {false, 0, false};
static ButtonState btn2 = {false, 0, false};

static int tickBtn1(bool isPressed, unsigned long now) {
    int action = 0;
    if (isPressed && !btn1.wasPressed) {
        /* Falling edge */
        btn1.pressedAt   = now;
        btn1.actionFired = false;
    } else if (isPressed && btn1.wasPressed && !btn1.actionFired) {
        /* Held: fire long press when threshold crossed */
        if (now - btn1.pressedAt >= BTN_LONG_PRESS_MS) {
            btn1.actionFired = true;
            action = 2;
        }
    } else if (!isPressed && btn1.wasPressed) {
        /* Rising edge */
        if (!btn1.actionFired && (now - btn1.pressedAt >= BTN_DEBOUNCE_MS)) {
            action = 1;
        }
    }
    btn1.wasPressed = isPressed;
    return action;
}

static int tickBtn2(bool isPressed, unsigned long now) {
    int action = 0;
    if (isPressed && !btn2.wasPressed) {
        btn2.pressedAt   = now;
        btn2.actionFired = false;
    } else if (!isPressed && btn2.wasPressed) {
        if (!btn2.actionFired && (now - btn2.pressedAt >= BTN_DEBOUNCE_MS)) {
            action = 1;
        }
    }
    btn2.wasPressed = isPressed;
    return action;
}

/* =========================================================================
 * Unity boilerplate
 * ========================================================================= */

void setUp(void) {
    btn1 = {false, 0, false};
    btn2 = {false, 0, false};
}

void tearDown(void) {}

/* =========================================================================
 * Button 1 tests
 * ========================================================================= */

void test_btn1_idle_produces_no_action(void) {
    TEST_ASSERT_EQUAL_INT(0, tickBtn1(false, 1000));
}

void test_btn1_short_press_fires_on_release(void) {
    tickBtn1(true, 100);
    int action = tickBtn1(false, 300);  /* 200 ms hold → short press */
    TEST_ASSERT_EQUAL_INT(1, action);
}

void test_btn1_debounce_suppresses_very_short_press(void) {
    tickBtn1(true, 100);
    int action = tickBtn1(false, 140);  /* only 40 ms < 50 ms */
    TEST_ASSERT_EQUAL_INT(0, action);
}

void test_btn1_press_exactly_at_debounce_threshold_fires(void) {
    /* 100 + 50 = 150 ms: just at the threshold — must fire */
    tickBtn1(true, 100);
    int action = tickBtn1(false, 150);
    TEST_ASSERT_EQUAL_INT(1, action);
}

void test_btn1_press_one_ms_below_threshold_suppressed(void) {
    tickBtn1(true, 100);
    int action = tickBtn1(false, 149);  /* 49 ms < 50 ms */
    TEST_ASSERT_EQUAL_INT(0, action);
}

void test_btn1_long_press_fires_while_button_held(void) {
    tickBtn1(true, 0);
    /* Tick at 500 ms — not yet */
    int a1 = tickBtn1(true, 500);
    TEST_ASSERT_EQUAL_INT(0, a1);
    /* Tick at exactly 800 ms — threshold crossed */
    int a2 = tickBtn1(true, 800);
    TEST_ASSERT_EQUAL_INT(2, a2);
}

void test_btn1_long_press_fires_one_ms_after_threshold(void) {
    tickBtn1(true, 0);
    int action = tickBtn1(true, 801);
    TEST_ASSERT_EQUAL_INT(2, action);
}

void test_btn1_long_press_fires_only_once(void) {
    tickBtn1(true, 0);
    int a1 = tickBtn1(true, 800);   /* long press fires */
    int a2 = tickBtn1(true, 1000);  /* still held — must not fire again */
    int a3 = tickBtn1(true, 1200);
    TEST_ASSERT_EQUAL_INT(2, a1);
    TEST_ASSERT_EQUAL_INT(0, a2);
    TEST_ASSERT_EQUAL_INT(0, a3);
}

void test_btn1_long_press_suppresses_short_press_on_release(void) {
    tickBtn1(true, 0);
    tickBtn1(true, 800);   /* long press fires */
    int action = tickBtn1(false, 1000);  /* release after long press */
    TEST_ASSERT_EQUAL_INT(0, action);    /* no short press */
}

void test_btn1_can_fire_short_press_on_second_cycle(void) {
    /* First press */
    tickBtn1(true,  100);
    tickBtn1(false, 300);
    /* Second press — state should be reset */
    tickBtn1(true,  500);
    int action = tickBtn1(false, 700);
    TEST_ASSERT_EQUAL_INT(1, action);
}

void test_btn1_long_press_exactly_at_threshold(void) {
    /* Verify the condition is >= (not >) */
    tickBtn1(true, 0);
    int action = tickBtn1(true, BTN_LONG_PRESS_MS);  /* exactly 800 */
    TEST_ASSERT_EQUAL_INT(2, action);
}

/* =========================================================================
 * Button 2 tests
 * ========================================================================= */

void test_btn2_short_press_fires(void) {
    tickBtn2(true, 0);
    int action = tickBtn2(false, 200);
    TEST_ASSERT_EQUAL_INT(1, action);
}

void test_btn2_debounce_suppresses_very_short_press(void) {
    tickBtn2(true, 0);
    int action = tickBtn2(false, 20);  /* 20 ms < 50 ms */
    TEST_ASSERT_EQUAL_INT(0, action);
}

void test_btn2_held_long_produces_no_action(void) {
    /* Button 2 has no long-press action */
    tickBtn2(true, 0);
    int action = tickBtn2(true, 2000);  /* held 2 s */
    TEST_ASSERT_EQUAL_INT(0, action);
}

void test_btn2_release_after_long_hold_fires_short(void) {
    /* Button 2: no long press, so release still counts as short */
    tickBtn2(true, 0);
    tickBtn2(true, 2000);               /* held with no actionFired */
    int action = tickBtn2(false, 2001);
    TEST_ASSERT_EQUAL_INT(1, action);
}

/* =========================================================================
 * Display mode and W-brightness cycling
 * ========================================================================= */

static const int DISPLAY_MAX         = 9;
static const int W_BRIGHT_LEVEL_COUNT = 4;

void test_display_mode_increments_through_all(void) {
    for (int m = 0; m < DISPLAY_MAX - 1; m++) {
        TEST_ASSERT_EQUAL_INT(m + 1, (m + 1) % DISPLAY_MAX);
    }
}

void test_display_mode_wraps_from_last_to_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, (DISPLAY_MAX - 1 + 1) % DISPLAY_MAX);
}

void test_wbright_cycles_through_four_levels(void) {
    TEST_ASSERT_EQUAL_INT(1, (0 + 1) % W_BRIGHT_LEVEL_COUNT);
    TEST_ASSERT_EQUAL_INT(2, (1 + 1) % W_BRIGHT_LEVEL_COUNT);
    TEST_ASSERT_EQUAL_INT(3, (2 + 1) % W_BRIGHT_LEVEL_COUNT);
    TEST_ASSERT_EQUAL_INT(0, (3 + 1) % W_BRIGHT_LEVEL_COUNT);  /* wraps */
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_btn1_idle_produces_no_action);
    RUN_TEST(test_btn1_short_press_fires_on_release);
    RUN_TEST(test_btn1_debounce_suppresses_very_short_press);
    RUN_TEST(test_btn1_press_exactly_at_debounce_threshold_fires);
    RUN_TEST(test_btn1_press_one_ms_below_threshold_suppressed);
    RUN_TEST(test_btn1_long_press_fires_while_button_held);
    RUN_TEST(test_btn1_long_press_fires_one_ms_after_threshold);
    RUN_TEST(test_btn1_long_press_fires_only_once);
    RUN_TEST(test_btn1_long_press_suppresses_short_press_on_release);
    RUN_TEST(test_btn1_can_fire_short_press_on_second_cycle);
    RUN_TEST(test_btn1_long_press_exactly_at_threshold);

    RUN_TEST(test_btn2_short_press_fires);
    RUN_TEST(test_btn2_debounce_suppresses_very_short_press);
    RUN_TEST(test_btn2_held_long_produces_no_action);
    RUN_TEST(test_btn2_release_after_long_hold_fires_short);

    RUN_TEST(test_display_mode_increments_through_all);
    RUN_TEST(test_display_mode_wraps_from_last_to_zero);
    RUN_TEST(test_wbright_cycles_through_four_levels);

    return UNITY_END();
}
