/* MOV/MOVB/MOVBZ/MOVBS vectors ported from emu/tests/cpu/test_mov.py. */
#include "test_harness.h"

static void test_mov_data4_zero_extended(void) {
    RUN(0xE0, 0xC3);              /* mov r3, #0xC */
    CHECK(GPR(3) == 0x000C);
    CHECK_FLAG(F_Z, 0); CHECK_FLAG(F_N, 0);
}
static void test_mov_data4_zero_value(void) {
    RUN(0xE0, 0x05);
    CHECK(GPR(5) == 0);
    CHECK_FLAG(F_Z, 1);
}
static void test_mov_reg_reg_flags_from_source(void) {
    SET_GPR(1, 0x8000);
    RUN(0xF0, 0x01);             /* mov r0, r1 */
    CHECK(GPR(0) == 0x8000);
    CHECK_FLAG(F_N, 1); CHECK_FLAG(F_E, 1); CHECK_FLAG(F_Z, 0);
}
static void test_mov_reg_reg_preserves_v_c(void) {
    SET_GPR(1, 1);
    SET_PSW(0x0006);
    RUN(0xF0, 0x01);
    CHECK_FLAG(F_V, 1); CHECK_FLAG(F_C, 1);
}
static void test_mov_reg_imm16(void) {
    RUN(0xE6, 0xF4, 0xCD, 0xAB); /* mov r4, #0xABCD */
    CHECK(GPR(4) == 0xABCD);
}
static void test_mov_reg_from_mem(void) {
    SET_MEM16(0x0200, 0xBEEF);
    RUN(0xF2, 0xF7, 0x00, 0x02); /* mov r7, 0x0200 */
    CHECK(GPR(7) == 0xBEEF);
}
static void test_mov_mem_from_reg(void) {
    SET_GPR(7, 0x1357);
    RUN(0xF6, 0xF7, 0x10, 0x02); /* mov 0x0210, r7 */
    CHECK(MEM16(0x0210) == 0x1357);
}
static void test_movb_reg_from_mem(void) {
    SET_MEM8(0x0300, 0xA5);
    RUN(0xF3, 0xF2, 0x00, 0x03); /* movb rb2, 0x0300 */
    CHECK((GPR(1) & 0xFF) == 0xA5);
}
static void test_mov_indexed_load(void) {
    SET_GPR(1, 0x0400);
    SET_MEM16(0x0404, 0xCAFE);
    RUN(0xD4, 0x01, 0x04, 0x00); /* mov r0, [r1 + #4] */
    CHECK(GPR(0) == 0xCAFE);
}
static void test_mov_indexed_store(void) {
    SET_GPR(1, 0x0500); SET_GPR(0, 0x9999);
    RUN(0xC4, 0x01, 0x02, 0x00); /* mov [r1 + #2], r0 */
    CHECK(MEM16(0x0502) == 0x9999);
}
static void test_mov_postinc_word(void) {
    SET_GPR(1, 0x0600);
    SET_MEM16(0x0600, 0x1122);
    RUN(0x98, 0x01);             /* mov r0, [r1+] */
    CHECK(GPR(0) == 0x1122);
    CHECK(GPR(1) == 0x0602);
}
static void test_movb_postinc_byte(void) {
    SET_GPR(1, 0x0700);
    SET_MEM8(0x0700, 0x44);
    RUN(0x99, 0x01);             /* movb rb0, [r1+] */
    CHECK((GPR(0) & 0xFF) == 0x44);
    CHECK(GPR(1) == 0x0701);
}
static void test_mov_predec_word(void) {
    SET_GPR(1, 0x0802); SET_GPR(0, 0x5566);
    RUN(0x88, 0x01);             /* mov [-r1], r0 */
    CHECK(GPR(1) == 0x0800);
    CHECK(MEM16(0x0800) == 0x5566);
}
static void test_movbz_reg_reg(void) {
    SET_GPR(4, 0x00C7);          /* rb8 = R4.low = 0xC7 */
    RUN(0xC0, 0x84);             /* movbz r4, rb8 */
    CHECK(GPR(4) == 0x00C7);
    CHECK_FLAG(F_Z, 0); CHECK_FLAG(F_N, 0);
}
static void test_movbz_zero(void) {
    SET_GPR(4, 0x0000);
    RUN(0xC0, 0x84);
    CHECK(GPR(4) == 0);
    CHECK_FLAG(F_Z, 1); CHECK_FLAG(F_N, 0);
}
static void test_movbs_sign_extend(void) {
    SET_GPR(4, 0x0080);
    RUN(0xD0, 0x84);             /* movbs r4, rb8 */
    CHECK(GPR(4) == 0xFF80);
    CHECK_FLAG(F_N, 1);
}
static void test_movbs_positive(void) {
    SET_GPR(4, 0x007F);
    RUN(0xD0, 0x84);
    CHECK(GPR(4) == 0x007F);
    CHECK_FLAG(F_N, 0);
}
static void test_movbs_mem_from_reg_sign_extends_word(void) {
    SET_GPR(2, 0x0090);          /* rb4 = R2.low = 0x90 */
    RUN(0xD5, 0xF4, 0x00, 0x09); /* movbs 0x0900, rb4 */
    CHECK(MEM16(0x0900) == 0xFF90);
}

static const test_entry_t TESTS[] = {
    {"mov_data4_zero_extended", test_mov_data4_zero_extended},
    {"mov_data4_zero_value", test_mov_data4_zero_value},
    {"mov_reg_reg_flags_from_source", test_mov_reg_reg_flags_from_source},
    {"mov_reg_reg_preserves_v_c", test_mov_reg_reg_preserves_v_c},
    {"mov_reg_imm16", test_mov_reg_imm16},
    {"mov_reg_from_mem", test_mov_reg_from_mem},
    {"mov_mem_from_reg", test_mov_mem_from_reg},
    {"movb_reg_from_mem", test_movb_reg_from_mem},
    {"mov_indexed_load", test_mov_indexed_load},
    {"mov_indexed_store", test_mov_indexed_store},
    {"mov_postinc_word", test_mov_postinc_word},
    {"movb_postinc_byte", test_movb_postinc_byte},
    {"mov_predec_word", test_mov_predec_word},
    {"movbz_reg_reg", test_movbz_reg_reg},
    {"movbz_zero", test_movbz_zero},
    {"movbs_sign_extend", test_movbs_sign_extend},
    {"movbs_positive", test_movbs_positive},
    {"movbs_mem_from_reg_sign_extends_word", test_movbs_mem_from_reg_sign_extends_word},
};

int main(void) { return run_all(TESTS, (int)(sizeof(TESTS)/sizeof(TESTS[0]))); }
