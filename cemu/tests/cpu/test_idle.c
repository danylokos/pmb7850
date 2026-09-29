/* IDLE / PWRDN and interrupt wake — ported from emu/tests/cpu/test_idle.py.
 * The Python protected_ops counter isn't modeled in the C port (not machine
 * state / not in the snapshot), so those two assertions are dropped; the
 * behavioral ones (idle latch, PC-frozen spin, IRQ wake) are kept. */
#include "test_harness.h"

/* IDLE = 0x87877887 -> 87 78 87 87. PWRDN = 0x97976897 -> 97 68 97 97. */
static int atomic_idle_pec_checks;
static interrupt_service_t observe_atomic_idle_pec(
    void *ctx, const interrupt_request_t *request) {
    (void)ctx; (void)request;
    atomic_idle_pec_checks++;
    return INTERRUPT_SERVICE_CPU;
}

static void test_idle_sets_idle_state(void) {
    RUN_AT(0x0100, 0x87, 0x78, 0x87, 0x87);
    CHECK(g_cpu.idle == 1);
    CHECK(g_cpu.halted == 0);
}
static void test_idle_step_parks_without_advancing_pc(void) {
    RUN_AT(0x0100, 0x87, 0x78, 0x87, 0x87);
    uint32_t pc_after = PC();
    uint64_t ic0 = g_cpu.icount;
    STEP();
    CHECK(g_cpu.idle == 1);
    CHECK(PC() == pc_after);
    CHECK(g_cpu.icount == ic0 + 1);
}
static void test_idle_wakes_on_pending_interrupt(void) {
    SET_PSW(1u << 11);           /* IEN set, CPU level 0 */
    RUN_AT(0x0100, 0x87, 0x78, 0x87, 0x87);
    CHECK(g_cpu.idle == 1);
    SET_MEM16(0xFF6C, (1 << 7) | (1 << 6) | (1 << 2));  /* IR, IE, ILVL=1 */
    g_pend_valid = 1; g_pend_trap = 0x2A; g_pend_ic = 0xFF6C; g_pend_ilvl = 1;
    STEP();
    CHECK(g_cpu.idle == 0);
    CHECK(g_cpu.interrupts_delivered == 1);
    CHECK(0xA8 <= IP() && IP() <= 0xA8 + 4);
}
static void test_pwrdn_sets_idle(void) {
    RUN_AT(0x0100, 0x97, 0x68, 0x97, 0x97);
    CHECK(g_cpu.idle == 1);
}
static void test_idle_exits_when_ien_clear(void) {
    SET_PSW(0x0000);             /* IEN clear */
    SET_MEM16(0xFF6C, (1 << 6) | (1 << 2));    /* IE, ILVL=1 */
    RUN_AT(0x0100, 0x87, 0x78, 0x87, 0x87);
    g_pend_valid = 1; g_pend_trap = 0x2A; g_pend_ic = 0xFF6C; g_pend_ilvl = 1;
    STEP();
    CHECK(g_cpu.idle == 0);
    CHECK(g_cpu.interrupts_delivered == 0);
    CHECK(PC() != 0x0104);
}

static void test_idle_exits_when_priority_too_low(void) {
    SET_PSW((1u << 11) | (15u << 12));
    SET_MEM16(0xFF6C, (1 << 6) | (1 << 2));    /* IE, ILVL=1 */
    RUN_AT(0x0100, 0x87, 0x78, 0x87, 0x87);
    g_pend_valid = 1; g_pend_trap = 0x2A; g_pend_ic = 0xFF6C; g_pend_ilvl = 1;
    STEP();
    CHECK(g_cpu.idle == 0);
    CHECK(g_cpu.interrupts_delivered == 0);
    CHECK(PC() != 0x0104);
}

/* M55 v91 uses this exact ISR-return shape.  ATOMIC #3 covers RETI, the
 * returned-to IDLE, and the following RETS.  A request during IDLE must wake
 * the CPU without being serviced until RETS consumes the last protected slot. */
static void test_idle_wakes_inside_atomic_reti_window(void) {
    SET_MEM8(0x0100, 0x87); SET_MEM8(0x0101, 0x78);
    SET_MEM8(0x0102, 0x87); SET_MEM8(0x0103, 0x87);  /* idle */
    SET_MEM8(0x0104, 0xDB); SET_MEM8(0x0105, 0x00);  /* rets */
    SET_MEM8(0x0300, 0xCC); SET_MEM8(0x0301, 0x00);  /* nop */

    SET_MEM16(0xFE12, 0xFAFA);  /* RETI frame, then RETS frame */
    SET_MEM16(0xFAFA, 0x0100);  /* restored IP */
    SET_MEM16(0xFAFC, 0x0000);  /* restored CSP */
    SET_MEM16(0xFAFE, 0x0800);  /* restored PSW: IEN, ILVL=0 */
    SET_MEM16(0xFB00, 0x0300);  /* RETS destination IP */
    SET_MEM16(0xFB02, 0x0000);  /* RETS destination CSP */
    atomic_idle_pec_checks = 0;
    g_interrupt_port.begin_service = observe_atomic_idle_pec;
    LOAD_AT(0x0200,
            0xD1, 0x20,         /* atomic #3 */
            0xFB, 0x88);        /* reti */

    STEP();
    CHECK(g_cpu.ext_count == 3);
    STEP();
    CHECK(PC() == 0x0100 && g_cpu.ext_count == 2);
    STEP();
    CHECK(g_cpu.idle == 1 && PC() == 0x0104 && g_cpu.ext_count == 1);

    g_pend_valid = 1; g_pend_trap = 0x2A; g_pend_ic = 0xFF6C; g_pend_ilvl = 1;
    STEP();
    CHECK(g_cpu.idle == 0);
    CHECK(g_cpu.interrupts_delivered == 0);
    CHECK(atomic_idle_pec_checks == 0);
    CHECK(PC() == 0x0300);
    CHECK(g_cpu.ext_count == 0);
    CHECK(g_cpu.last_ran == 1);

    STEP();
    CHECK(atomic_idle_pec_checks == 1);
    CHECK(g_cpu.interrupts_delivered == 1);
}

static void test_idle_wake_preserves_ext_window_for_next_instruction(void) {
    SET_MEM16((0x05u << 16) | 0x1000, 0xDEAD);
    SET_PSW(1u << 11);
    LOAD_AT(0x0100,
            0xD7, 0x10, 0x05, 0x00,  /* exts #0x05, #2 */
            0x87, 0x78, 0x87, 0x87,  /* idle */
            0xF2, 0xF0, 0x00, 0x10,  /* mov r0,0x1000 */
            0xCC, 0x00);             /* nop */

    STEP();
    CHECK(g_cpu.ext_kind == EXT_SEG && g_cpu.ext_count == 2);
    STEP();
    CHECK(g_cpu.idle == 1 && PC() == 0x0108 && g_cpu.ext_count == 1);

    g_pend_valid = 1; g_pend_trap = 0x2A; g_pend_ic = 0xFF6C; g_pend_ilvl = 1;
    STEP();
    CHECK(g_cpu.idle == 0);
    CHECK(g_cpu.interrupts_delivered == 0);
    CHECK(GPR(0) == 0xDEAD);
    CHECK(g_cpu.ext_kind == EXT_NONE && g_cpu.ext_count == 0);

    STEP();
    CHECK(g_cpu.interrupts_delivered == 1);
}

static const test_entry_t TESTS[] = {
    {"idle_sets_idle_state", test_idle_sets_idle_state},
    {"idle_step_parks_without_advancing_pc", test_idle_step_parks_without_advancing_pc},
    {"idle_wakes_on_pending_interrupt", test_idle_wakes_on_pending_interrupt},
    {"pwrdn_sets_idle", test_pwrdn_sets_idle},
    {"idle_exits_when_ien_clear", test_idle_exits_when_ien_clear},
    {"idle_exits_when_priority_too_low", test_idle_exits_when_priority_too_low},
    {"idle_wakes_inside_atomic_reti_window", test_idle_wakes_inside_atomic_reti_window},
    {"idle_wake_preserves_ext_window_for_next_instruction", test_idle_wake_preserves_ext_window_for_next_instruction},
};

int main(void) { return run_all(TESTS, (int)(sizeof(TESTS)/sizeof(TESTS[0]))); }
