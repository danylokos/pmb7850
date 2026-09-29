/* Control-flow vectors ported from emu/tests/cpu/test_controlflow.py.
 * Condition codes, jumps, calls, returns, JB/JNB/JBC, PUSH/POP, TRAP, RETI. */
#include "test_harness.h"

#define SP_ADDR  0xFE12
#define TFR      0xFFAC
#define SP()     MEM16(SP_ADDR)

static void test_jmpr_uc_always_taken(void) {
    RUN_AT(0x0100, 0x0D, 0x02);  /* jmpr cc_UC, +2 */
    CHECK(IP() == 0x0106);
}
static void test_jmpr_z_taken_when_zero(void) {
    SET_PSW(0x0008);
    RUN_AT(0x0100, 0x2D, 0x03);  /* jmpr cc_Z, +3 */
    CHECK(IP() == 0x0100 + 2 + 3 * 2);
}
static void test_jmpr_z_not_taken(void) {
    SET_PSW(0x0000);
    RUN_AT(0x0100, 0x2D, 0x03);
    CHECK(IP() == 0x0102);
}
static void test_jmpr_negative_offset(void) {
    RUN_AT(0x0100, 0x0D, 0xFE);  /* rel=-2 */
    CHECK(IP() == 0x00FE);
}
static void test_cc_ugt_unsigned(void) {
    SET_PSW(0x0000);
    RUN_AT(0x0100, 0xED, 0x01);
    CHECK(IP() == 0x0104);
    SET_PSW(0x0002);
    RUN_AT(0x0200, 0xED, 0x01);
    CHECK(IP() == 0x0202);
}
static void test_cc_sgt_signed(void) {
    SET_PSW(0x0000);
    RUN_AT(0x0100, 0xAD, 0x01);
    CHECK(IP() == 0x0104);
    SET_PSW(0x0001);
    RUN_AT(0x0200, 0xAD, 0x01);
    CHECK(IP() == 0x0202);
}
static void test_jmpa_taken(void) {
    SET_PSW(0x0008);
    RUN_AT(0x0100, 0xEA, 0x20, 0x00, 0x30);  /* jmpa cc_Z, 0x3000 */
    CHECK(IP() == 0x3000);
}
static void test_calla_pushes_ip(void) {
    uint16_t sp0 = SP();
    RUN_AT(0x0100, 0xCA, 0x00, 0x00, 0x40);  /* calla cc_UC, 0x4000 */
    CHECK(IP() == 0x4000);
    CHECK(SP() == ((sp0 - 2) & 0xFFFF));
    CHECK(MEM16(SP()) == 0x0104);
}
static void test_callr_then_ret(void) {
    uint16_t sp0 = SP();
    RUN_AT(0x0100, 0xBB, 0x04);  /* callr +4 -> 0x010A */
    CHECK(IP() == 0x010A);
    CHECK(MEM16(SP()) == 0x0102);
    RUN_AT(PC(), 0xCB, 0x00);    /* ret */
    CHECK(IP() == 0x0102);
    CHECK(SP() == sp0);
}
static void test_calls_pushes_csp_and_ip(void) {
    g_cpu.csp = 0x00;
    uint16_t sp0 = SP();
    RUN_AT(0x0100, 0xDA, 0x05, 0x00, 0x20);  /* calls 0x05, 0x2000 */
    CHECK(CSP() == 0x05 && IP() == 0x2000);
    CHECK(MEM16(SP()) == 0x0104);
    CHECK(MEM16(SP() + 2) == 0x0000);
    CHECK(SP() == ((sp0 - 4) & 0xFFFF));
}
static void test_jmpi_indirect(void) {
    SET_GPR(3, 0x5000);
    RUN_AT(0x0100, 0x9C, 0x03);  /* jmpi cc_UC, [r3] */
    CHECK(IP() == 0x5000);
}
static void test_jb_taken_when_set(void) {
    SET_MEM16(0xFC00, 0x0001);
    RUN_AT(0x0100, 0x8A, 0xF0, 0x02, 0x00);  /* jb r0.0, +2 */
    CHECK(IP() == 0x0104 + 2 * 2);
}
static void test_jnb_not_taken_when_set(void) {
    SET_MEM16(0xFC00, 0x0001);
    RUN_AT(0x0100, 0x9A, 0xF0, 0x02, 0x00);  /* jnb r0.0 */
    CHECK(IP() == 0x0104);
}
static void test_jbc_clears_bit_when_taken(void) {
    SET_MEM16(0xFC00, 0x0001);
    RUN_AT(0x0100, 0xAA, 0xF0, 0x02, 0x00);  /* jbc r0.0, +2 */
    CHECK((MEM16(0xFC00) & 1) == 0);
    CHECK(IP() == 0x0104 + 4);
    CHECK_FLAG(F_N, 1); CHECK_FLAG(F_Z, 0);
}
static void test_jbc_not_taken_when_clear(void) {
    SET_MEM16(0xFC00, 0x0000);
    RUN_AT(0x0100, 0xAA, 0xF0, 0x02, 0x00);
    CHECK(IP() == 0x0104);
    CHECK_FLAG(F_N, 0); CHECK_FLAG(F_Z, 1);
}
static void test_extr_jb_uses_esfr_bitoff_window(void) {
    SET_MEM16(0xFF3C, 0x0000);   /* normal SFR bitoff 0x9e target */
    SET_MEM16(0xF13C, 0x0002);   /* ESFR IRQ78IC bit 1 */
    LOAD_AT(0x0100, 0xD1, 0x80, 0x8A, 0x9E, 0x0A, 0x10);
    g_cpu.csp = 0; g_cpu.ip = 0x0100;
    STEP();                      /* extr #1 */
    STEP();                      /* jb 0x9e.1, +0x0a */
    CHECK(IP() == 0x0106 + 0x0A * 2);
    CHECK(g_cpu.extr == 0);
    CHECK(g_cpu.ext_count == 0);
}
static void test_push_pop_roundtrip(void) {
    SET_GPR(5, 0xABCD);
    uint16_t sp0 = SP();
    RUN_AT(0x0100, 0xEC, 0xF5);  /* push r5 */
    CHECK(SP() == ((sp0 - 2) & 0xFFFF));
    CHECK(MEM16(SP()) == 0xABCD);
    RUN_AT(0x0102, 0xFC, 0xF6);  /* pop r6 */
    CHECK(GPR(6) == 0xABCD);
    CHECK(SP() == sp0);
}
static void test_trap_pushes_psw_csp_ip_and_vectors(void) {
    SET_PSW(0x00AA);
    uint16_t sp0 = SP();
    RUN_AT(0x0100, 0x9B, 0x08);  /* trap #4 -> vec 0x10 */
    CHECK(IP() == 0x10 && CSP() == 0x00);
    CHECK(SP() == ((sp0 - 6) & 0xFFFF));
    CHECK(MEM16(SP()) == 0x0102);
}
static void test_reti_restores_psw_csp_ip(void) {
    SET_MEM16(SP_ADDR, 0xFB00 - 6);
    uint32_t base = 0xFB00 - 6;
    SET_MEM16(base + 0, 0x1234);
    SET_MEM16(base + 2, 0x0001);
    SET_MEM16(base + 4, 0x0055);
    RUN_AT(0x0100, 0xFB, 0x88);  /* reti */
    CHECK(IP() == 0x1234);
    CHECK(CSP() == 0x01);
    CHECK(PSW() == 0x0055);
    CHECK(SP() == 0xFB00);
}
static void test_undefined_opcode_44_45_enters_class_b_trap(void) {
    int ops[2] = {0x44, 0x45};
    for (int i = 0; i < 2; i++) {
        SET_MEM16(SP_ADDR, 0xFB00);
        SET_MEM16(TFR, 0x0000);
        SET_PSW(0x0048);
        RUN_AT(0x010100, (uint8_t)ops[i], 0x00);
        CHECK(CSP() == 0x00);
        CHECK(IP() == 0x0028);   /* BTRAP 0x0A -> vec 0x28 */
        CHECK(MEM16(TFR) & 0x0080);
        CHECK((PSW() & 0xF000) == 0xF000);
        uint32_t base = 0xFB00 - 6;
        CHECK(SP() == base);
        CHECK(MEM16(base + 0) == 0x0100);
        CHECK(MEM16(base + 2) == 0x0001);
        CHECK(MEM16(base + 4) == 0x0048);
    }
}
static void test_reset_and_class_b_vectors_can_far_jump(void) {
    RUN_AT(0x0000, 0xFA, 0x80, 0xC4, 0x2F);
    CHECK(PC() == 0x802FC4);

    SET_MEM16(SP_ADDR, 0xFB00);
    SET_MEM8(0x0028, 0xFA);
    SET_MEM8(0x0029, 0x80);
    SET_MEM8(0x002A, 0x30);
    SET_MEM8(0x002B, 0x01);
    RUN_AT(0x0100, 0x44, 0x00);
    CHECK(PC() == 0x000028);
    STEP();
    CHECK(PC() == 0x800130);
}
static void test_max_v1_vector_slot_is_01fc(void) {
    RUN_AT(0x0100, 0x9B, 0xFE);  /* TRAP #0x7f */
    CHECK(PC() == 0x0001FC);
}

static const test_entry_t TESTS[] = {
    {"jmpr_uc_always_taken", test_jmpr_uc_always_taken},
    {"jmpr_z_taken_when_zero", test_jmpr_z_taken_when_zero},
    {"jmpr_z_not_taken", test_jmpr_z_not_taken},
    {"jmpr_negative_offset", test_jmpr_negative_offset},
    {"cc_ugt_unsigned", test_cc_ugt_unsigned},
    {"cc_sgt_signed", test_cc_sgt_signed},
    {"jmpa_taken", test_jmpa_taken},
    {"calla_pushes_ip", test_calla_pushes_ip},
    {"callr_then_ret", test_callr_then_ret},
    {"calls_pushes_csp_and_ip", test_calls_pushes_csp_and_ip},
    {"jmpi_indirect", test_jmpi_indirect},
    {"jb_taken_when_set", test_jb_taken_when_set},
    {"jnb_not_taken_when_set", test_jnb_not_taken_when_set},
    {"jbc_clears_bit_when_taken", test_jbc_clears_bit_when_taken},
    {"jbc_not_taken_when_clear", test_jbc_not_taken_when_clear},
    {"extr_jb_uses_esfr_bitoff_window", test_extr_jb_uses_esfr_bitoff_window},
    {"push_pop_roundtrip", test_push_pop_roundtrip},
    {"trap_pushes_psw_csp_ip_and_vectors", test_trap_pushes_psw_csp_ip_and_vectors},
    {"reti_restores_psw_csp_ip", test_reti_restores_psw_csp_ip},
    {"undefined_opcode_44_45_enters_class_b_trap", test_undefined_opcode_44_45_enters_class_b_trap},
    {"reset_and_class_b_vectors_can_far_jump", test_reset_and_class_b_vectors_can_far_jump},
    {"max_v1_vector_slot_is_01fc", test_max_v1_vector_slot_is_01fc},
};

int main(void) { return run_all(TESTS, (int)(sizeof(TESTS)/sizeof(TESTS[0]))); }
