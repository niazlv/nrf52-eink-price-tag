#include "test.h"
#include "eink_lut.h"

static uint8_t fake_working[EINK_LUT_SIZE];
static uint8_t fake_default[EINK_LUT_SIZE];

static void test_unbound_fallbacks(void)
{
    /* Before bind_base_tables(): CLEAN and use_custom must fall back to the
     * built-in balanced table instead of dereferencing NULL. */
    eink_lut_set_use_custom(false);
    eink_lut_set_mode(EINK_LUT_MODE_CLEAN);
    const uint8_t *lut = eink_lut_select_partial();
    T_ASSERT(lut != NULL);
    T_ASSERT_EQ(lut[0], 0xAA); /* lut_balanced signature byte */

    eink_lut_set_use_custom(true);
    lut = eink_lut_select_partial();
    T_ASSERT(lut != NULL);
    T_ASSERT_EQ(lut[0], 0xAA);
    eink_lut_set_use_custom(false);
}

static void test_mode_selection(void)
{
    eink_lut_set_mode(EINK_LUT_MODE_TURBO);
    T_ASSERT_EQ(eink_lut_get_mode(), EINK_LUT_MODE_TURBO);
    const uint8_t *turbo = eink_lut_select_partial();
    T_ASSERT_EQ(turbo[0], 0x55);  /* LUT0 black: VSH1 */
    T_ASSERT_EQ(turbo[7], 0xAA);  /* LUT1 white: VSL */
    T_ASSERT_EQ(turbo[35], 0x07); /* Ph0 TA */

    eink_lut_set_mode(EINK_LUT_MODE_BALANCED);
    T_ASSERT_EQ(eink_lut_select_partial()[0], 0xAA);

    eink_lut_set_mode(EINK_LUT_MODE_STABLE);
    T_ASSERT_EQ(eink_lut_select_partial()[0], 0x22);

    eink_lut_set_mode(EINK_LUT_MODE_TONE_DARK);
    const uint8_t *dark = eink_lut_select_partial();
    T_ASSERT_EQ(dark[0], 0x55);
    T_ASSERT_EQ(dark[7], 0x00);
    T_ASSERT_EQ(dark[35], 0x02);

    /* soft variants patch only Ph0 TA/TB down to 1 subframe */
    eink_lut_set_mode(EINK_LUT_MODE_TONE_SOFT_DARK);
    const uint8_t *soft = eink_lut_select_partial();
    T_ASSERT_EQ(soft[35], 0x01);
    T_ASSERT_EQ(soft[36], 0x01);
    T_ASSERT_EQ(soft[0], dark[0]);
    T_ASSERT(memcmp(soft + 1, dark + 1, 34) == 0);   /* VS section intact */
    T_ASSERT(memcmp(soft + 37, dark + 37, EINK_LUT_SIZE - 37) == 0);

    /* out-of-enum mode falls back to balanced */
    eink_lut_set_mode((eink_lut_mode_t)99);
    T_ASSERT_EQ(eink_lut_select_partial()[0], 0xAA);
}

static void test_bound_tables(void)
{
    memset(fake_working, 0x11, sizeof(fake_working));
    memset(fake_default, 0x33, sizeof(fake_default));
    eink_lut_bind_base_tables(fake_working, fake_default);

    eink_lut_set_mode(EINK_LUT_MODE_CLEAN);
    T_ASSERT(eink_lut_select_partial() == fake_default);

    eink_lut_set_use_custom(true);
    T_ASSERT(eink_lut_get_use_custom());
    T_ASSERT(eink_lut_select_partial() == fake_working);
    eink_lut_set_use_custom(false);
}

static void test_flush_red_table(void)
{
    const uint8_t *flush = eink_lut_flush_red();
    T_ASSERT(flush != NULL);
    /* LUT2 (red) must be all-VSL (0xAA) to drive pigment away */
    for (int i = 14; i < 28; i++) {
        T_ASSERT_EQ(flush[i], 0xAA);
    }
}

static void test_vlut_validation(void)
{
    uint8_t off = 0, val = 0x77;

    T_ASSERT_EQ(eink_lut_vlut_get_count(), 8);
    T_ASSERT_EQ(eink_lut_vlut_define(-1, 0, &off, &val, 1), -1);
    T_ASSERT_EQ(eink_lut_vlut_define(8, 0, &off, &val, 1), -1);
    T_ASSERT_EQ(eink_lut_vlut_define(0, 0, &off, &val, -1), -2);
    T_ASSERT_EQ(eink_lut_vlut_define(0, 0, &off, &val, 17), -2);

    T_ASSERT(!eink_lut_vlut_slot_defined(-1));
    T_ASSERT(!eink_lut_vlut_slot_defined(8));

    eink_lut_vlut_activate(99);
    T_ASSERT_EQ(eink_lut_vlut_active(), -1);
}

static void test_vlut_patching(void)
{
    eink_lut_set_mode(EINK_LUT_MODE_TURBO);
    eink_lut_set_use_custom(false);
    const uint8_t *turbo = eink_lut_select_partial();

    uint8_t offs[2] = { 0, 36 };
    uint8_t vals[2] = { 0x77, 0x09 };
    T_ASSERT_EQ(eink_lut_vlut_define(2, 0, offs, vals, 2), 0);
    T_ASSERT(eink_lut_vlut_slot_defined(2));

    eink_lut_vlut_activate(2);
    T_ASSERT_EQ(eink_lut_vlut_active(), 2);

    const uint8_t *patched = eink_lut_select_partial();
    T_ASSERT_EQ(patched[0], 0x77);
    T_ASSERT_EQ(patched[36], 0x09);
    T_ASSERT(memcmp(patched + 1, turbo + 1, 35) == 0);
    T_ASSERT(memcmp(patched + 37, turbo + 37, EINK_LUT_SIZE - 37) == 0);

    /* use_custom outranks an active VLUT slot */
    eink_lut_set_use_custom(true);
    T_ASSERT(eink_lut_select_partial() == fake_working);
    eink_lut_set_use_custom(false);

    /* an out-of-range patch offset is skipped, not written */
    uint8_t bad_off = 200, bad_val = 0xEE;
    T_ASSERT_EQ(eink_lut_vlut_define(3, 0, &bad_off, &bad_val, 1), 0);
    eink_lut_vlut_activate(3);
    const uint8_t *unpatched = eink_lut_select_partial();
    T_ASSERT(memcmp(unpatched, turbo, EINK_LUT_SIZE) == 0);

    /* activating an empty slot leaves mode selection in charge */
    eink_lut_vlut_clear();
    T_ASSERT_EQ(eink_lut_vlut_active(), -1);
    T_ASSERT(!eink_lut_vlut_slot_defined(2));
    eink_lut_vlut_activate(2);
    T_ASSERT(eink_lut_select_partial() == turbo);
    eink_lut_vlut_activate(-1);
}

int main(void)
{
    T_RUN(test_unbound_fallbacks);
    T_RUN(test_mode_selection);
    T_RUN(test_bound_tables);
    T_RUN(test_flush_red_table);
    T_RUN(test_vlut_validation);
    T_RUN(test_vlut_patching);
    return t_report("eink_lut");
}
