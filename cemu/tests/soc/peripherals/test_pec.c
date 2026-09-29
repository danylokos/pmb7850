/* C166S PEC short/long-transfer behavior and CPU interrupt integration. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "soc.h"
#include "synth.h"
#include "cpu.h"

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define S0RIC    0xFF6Eu
#define EOPIC    0xF180u
#define PECISNC  0xFFA8u
#define PECC2    0xFEC4u
#define PECC4    0xFEC8u
#define PECXC2   0xFEF2u
#define PECSN2   0xFED4u
#define PECSN4   0xFED8u
#define SRCP2    0xFCE8u
#define DSTP2    0xFCEAu
#define SRCP4    0xFCF0u
#define DSTP4    0xFCF2u
#define SSC0TB   0xF0B0u
#define SSC0RB   0xF0B2u
#define SSC0TIC  0xFF72u
#define SSC0RIC  0xFF74u
#define SSC0CON  0xFFB2u

#define PECC_EOPINT (1u << 14)
#define PECC_PT     (1u << 15)
#define PECC_CL     (1u << 11)
#define PECC_INC_DST 0x0200u
#define PECC_INC_SRC 0x0400u
#define PECC_INC_RSV 0x0600u
#define PECC_BWT    0x0100u
#define SSC_CON_LB  (1u << 7)
#define SSC_CON_MS  (1u << 14)
#define SSC_CON_EN  (1u << 15)

static uint8_t *make_flash(size_t len) {
    uint8_t *f = calloc(len, 1);
    memset(f, 0xFF, len);
    f[0] = 0xFA; f[1] = 0x80; f[2] = 0xC4; f[3] = 0x2F;
    return f;
}

static soc_t S;
static uint8_t *FL;
#define FLEN (8u * 1024 * 1024)

static void setup(void) {
    if (!FL) FL = make_flash(FLEN);
    device_config_t cfg = *cemu_device_by_name("c55");
    cemu_soc_init(&S, FL, FLEN, &cfg, synth_defaults(), 0);
}

static int service_pec(uint32_t ic_addr, int ilvl) {
    int token = cemu_interrupt_subsystem_source_token(&S.interrupts, ic_addr);
    if (token < 0) return 0;
    interrupt_source_t *source = &S.interrupts.sources[token];
    interrupt_request_t request = {
        .source_token = (uint32_t)token,
        .ic_addr = source->addr,
        .trap = source->trap,
        .ilvl = ilvl,
    };
    return cemu_interrupt_subsystem_begin_service(&S.interrupts, &request) ==
           INTERRUPT_SERVICE_PEC;
}

static uint16_t irq_value(int ilvl, int xglvl) {
    uint16_t v = (uint16_t)(XIC_IR_BIT | XIC_IE_BIT | ((uint16_t)ilvl << 2) | (xglvl & 0x3));
    if (xglvl & 0x4) v |= 0x0100;
    return v;
}

static void arm_ch2_word(uint16_t pecc) {
    bus_write16(&S.bus, PECSN2, 0x0000);
    bus_write16(&S.bus, SRCP2, 0xE100);
    bus_write16(&S.bus, DSTP2, 0xE200);
    bus_write16(&S.bus, PECC2, pecc);
    bus_write16(&S.bus, S0RIC, irq_value(14, 2));
}

static void test_word_transfer_decrements_count_and_clears_ir(void) {
    setup();
    arm_ch2_word(0x0002);
    bus_write16(&S.bus, 0xE100, 0xBEEF);
    CHECK(service_pec(S0RIC, 14) == 1);
    CHECK(bus_read16(&S.bus, 0xE200) == 0xBEEF);
    CHECK((bus_read16(&S.bus, S0RIC) & XIC_IR_BIT) == 0);
    CHECK((bus_read16(&S.bus, PECC2) & 0x00FF) == 1);
    cemu_soc_free(&S);
}

static void test_byte_transfer_destination_increment(void) {
    setup();
    bus_write16(&S.bus, PECSN4, 0x0000);
    bus_write16(&S.bus, SRCP4, 0xE110);
    bus_write16(&S.bus, DSTP4, 0xE210);
    bus_write16(&S.bus, PECC4, PECC_BWT | PECC_INC_DST | 0x0002);
    bus_write8(&S.bus, 0xE110, 0xAB);
    bus_write16(&S.bus, S0RIC, irq_value(15, 0));
    CHECK(service_pec(S0RIC, 15) == 1);
    CHECK(bus_read8(&S.bus, 0xE210) == 0xAB);
    CHECK(bus_read16(&S.bus, DSTP4) == 0xE211);
    CHECK((bus_read16(&S.bus, PECC4) & 0x00FF) == 1);
    cemu_soc_free(&S);
}

static void test_reserved_increment_combination_behaves_like_source_increment(void) {
    setup();
    bus_write16(&S.bus, PECSN2, 0x0000);
    bus_write16(&S.bus, SRCP2, 0xE120);
    bus_write16(&S.bus, DSTP2, 0xE220);
    bus_write16(&S.bus, PECC2, PECC_BWT | PECC_INC_RSV | 0x0002);
    bus_write8(&S.bus, 0xE120, 0x44);
    bus_write16(&S.bus, S0RIC, irq_value(14, 2));
    CHECK(service_pec(S0RIC, 14) == 1);
    CHECK(bus_read8(&S.bus, 0xE220) == 0x44);
    CHECK(bus_read16(&S.bus, SRCP2) == 0xE121);
    CHECK(bus_read16(&S.bus, DSTP2) == 0xE220);
    cemu_soc_free(&S);
}

static void test_continuous_count_ff_is_not_decremented(void) {
    setup();
    arm_ch2_word(0x00FF);
    bus_write16(&S.bus, 0xE100, 0x1234);
    CHECK(service_pec(S0RIC, 14) == 1);
    CHECK(bus_read16(&S.bus, 0xE200) == 0x1234);
    CHECK((bus_read16(&S.bus, PECC2) & 0x00FF) == 0x00FF);
    CHECK((bus_read16(&S.bus, S0RIC) & XIC_IR_BIT) == 0);
    cemu_soc_free(&S);
}

static void test_count_one_eopint_zero_leaves_source_request_for_isr(void) {
    setup();
    arm_ch2_word(0x0001);
    bus_write16(&S.bus, 0xE100, 0x2222);
    CHECK(service_pec(S0RIC, 14) == 1);
    CHECK(bus_read16(&S.bus, 0xE200) == 0x2222);
    CHECK((bus_read16(&S.bus, PECC2) & 0x00FF) == 0);
    CHECK((bus_read16(&S.bus, S0RIC) & XIC_IR_BIT) != 0);
    CHECK(service_pec(S0RIC, 14) == 0);
    cemu_soc_free(&S);
}

static void test_count_one_eopint_one_sets_subnode_and_eopic(void) {
    setup();
    arm_ch2_word((uint16_t)(PECC_EOPINT | 0x0001));
    bus_write16(&S.bus, PECISNC, (uint16_t)(1u << 4));  /* C2IE */
    bus_write16(&S.bus, EOPIC, (uint16_t)(XIC_IE_BIT | (13u << 2) | 0x3));
    bus_write16(&S.bus, 0xE100, 0x3333);
    CHECK(service_pec(S0RIC, 14) == 1);
    CHECK((bus_read16(&S.bus, S0RIC) & XIC_IR_BIT) == 0);
    CHECK((bus_read16(&S.bus, PECISNC) & (1u << 5)) != 0);  /* C2IR */
    CHECK((bus_read16(&S.bus, EOPIC) & XIC_IR_BIT) != 0);
    interrupt_request_t request;
    CHECK(cemu_interrupt_subsystem_pending(&S.interrupts, &request) == 1);
    CHECK(request.trap == 0x4C && request.ic_addr == EOPIC &&
          request.ilvl == 13);
    cemu_soc_free(&S);
}

static void test_subnode_enable_with_pending_ir_sets_eopic(void) {
    setup();
    bus_write16(&S.bus, PECISNC, (uint16_t)(1u << 5));  /* C2IR only */
    CHECK((bus_read16(&S.bus, EOPIC) & XIC_IR_BIT) == 0);
    bus_write16(&S.bus, PECISNC, (uint16_t)((1u << 5) | (1u << 4)));
    CHECK((bus_read16(&S.bus, EOPIC) & XIC_IR_BIT) != 0);
    cemu_soc_free(&S);
}

static void test_count_zero_and_unsupported_modes_fall_through(void) {
    setup();
    arm_ch2_word(0x0000);
    CHECK(service_pec(S0RIC, 14) == 0);
    bus_write16(&S.bus, PECC2, (uint16_t)(PECC_PT | 0x0002));
    CHECK(service_pec(S0RIC, 14) == 0);
    bus_write16(&S.bus, PECC2, PECC_PT);
    bus_write16(&S.bus, PECXC2, 0x0000);
    CHECK(service_pec(S0RIC, 14) == 0);
    bus_write16(&S.bus, PECC2, (uint16_t)(PECC_CL | 0x0002));
    CHECK(service_pec(S0RIC, 14) == 0);
    cemu_soc_free(&S);
}

static void test_long_byte_transfer_uses_pecxc_and_source_increment(void) {
    setup();
    bus_write16(&S.bus, PECSN2, 0x0000);
    bus_write16(&S.bus, SRCP2, 0xE140);
    bus_write16(&S.bus, DSTP2, 0xE240);
    bus_write16(&S.bus, PECC2,
                (uint16_t)(PECC_PT | PECC_BWT | PECC_INC_SRC));
    bus_write16(&S.bus, PECXC2, 0x0002);
    bus_write8(&S.bus, 0xE140, 0x6A);
    bus_write16(&S.bus, S0RIC, irq_value(14, 2));

    CHECK(service_pec(S0RIC, 14) == 1);
    CHECK(bus_read8(&S.bus, 0xE240) == 0x6A);
    CHECK(bus_read16(&S.bus, SRCP2) == 0xE141);
    CHECK(bus_read16(&S.bus, PECXC2) == 0x0001);
    CHECK((bus_read16(&S.bus, PECC2) & 0x00FF) == 0);
    CHECK((bus_read16(&S.bus, S0RIC) & XIC_IR_BIT) == 0);
    cemu_soc_free(&S);
}

static void test_long_final_transfer_follows_eopint(void) {
    setup();
    bus_write16(&S.bus, PECSN2, 0x0000);
    bus_write16(&S.bus, SRCP2, 0xE150);
    bus_write16(&S.bus, DSTP2, 0xE250);
    bus_write16(&S.bus, PECC2, PECC_PT);
    bus_write16(&S.bus, PECXC2, 0x0001);
    bus_write16(&S.bus, 0xE150, 0x7788);
    bus_write16(&S.bus, S0RIC, irq_value(14, 2));
    CHECK(service_pec(S0RIC, 14) == 1);
    CHECK(bus_read16(&S.bus, PECXC2) == 0);
    CHECK((bus_read16(&S.bus, S0RIC) & XIC_IR_BIT) != 0);

    bus_write16(&S.bus, PECC2, (uint16_t)(PECC_PT | PECC_EOPINT));
    bus_write16(&S.bus, PECXC2, 0x0001);
    bus_write16(&S.bus, PECISNC, (uint16_t)(1u << 4));
    bus_write16(&S.bus, EOPIC,
                (uint16_t)(XIC_IE_BIT | (13u << 2) | 0x3));
    bus_write16(&S.bus, S0RIC, irq_value(14, 2));
    CHECK(service_pec(S0RIC, 14) == 1);
    CHECK(bus_read16(&S.bus, PECXC2) == 0);
    CHECK((bus_read16(&S.bus, S0RIC) & XIC_IR_BIT) == 0);
    CHECK((bus_read16(&S.bus, PECISNC) & (1u << 5)) != 0);
    CHECK((bus_read16(&S.bus, EOPIC) & XIC_IR_BIT) != 0);
    cemu_soc_free(&S);
}

static void test_m55_style_long_ssc_transfer_runs_139_bytes(void) {
    setup();
    bus_write16(&S.bus, SSC0CON, SSC_CON_EN | SSC_CON_MS | 0x0037);
    bus_write16(&S.bus, PECSN2, 0x0004);
    bus_write16(&S.bus, SRCP2, 0x4746);
    bus_write16(&S.bus, DSTP2, SSC0TB);
    bus_write16(&S.bus, PECC2, 0xC500);
    bus_write16(&S.bus, PECXC2, 0x008B);
    for (unsigned i = 0; i < 139; i++)
        bus_write8(&S.bus, 0x044746u + i, (uint8_t)i);
    bus_write16(&S.bus, SSC0TIC, irq_value(14, 2));

    for (unsigned i = 0; i < 139; i++) {
        CHECK(service_pec(SSC0TIC, 14) == 1);
        CHECK(bus_read16(&S.bus, PECXC2) == 138u - i);
        cemu_soc_tick(&S, 16);
    }
    CHECK(bus_read16(&S.bus, SRCP2) == 0x47D1);
    CHECK(bus_read16(&S.bus, PECXC2) == 0);
    CHECK((bus_read16(&S.bus, PECC2) & 0x00FF) == 0);
    CHECK((bus_read16(&S.bus, SSC0TIC) & XIC_IR_BIT) != 0);
    cemu_soc_free(&S);
}

static void test_pec_write_to_ssc0tb_uses_normal_side_effects(void) {
    setup();
    bus_write16(&S.bus, SSC0CON, SSC_CON_EN | SSC_CON_MS | SSC_CON_LB | 0x0007);
    bus_write16(&S.bus, PECSN2, 0x0000);
    bus_write16(&S.bus, SRCP2, 0xE130);
    bus_write16(&S.bus, DSTP2, SSC0TB);
    bus_write16(&S.bus, PECC2, PECC_BWT | 0x0002);
    bus_write8(&S.bus, 0xE130, 0x5A);
    bus_write16(&S.bus, S0RIC, irq_value(14, 2));
    CHECK(service_pec(S0RIC, 14) == 1);
    cemu_soc_tick(&S, 16);
    CHECK(bus_read16(&S.bus, SSC0RB) == 0x005A);
    CHECK((bus_read16(&S.bus, SSC0TIC) & XIC_IR_BIT) != 0);
    CHECK((bus_read16(&S.bus, SSC0RIC) & XIC_IR_BIT) != 0);
    cemu_soc_free(&S);
}

static void write_nop(uint32_t addr) {
    cemu_memory_controller_poke8(&S.memory, addr, 0xCC);
    cemu_memory_controller_poke8(&S.memory, addr + 1, 0x00);
}

static void test_cpu_ien_gates_pec_transfer(void) {
    setup();
    cpu_t c;
    cemu_cpu_init(&c, &S.bus);
    cemu_soc_attach_cpu(&S, &c);
    write_nop(0x0100);
    arm_ch2_word(0x0002);
    bus_write16(&S.bus, 0xE100, 0x4444);
    c.csp = 0; c.ip = 0x0100;
    cemu_cpu_set_psw(&c, 0x0000);
    CHECK(cemu_cpu_step(&c) == STEP_OK);
    CHECK(bus_read16(&S.bus, 0xE200) == 0x0000);
    CHECK((bus_read16(&S.bus, PECC2) & 0x00FF) == 2);

    c.csp = 0; c.ip = 0x0100;
    cemu_cpu_set_psw(&c, 1u << 11);
    CHECK(cemu_cpu_step(&c) == STEP_OK);
    CHECK(bus_read16(&S.bus, 0xE200) == 0x4444);
    CHECK(c.interrupts_delivered == 0);
    CHECK((bus_read16(&S.bus, PECC2) & 0x00FF) == 1);
    cemu_soc_free(&S);
}

static void test_finite_progress_predicate_respects_ien_and_priority(void) {
    setup();
    cpu_t c;
    cemu_cpu_init(&c, &S.bus);
    cemu_soc_attach_cpu(&S, &c);
    arm_ch2_word(0x0002);

    cemu_cpu_set_psw(&c, 1u << 11);
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 1);
    cemu_cpu_set_psw(&c, 0);
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 0);
    cemu_cpu_set_psw(&c, (uint16_t)((1u << 11) | (14u << 12)));
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 0);
    cemu_cpu_set_psw(&c, (uint16_t)((1u << 11) | (13u << 12)));
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 1);
    cemu_soc_free(&S);
}

static void test_finite_progress_predicate_rejects_nonfinite_modes(void) {
    setup();
    cpu_t c;
    cemu_cpu_init(&c, &S.bus);
    cemu_soc_attach_cpu(&S, &c);
    cemu_cpu_set_psw(&c, 1u << 11);
    arm_ch2_word(0x0002);
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 1);

    bus_write16(&S.bus, PECC2, 0x0000);
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 0);
    bus_write16(&S.bus, PECC2, 0x00FF);
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 0);
    bus_write16(&S.bus, PECC2, (uint16_t)(PECC_CL | 0x0002));
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 0);
    bus_write16(&S.bus, PECC2, (uint16_t)(PECC_PT | 0x0002));
    bus_write16(&S.bus, PECXC2, 0x0002);
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 0);
    bus_write16(&S.bus, PECC2, PECC_PT);
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 1);
    cemu_soc_free(&S);
}

static void test_finite_progress_predicate_accepts_pending_or_scheduled_source(void) {
    setup();
    cpu_t c;
    cemu_cpu_init(&c, &S.bus);
    cemu_soc_attach_cpu(&S, &c);
    cemu_cpu_set_psw(&c, 1u << 11);
    arm_ch2_word(0x0002);
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 1);
    bus_write16(&S.bus, S0RIC, irq_value(14, 2) & (uint16_t)~XIC_IR_BIT);
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 0);

    bus_write16(&S.bus, SSC0CON, SSC_CON_EN | SSC_CON_MS | 0x0007);
    bus_write16(&S.bus, SSC0TB, 0x0011);
    bus_write16(&S.bus, SSC0TB, 0x0022);
    bus_write16(&S.bus, SSC0TIC, irq_value(14, 2) & (uint16_t)~XIC_IR_BIT);
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 1);
    bus_write16(&S.bus, SSC0TIC,
                (irq_value(14, 2) & (uint16_t)~(XIC_IR_BIT | XIC_IE_BIT)));
    CHECK(cemu_soc_finite_pec_will_progress(&S) == 0);
    cemu_soc_free(&S);
}

static void test_count_zero_falls_through_to_normal_cpu_irq(void) {
    setup();
    cpu_t c;
    cemu_cpu_init(&c, &S.bus);
    cemu_soc_attach_cpu(&S, &c);
    write_nop(0x0100);
    write_nop(0x00AC);  /* S0RIC trap 0x2b vector */
    arm_ch2_word(0x0000);
    c.csp = 0; c.ip = 0x0100;
    cemu_cpu_set_psw(&c, 1u << 11);
    CHECK(cemu_cpu_step(&c) == STEP_OK);
    CHECK(c.interrupts_delivered == 1);
    CHECK(cpu_pc(&c) == 0x0000AE);
    CHECK((bus_read16(&S.bus, S0RIC) & XIC_IR_BIT) == 0);
    cemu_soc_free(&S);
}

static void test_final_transfer_retains_ir_then_next_step_enters_isr(void) {
    setup();
    cpu_t c;
    cemu_cpu_init(&c, &S.bus);
    cemu_soc_attach_cpu(&S, &c);
    write_nop(0x0100);
    write_nop(0x0102);
    write_nop(0x00AC);  /* S0RIC trap 0x2b vector */
    arm_ch2_word(0x0001);
    bus_write16(&S.bus, 0xE100, 0x5555);
    c.csp = 0; c.ip = 0x0100;
    cemu_cpu_set_psw(&c, 1u << 11);

    CHECK(cemu_cpu_step(&c) == STEP_OK);
    CHECK(bus_read16(&S.bus, 0xE200) == 0x5555);
    CHECK((bus_read16(&S.bus, PECC2) & 0x00FF) == 0);
    CHECK((bus_read16(&S.bus, S0RIC) & XIC_IR_BIT) != 0);
    CHECK(c.interrupts_delivered == 0);
    CHECK(cpu_pc(&c) == 0x000102);

    CHECK(cemu_cpu_step(&c) == STEP_OK);
    CHECK(c.interrupts_delivered == 1);
    CHECK(cpu_pc(&c) == 0x0000AE);
    CHECK((bus_read16(&S.bus, S0RIC) & XIC_IR_BIT) == 0);
    cemu_soc_free(&S);
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
    {"word_transfer_decrements_count_and_clears_ir", test_word_transfer_decrements_count_and_clears_ir},
    {"byte_transfer_destination_increment", test_byte_transfer_destination_increment},
    {"reserved_increment_combination_behaves_like_source_increment", test_reserved_increment_combination_behaves_like_source_increment},
    {"continuous_count_ff_is_not_decremented", test_continuous_count_ff_is_not_decremented},
    {"count_one_eopint_zero_leaves_source_request_for_isr", test_count_one_eopint_zero_leaves_source_request_for_isr},
    {"count_one_eopint_one_sets_subnode_and_eopic", test_count_one_eopint_one_sets_subnode_and_eopic},
    {"subnode_enable_with_pending_ir_sets_eopic", test_subnode_enable_with_pending_ir_sets_eopic},
    {"count_zero_and_unsupported_modes_fall_through", test_count_zero_and_unsupported_modes_fall_through},
    {"long_byte_transfer_uses_pecxc_and_source_increment", test_long_byte_transfer_uses_pecxc_and_source_increment},
    {"long_final_transfer_follows_eopint", test_long_final_transfer_follows_eopint},
    {"m55_style_long_ssc_transfer_runs_139_bytes", test_m55_style_long_ssc_transfer_runs_139_bytes},
    {"pec_write_to_ssc0tb_uses_normal_side_effects", test_pec_write_to_ssc0tb_uses_normal_side_effects},
    {"cpu_ien_gates_pec_transfer", test_cpu_ien_gates_pec_transfer},
    {"finite_progress_predicate_respects_ien_and_priority",
     test_finite_progress_predicate_respects_ien_and_priority},
    {"finite_progress_predicate_rejects_nonfinite_modes",
     test_finite_progress_predicate_rejects_nonfinite_modes},
    {"finite_progress_predicate_accepts_pending_or_scheduled_source",
     test_finite_progress_predicate_accepts_pending_or_scheduled_source},
    {"count_zero_falls_through_to_normal_cpu_irq", test_count_zero_falls_through_to_normal_cpu_irq},
    {"final_transfer_retains_ir_then_next_step_enters_isr", test_final_transfer_retains_ir_then_next_step_enters_isr},
};

int main(void) {
    FL = make_flash(FLEN);
    int n = (int)(sizeof(TESTS) / sizeof(TESTS[0]));
    for (int i = 0; i < n; i++) {
        g_fail = 0;
        TESTS[i].fn();
        g_total_run++;
        if (g_fail) { g_total_fail++; printf("[FAIL] %s (%d)\n", TESTS[i].name, g_fail); }
    }
    printf("\n%d tests, %d failed\n", g_total_run, g_total_fail);
    return g_total_fail ? 1 : 0;
}
