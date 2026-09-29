/* ALU family vectors ported from emu/tests/cpu/test_alu.py.
 * add/addc/sub/subc/cmp/and/or/xor, neg/cpl, cmpi/cmpd. */
#include "test_harness.h"

static void test_add_basic(void) {
    SET_GPR(0, 5); SET_GPR(1, 7);
    RUN(0x00, 0x01);              /* add r0, r1 */
    CHECK(GPR(0) == 12);
    CHECK_FLAG(F_Z, 0); CHECK_FLAG(F_N, 0); CHECK_FLAG(F_C, 0); CHECK_FLAG(F_V, 0);
}

static void test_add_carry_out(void) {
    SET_GPR(0, 0xFFFF); SET_GPR(1, 1);
    RUN(0x00, 0x01);
    CHECK(GPR(0) == 0x0000);
    CHECK_FLAG(F_Z, 1); CHECK_FLAG(F_C, 1); CHECK_FLAG(F_V, 0); CHECK_FLAG(F_N, 0);
}

static void test_add_signed_overflow(void) {
    SET_GPR(0, 0x7FFF); SET_GPR(1, 1);
    RUN(0x00, 0x01);
    CHECK(GPR(0) == 0x8000);
    CHECK_FLAG(F_V, 1); CHECK_FLAG(F_N, 1); CHECK_FLAG(F_C, 0); CHECK_FLAG(F_Z, 0);
}

static void test_add_e_flag_from_op2(void) {
    SET_GPR(0, 0); SET_GPR(1, 0x8000);
    RUN(0x00, 0x01);
    CHECK_FLAG(F_E, 1);
}

static void test_sub_no_borrow(void) {
    SET_GPR(0, 10); SET_GPR(1, 3);
    RUN(0x20, 0x01);              /* sub r0, r1 */
    CHECK(GPR(0) == 7);
    CHECK_FLAG(F_C, 0); CHECK_FLAG(F_Z, 0); CHECK_FLAG(F_N, 0); CHECK_FLAG(F_V, 0);
}

static void test_sub_borrow(void) {
    SET_GPR(0, 3); SET_GPR(1, 10);
    RUN(0x20, 0x01);
    CHECK(GPR(0) == ((3 - 10) & 0xFFFF));
    CHECK_FLAG(F_C, 1); CHECK_FLAG(F_N, 1);
}

static void test_cmp_equal_sets_zero_no_write(void) {
    SET_GPR(0, 0x1234); SET_GPR(1, 0x1234);
    RUN(0x40, 0x01);              /* cmp r0, r1 */
    CHECK(GPR(0) == 0x1234);      /* CMP must not write */
    CHECK_FLAG(F_Z, 1); CHECK_FLAG(F_C, 0);
}

static void test_addc_uses_carry_in(void) {
    SET_GPR(0, 0x0001); SET_GPR(1, 0x0001);
    SET_PSW(PSW() | 0x0002);      /* set C */
    RUN(0x10, 0x01);              /* addc r0, r1 -> 1+1+1 */
    CHECK(GPR(0) == 3);
}

static void test_addc_sticky_zero(void) {
    SET_GPR(0, 0); SET_GPR(1, 0);
    SET_PSW(0);
    RUN(0x10, 0x01);
    CHECK(GPR(0) == 0);
    CHECK_FLAG(F_Z, 0);           /* result 0 but prev Z clear */
}

static void test_subc_sticky_zero_preserves_prev_z(void) {
    SET_GPR(0, 5); SET_GPR(1, 5);
    SET_PSW(0x0008);              /* previous Z = 1 */
    RUN(0x30, 0x01);              /* subc r0, r1 */
    CHECK(GPR(0) == 0);
    CHECK_FLAG(F_Z, 1);
}

static void test_and_clears_v_c(void) {
    SET_GPR(0, 0xFF0F); SET_GPR(1, 0x0FF0);
    SET_PSW(0x0006);              /* pre-set V, C */
    RUN(0x60, 0x01);              /* and r0, r1 */
    CHECK(GPR(0) == 0x0F00);
    CHECK_FLAG(F_V, 0); CHECK_FLAG(F_C, 0); CHECK_FLAG(F_Z, 0); CHECK_FLAG(F_N, 0);
}

static void test_or_and_xor(void) {
    SET_GPR(0, 0xF000); SET_GPR(1, 0x000F);
    RUN(0x70, 0x01); CHECK(GPR(0) == 0xF00F);   /* or */
    SET_GPR(0, 0xFF00); SET_GPR(1, 0x0F0F);
    RUN(0x50, 0x01); CHECK(GPR(0) == 0xF00F);   /* xor */
}

static void test_addb_byte_width(void) {
    SET_GPR(0, 0x03FE);           /* rb0=0xFE, rb1=0x03 */
    RUN(0x01, 0x01);              /* addb rb0, rb1 */
    CHECK(GPR(0) == 0x0301);
    CHECK_FLAG(F_C, 1);
}

static void test_add_reg_imm16(void) {
    SET_GPR(2, 0x1000);
    RUN(0x06, 0xF2, 0x34, 0x12);  /* add r2, #0x1234 */
    CHECK(GPR(2) == 0x2234);
}

static void test_neg(void) {
    SET_GPR(3, 1);
    RUN(0x81, 0x30);              /* neg r3 */
    CHECK(GPR(3) == 0xFFFF);
    CHECK_FLAG(F_N, 1); CHECK_FLAG(F_C, 1);
}

static void test_neg_zero(void) {
    SET_GPR(3, 0);
    RUN(0x81, 0x30);
    CHECK(GPR(3) == 0);
    CHECK_FLAG(F_Z, 1); CHECK_FLAG(F_C, 0);
}

static void test_cpl_clears_v_and_c(void) {
    SET_GPR(3, 0x0F0F);
    SET_PSW(0x0006);
    RUN(0x91, 0x30);              /* cpl r3 */
    CHECK(GPR(3) == 0xF0F0);
    CHECK_FLAG(F_V, 0); CHECK_FLAG(F_C, 0); CHECK_FLAG(F_N, 1);
}

static void test_cpl_e_flag_from_original(void) {
    SET_GPR(3, 0x8000);
    RUN(0x91, 0x30);
    CHECK_FLAG(F_E, 1);
}

static void test_cmpi1_increments_after_compare(void) {
    SET_GPR(0, 5);
    RUN(0x80, 0x50);              /* cmpi1 r0, #5 */
    CHECK(GPR(0) == 6);
    CHECK_FLAG(F_Z, 1);
}

static void test_cmpd1_decrements_after_compare(void) {
    SET_GPR(0, 8);
    RUN(0xA0, 0x30);              /* cmpd1 r0, #3 */
    CHECK(GPR(0) == 7);
    CHECK_FLAG(F_Z, 0); CHECK_FLAG(F_C, 0);
}

static const test_entry_t TESTS[] = {
    {"add_basic", test_add_basic},
    {"add_carry_out", test_add_carry_out},
    {"add_signed_overflow", test_add_signed_overflow},
    {"add_e_flag_from_op2", test_add_e_flag_from_op2},
    {"sub_no_borrow", test_sub_no_borrow},
    {"sub_borrow", test_sub_borrow},
    {"cmp_equal_sets_zero_no_write", test_cmp_equal_sets_zero_no_write},
    {"addc_uses_carry_in", test_addc_uses_carry_in},
    {"addc_sticky_zero", test_addc_sticky_zero},
    {"subc_sticky_zero_preserves_prev_z", test_subc_sticky_zero_preserves_prev_z},
    {"and_clears_v_c", test_and_clears_v_c},
    {"or_and_xor", test_or_and_xor},
    {"addb_byte_width", test_addb_byte_width},
    {"add_reg_imm16", test_add_reg_imm16},
    {"neg", test_neg},
    {"neg_zero", test_neg_zero},
    {"cpl_clears_v_and_c", test_cpl_clears_v_and_c},
    {"cpl_e_flag_from_original", test_cpl_e_flag_from_original},
    {"cmpi1_increments_after_compare", test_cmpi1_increments_after_compare},
    {"cmpd1_decrements_after_compare", test_cmpd1_decrements_after_compare},
};

int main(void) {
    return run_all(TESTS, (int)(sizeof(TESTS) / sizeof(TESTS[0])));
}
