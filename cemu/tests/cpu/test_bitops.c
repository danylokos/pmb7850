/* Bit-instruction vectors ported from emu/tests/cpu/test_bitops.py.
 * bitoff 0xF0 -> R0 at CP+0 = 0xFC00 (CP set by the fixture). */
#include "test_harness.h"

#define R0_ADDR 0xFC00

static void test_bset_sets_bit_and_flags(void) {
    SET_MEM16(R0_ADDR, 0x0000);
    RUN(0x2F, 0xF0);             /* bset r0.2 */
    CHECK(MEM16(R0_ADDR) == 0x0004);
    CHECK_FLAG(F_N, 0); CHECK_FLAG(F_Z, 1);
}
static void test_bset_previous_bit_set(void) {
    SET_MEM16(R0_ADDR, 0x0004);
    RUN(0x2F, 0xF0);
    CHECK(MEM16(R0_ADDR) == 0x0004);
    CHECK_FLAG(F_N, 1); CHECK_FLAG(F_Z, 0);
}
static void test_bclr_clears_bit(void) {
    SET_MEM16(R0_ADDR, 0x00FF);
    RUN(0x3E, 0xF0);             /* bclr r0.3 */
    CHECK(MEM16(R0_ADDR) == 0x00F7);
    CHECK_FLAG(F_N, 1); CHECK_FLAG(F_Z, 0);
}
static void two_bit_setup(uint16_t dst, uint16_t src) {
    SET_MEM16(R0_ADDR, dst);
    SET_MEM16(R0_ADDR + 2, src);
}
static void test_band_both_one(void) {
    two_bit_setup(0x0001, 0x0001);
    RUN(0x6A, 0xF1, 0xF0, 0x00); /* band r0.0, r1.0 */
    CHECK((MEM16(R0_ADDR) & 1) == 1);
    CHECK_FLAG(F_N, 0); CHECK_FLAG(F_C, 1); CHECK_FLAG(F_V, 1); CHECK_FLAG(F_Z, 0);
}
static void test_bxor_differing(void) {
    two_bit_setup(0x0001, 0x0000);
    RUN(0x7A, 0xF1, 0xF0, 0x00); /* bxor r0.0, r1.0 */
    CHECK((MEM16(R0_ADDR) & 1) == 1);
    CHECK_FLAG(F_N, 1); CHECK_FLAG(F_C, 0); CHECK_FLAG(F_V, 1); CHECK_FLAG(F_Z, 0);
}
static void test_bor_both_zero_is_nor(void) {
    two_bit_setup(0x0000, 0x0000);
    RUN(0x5A, 0xF1, 0xF0, 0x00); /* bor r0.0, r1.0 */
    CHECK_FLAG(F_Z, 1); CHECK_FLAG(F_V, 0); CHECK_FLAG(F_C, 0); CHECK_FLAG(F_N, 0);
}
static void test_bmov_copies_bit(void) {
    two_bit_setup(0x0000, 0x0001);
    RUN(0x4A, 0xF1, 0xF0, 0x00); /* bmov r0.0, r1.0 */
    CHECK((MEM16(R0_ADDR) & 1) == 1);
    CHECK_FLAG(F_N, 1); CHECK_FLAG(F_Z, 0); CHECK_FLAG(F_V, 0); CHECK_FLAG(F_C, 0);
}
static void test_bmovn_inverts_bit(void) {
    two_bit_setup(0x0000, 0x0000);
    RUN(0x3A, 0xF1, 0xF0, 0x00); /* bmovn r0.0, r1.0 */
    CHECK((MEM16(R0_ADDR) & 1) == 1);
    CHECK_FLAG(F_N, 0); CHECK_FLAG(F_Z, 1);
}
static void test_bfldl_low_byte(void) {
    SET_MEM16(R0_ADDR, 0xFFFF);
    RUN(0x0A, 0xF0, 0x0F, 0x0A); /* bfldl r0, mask=0x0F, data=0x0A */
    CHECK(MEM16(R0_ADDR) == 0xFFFA);
}
static void test_bfldh_high_byte(void) {
    SET_MEM16(R0_ADDR, 0xFFFF);
    RUN(0x1A, 0xF0, 0x0A, 0x0F); /* bfldh r0, mask=0x0F, data=0x0A */
    CHECK(MEM16(R0_ADDR) == 0xFAFF);
}
static void test_bfldl_clears_masked_bits(void) {
    SET_MEM16(R0_ADDR, 0xFFFF);
    RUN(0x0A, 0xF0, 0x0F, 0x00);
    CHECK(MEM16(R0_ADDR) == 0xFFF0);
}
static void test_bfldl_raw_data_sets_bits_outside_mask(void) {
    SET_MEM16(R0_ADDR, 0x0000);
    RUN(0x0A, 0xF0, 0x0F, 0xFF);
    CHECK(MEM16(R0_ADDR) == 0x00FF);
}
static void test_bfldh_clears_masked_bits(void) {
    SET_MEM16(R0_ADDR, 0xFFFF);
    RUN(0x1A, 0xF0, 0x00, 0x0F);
    CHECK(MEM16(R0_ADDR) == 0xF0FF);
}

static const test_entry_t TESTS[] = {
    {"bset_sets_bit_and_flags", test_bset_sets_bit_and_flags},
    {"bset_previous_bit_set", test_bset_previous_bit_set},
    {"bclr_clears_bit", test_bclr_clears_bit},
    {"band_both_one", test_band_both_one},
    {"bxor_differing", test_bxor_differing},
    {"bor_both_zero_is_nor", test_bor_both_zero_is_nor},
    {"bmov_copies_bit", test_bmov_copies_bit},
    {"bmovn_inverts_bit", test_bmovn_inverts_bit},
    {"bfldl_low_byte", test_bfldl_low_byte},
    {"bfldh_high_byte", test_bfldh_high_byte},
    {"bfldl_clears_masked_bits", test_bfldl_clears_masked_bits},
    {"bfldl_raw_data_sets_bits_outside_mask", test_bfldl_raw_data_sets_bits_outside_mask},
    {"bfldh_clears_masked_bits", test_bfldh_clears_masked_bits},
};

int main(void) { return run_all(TESTS, (int)(sizeof(TESTS)/sizeof(TESTS[0]))); }
