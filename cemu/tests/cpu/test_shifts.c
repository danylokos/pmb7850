/* Shift/rotate vectors ported from emu/tests/cpu/test_shifts.py.
 * The subtle case is the sticky-V ordering for SHR/ROR/ASHR. */
#include "test_harness.h"

static void test_shl_imm(void) {
    SET_GPR(0, 0x1234);
    RUN(0x5C, 0x40);             /* shl r0, #4 */
    CHECK(GPR(0) == 0x2340);
    CHECK_FLAG(F_V, 0);
}
static void test_shl_carry_from_msb(void) {
    SET_GPR(0, 0x8000);
    RUN(0x5C, 0x10);             /* shl r0, #1 */
    CHECK(GPR(0) == 0x0000);
    CHECK_FLAG(F_C, 1); CHECK_FLAG(F_Z, 1); CHECK_FLAG(F_V, 0);
}
static void test_shl_count_zero_clears_carry(void) {
    SET_GPR(0, 0xFFFF);
    SET_PSW(0x0002);
    RUN(0x5C, 0x00);             /* shl r0, #0 */
    CHECK_FLAG(F_C, 0);
}
static void test_shr_imm(void) {
    SET_GPR(0, 0x2340);
    RUN(0x7C, 0x40);             /* shr r0, #4 */
    CHECK(GPR(0) == 0x0234);
}
static void test_shr_v_excludes_last_bit_out(void) {
    SET_GPR(0, 0x0002);
    RUN(0x7C, 0x20);             /* shr r0, #2 */
    CHECK(GPR(0) == 0x0000);
    CHECK_FLAG(F_C, 1); CHECK_FLAG(F_V, 0); CHECK_FLAG(F_Z, 1);
}
static void test_shr_v_set_when_earlier_one(void) {
    SET_GPR(0, 0x0003);
    RUN(0x7C, 0x20);
    CHECK(GPR(0) == 0x0000);
    CHECK_FLAG(F_C, 1); CHECK_FLAG(F_V, 1);
}
static void test_shr_single_bit_never_sets_v(void) {
    SET_GPR(0, 0x0001);
    RUN(0x7C, 0x10);             /* shr r0, #1 */
    CHECK_FLAG(F_C, 1); CHECK_FLAG(F_V, 0); CHECK_FLAG(F_Z, 1);
}
static void test_rol(void) {
    SET_GPR(0, 0x8001);
    RUN(0x1C, 0x10);             /* rol r0, #1 */
    CHECK(GPR(0) == 0x0003);
    CHECK_FLAG(F_C, 1); CHECK_FLAG(F_V, 0);
}
static void test_ror(void) {
    SET_GPR(0, 0x0001);
    RUN(0x3C, 0x10);             /* ror r0, #1 */
    CHECK(GPR(0) == 0x8000);
    CHECK_FLAG(F_C, 1); CHECK_FLAG(F_V, 0);
}
static void test_ror_v_sticky(void) {
    SET_GPR(0, 0x0003);
    RUN(0x3C, 0x20);             /* ror r0, #2 */
    CHECK_FLAG(F_V, 1);
}
static void test_ashr_sign_extend(void) {
    SET_GPR(0, 0x8000);
    RUN(0xBC, 0x10);             /* ashr r0, #1 */
    CHECK(GPR(0) == 0xC000);
    CHECK_FLAG(F_N, 1); CHECK_FLAG(F_C, 0);
}
static void test_ashr_positive(void) {
    SET_GPR(0, 0x0F00);
    RUN(0xBC, 0x40);             /* ashr r0, #4 */
    CHECK(GPR(0) == 0x00F0);
}
static void test_ashr_v_excludes_last_bit_out(void) {
    SET_GPR(0, 0x0002);
    RUN(0xBC, 0x20);
    CHECK_FLAG(F_C, 1); CHECK_FLAG(F_V, 0);
}
static void test_ashr_v_set_when_earlier_one(void) {
    SET_GPR(0, 0x0003);
    RUN(0xBC, 0x20);             /* ashr r0, #2 */
    CHECK_FLAG(F_C, 1); CHECK_FLAG(F_V, 1);
}
static void test_shift_reg_count_uses_low_4_bits(void) {
    SET_GPR(0, 0x0001); SET_GPR(1, 0x0014);
    RUN(0x4C, 0x01);             /* shl r0, r1 (count=0x14&0xF=4) */
    CHECK(GPR(0) == 0x0010);
}

static const test_entry_t TESTS[] = {
    {"shl_imm", test_shl_imm},
    {"shl_carry_from_msb", test_shl_carry_from_msb},
    {"shl_count_zero_clears_carry", test_shl_count_zero_clears_carry},
    {"shr_imm", test_shr_imm},
    {"shr_v_excludes_last_bit_out", test_shr_v_excludes_last_bit_out},
    {"shr_v_set_when_earlier_one", test_shr_v_set_when_earlier_one},
    {"shr_single_bit_never_sets_v", test_shr_single_bit_never_sets_v},
    {"rol", test_rol},
    {"ror", test_ror},
    {"ror_v_sticky", test_ror_v_sticky},
    {"ashr_sign_extend", test_ashr_sign_extend},
    {"ashr_positive", test_ashr_positive},
    {"ashr_v_excludes_last_bit_out", test_ashr_v_excludes_last_bit_out},
    {"ashr_v_set_when_earlier_one", test_ashr_v_set_when_earlier_one},
    {"shift_reg_count_uses_low_4_bits", test_shift_reg_count_uses_low_4_bits},
};

int main(void) { return run_all(TESTS, (int)(sizeof(TESTS)/sizeof(TESTS[0]))); }
