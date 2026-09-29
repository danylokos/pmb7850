/* DPP/EXT addressing, MUL/DIV, EXT-window lifecycle — ported from
 * emu/tests/cpu/test_addressing_muldiv.py. */
#include "test_harness.h"

#define MDL 0xFE0E
#define MDH 0xFE0C

static void test_dpp_identity_mapping(void) {
    SET_MEM16(0x4000, 0x7777);
    RUN(0xF2, 0xF0, 0x00, 0x40); /* mov r0, 0x4000 (page 1 -> DPP1=1) */
    CHECK(GPR(0) == 0x7777);
}
static void test_dpp_remap(void) {
    SET_MEM16(0xFE04, 0x0010);   /* DPP2 = 0x10 */
    SET_MEM16((0x10u << 14) | 0x0123, 0x4242);
    RUN(0xF2, 0xF0, 0x23, 0x81); /* mov r0, 0x8123 */
    CHECK(GPR(0) == 0x4242);
}
static void test_exts_segment_override(void) {
    SET_MEM16((0x05u << 16) | 0x1000, 0xDEAD);
    LOAD_AT(0x0100, 0xD7, 0x00, 0x05, 0x00,   /* exts #0x05, #1 */
                    0xF2, 0xF0, 0x00, 0x10);  /* mov r0, 0x1000 */
    g_cpu.csp = 0; g_cpu.ip = 0x0100;
    STEP();
    CHECK(g_cpu.ext_kind == EXT_SEG && g_cpu.ext_val == 0x05 && g_cpu.ext_count == 1);
    STEP();
    CHECK(GPR(0) == 0xDEAD);
    CHECK(g_cpu.ext_kind == EXT_NONE);
}
static void test_extp_page_override(void) {
    SET_MEM16((0x10u << 14) | 0x0050, 0xBABE);
    LOAD_AT(0x0100, 0xD7, 0x40, 0x10, 0x00,   /* extp #0x10, #1 */
                    0xF2, 0xF0, 0x50, 0x00);  /* mov r0, 0x0050 */
    g_cpu.csp = 0; g_cpu.ip = 0x0100;
    STEP(); STEP();
    CHECK(GPR(0) == 0xBABE);
}
static void test_ext_window_count_two(void) {
    LOAD_AT(0x0100, 0xD7, 0x10, 0x05, 0x00);  /* exts #0x05, #2 */
    g_cpu.csp = 0; g_cpu.ip = 0x0100;
    STEP();
    CHECK(g_cpu.ext_count == 2);
    LOAD_AT(0x0104, 0xCC, 0x00, 0xCC, 0x00);
    STEP(); CHECK(g_cpu.ext_kind != EXT_NONE);
    STEP(); CHECK(g_cpu.ext_kind == EXT_NONE);
}
static void test_extr_sets_esfr_window(void) {
    RUN_AT(0x0100, 0xD1, 0x80);  /* extr #1 */
    CHECK(g_cpu.extr == 1);
    CHECK(g_cpu.ext_count == 1);
}
static void test_atomic_defers_without_addressing_change(void) {
    RUN_AT(0x0100, 0xD1, 0x00);  /* atomic #1 */
    CHECK(g_cpu.ext_kind == EXT_NONE && g_cpu.extr == 0);
    CHECK(g_cpu.ext_count == 1);
}
static void test_nested_ext_reloads_atomic_window_before_pending_irq(void) {
    LOAD_AT(0x0100,
            0xD1, 0x00,             /* atomic #1 */
            0xDC, 0x57,             /* extp r7, #2 */
            0xF2, 0xF4, 0x10, 0x00, /* mov r4,0x0010 */
            0xF2, 0xF5, 0x12, 0x00, /* mov r5,0x0012 */
            0xCC, 0x00);            /* nop */
    SET_GPR(7, 0x0010);
    SET_MEM16((0x10u << 14) | 0x0010, 0x1111);
    SET_MEM16((0x10u << 14) | 0x0012, 0x2222);
    SET_PSW(1u << 11);

    STEP();
    g_pend_valid = 1;
    g_pend_trap = 0x20;
    g_pend_ic = 0xFF6C;
    g_pend_ilvl = 1;

    STEP();
    CHECK(g_cpu.interrupts_delivered == 0 && g_cpu.ext_count == 2);
    STEP();
    CHECK(g_cpu.interrupts_delivered == 0 && GPR(4) == 0x1111);
    STEP();
    CHECK(g_cpu.interrupts_delivered == 0 && GPR(5) == 0x2222);
    STEP();
    CHECK(g_cpu.interrupts_delivered == 1);
}
static void test_mulu_unsigned(void) {
    SET_GPR(0, 0x1000); SET_GPR(1, 0x0010);
    RUN(0x1B, 0x01);             /* mulu r0, r1 -> 0x10000 */
    CHECK(MEM16(MDL) == 0x0000);
    CHECK(MEM16(MDH) == 0x0001);
    CHECK_FLAG(F_V, 1); CHECK_FLAG(F_C, 0);
}
static void test_mul_signed(void) {
    SET_GPR(0, 0xFFFF); SET_GPR(1, 0x0002);  /* -1 * 2 = -2 */
    RUN(0x0B, 0x01);             /* mul r0, r1 */
    uint32_t result = ((uint32_t)MEM16(MDH) << 16) | MEM16(MDL);
    CHECK(result == 0xFFFFFFFE);
    CHECK_FLAG(F_N, 1); CHECK_FLAG(F_V, 0);
}
static void test_mul_small_fits_word(void) {
    SET_GPR(0, 3); SET_GPR(1, 4);
    RUN(0x1B, 0x01);             /* mulu */
    CHECK(MEM16(MDL) == 12);
    CHECK(MEM16(MDH) == 0);
    CHECK_FLAG(F_V, 0);
}
static void test_divu_16_by_16(void) {
    SET_MEM16(MDL, 100);
    SET_GPR(0, 7);
    RUN(0x5B, 0x00);             /* divu r0 */
    CHECK(MEM16(MDL) == 14);
    CHECK(MEM16(MDH) == 2);
}
static void test_div_signed(void) {
    SET_MEM16(MDL, (-20) & 0xFFFF);
    SET_GPR(0, 3);
    RUN(0x4B, 0x00);             /* div r0 */
    CHECK(MEM16(MDL) == ((-6) & 0xFFFF));
    CHECK(MEM16(MDH) == ((-2) & 0xFFFF));
}
static void test_div_by_zero_sets_v(void) {
    SET_MEM16(MDL, 100);
    SET_GPR(0, 0);
    RUN(0x5B, 0x00);
    CHECK_FLAG(F_V, 1);
}
static void test_divlu_32_by_16(void) {
    SET_MEM16(MDH, 0x0001);
    SET_MEM16(MDL, 0x0000);
    SET_GPR(0, 0x0100);
    RUN(0x7B, 0x00);             /* divlu r0 */
    CHECK(MEM16(MDL) == 0x0100);
    CHECK(MEM16(MDH) == 0x0000);
}
static void test_nop(void) {
    RUN_AT(0x0100, 0xCC, 0x00);
    CHECK(IP() == 0x0102);
}
static void test_einit_decodes(void) {
    RUN_AT(0x0100, 0xB5, 0x4A, 0xB5, 0xB5);
    CHECK(IP() == 0x0104);
}
static void test_srst_performs_full_reset(void) {
    SET_PSW(0x00FF);
    g_cpu.csp = 0x12;
    RUN_AT(0x0100, 0xB7, 0x48, 0xB7, 0xB7);
    CHECK(PSW() == 0x0000);
    CHECK(PC() == 0x000000);
}

static const test_entry_t TESTS[] = {
    {"dpp_identity_mapping", test_dpp_identity_mapping},
    {"dpp_remap", test_dpp_remap},
    {"exts_segment_override", test_exts_segment_override},
    {"extp_page_override", test_extp_page_override},
    {"ext_window_count_two", test_ext_window_count_two},
    {"extr_sets_esfr_window", test_extr_sets_esfr_window},
    {"atomic_defers_without_addressing_change", test_atomic_defers_without_addressing_change},
    {"nested_ext_reloads_atomic_window_before_pending_irq", test_nested_ext_reloads_atomic_window_before_pending_irq},
    {"mulu_unsigned", test_mulu_unsigned},
    {"mul_signed", test_mul_signed},
    {"mul_small_fits_word", test_mul_small_fits_word},
    {"divu_16_by_16", test_divu_16_by_16},
    {"div_signed", test_div_signed},
    {"div_by_zero_sets_v", test_div_by_zero_sets_v},
    {"divlu_32_by_16", test_divlu_32_by_16},
    {"nop", test_nop},
    {"einit_decodes", test_einit_decodes},
    {"srst_performs_full_reset", test_srst_performs_full_reset},
};

int main(void) { return run_all(TESTS, (int)(sizeof(TESTS)/sizeof(TESTS[0]))); }
