/* Experimental PMB7850 SIM controller and deterministic stub-card behavior. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "soc.h"
#include "sim.h"
#include "synth.h"

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define PECC3  0xFEC6u
#define PECSN3 0xFED6u
#define SRCP3  0xFCECu
#define DSTP3  0xFCEEu
#define PECC_BWT 0x0100u
#define PECC_INC_DST 0x0200u

static soc_t S;
static uint8_t *FL;
#define FLEN (8u * 1024 * 1024)

static void setup(void) {
    if (!FL) {
        FL = calloc(FLEN, 1);
        memset(FL, 0xFF, FLEN);
        FL[0] = 0xFA; FL[1] = 0x80; FL[2] = 0xC4; FL[3] = 0x2F;
    }
    device_config_t cfg = *cemu_device_by_name("c55");
    cemu_soc_init(&S, FL, FLEN, &cfg, synth_defaults(), 0);
}

static sim_state_t *sim_state(void) {
    return (sim_state_t *)S.sim_periph->state;
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

static void ready_stub_card(void) {
    cemu_sim_set_mode(S.sim_periph, &S, SIM_MODE_STUB);
    sim_state()->phase = SIM_PHASE_READY;
    sim_state()->due_tick = S.ticks;
}

static void test_disabled_mode_ignores_activation(void) {
    setup();
    CHECK(cemu_sim_get_mode(S.sim_periph) == SIM_MODE_NONE);
    bus_write16(&S.bus, SIM_CTRL, 1u << 13);
    cemu_soc_tick(&S, 512);
    CHECK(sim_state()->phase == SIM_PHASE_OFF);
    CHECK((bus_read16(&S.bus, SIM_STATUS_IC) & XIC_IR_BIT) == 0);
    CHECK((bus_read16(&S.bus, SIM_BYTE_IC) & XIC_IR_BIT) == 0);
    cemu_soc_free(&S);
}

static void test_activation_delivers_atr_in_order(void) {
    setup();
    cemu_sim_set_mode(S.sim_periph, &S, SIM_MODE_STUB);
    bus_write16(&S.bus, SIM_CTRL, 1u << 13);
    CHECK(sim_state()->phase == SIM_PHASE_INITIAL);

    cemu_soc_tick(&S, 256);
    CHECK(sim_state()->phase == SIM_PHASE_ATR);
    CHECK(bus_read16(&S.bus, SIM_STATUS) == 0x0002);
    CHECK(bus_read16(&S.bus, SIM_RX) == 0x0003);
    CHECK((bus_read16(&S.bus, SIM_STATUS_IC) & XIC_IR_BIT) != 0);

    bus_write16(&S.bus, SIM_STATUS_IC, 0);
    cemu_soc_tick(&S, 256);
    CHECK(bus_read8(&S.bus, SIM_RX) == 0x90);
    CHECK((bus_read16(&S.bus, SIM_BYTE_IC) & XIC_IR_BIT) != 0);
    bus_write16(&S.bus, SIM_BYTE_IC, 0);
    cemu_soc_tick(&S, 256);
    CHECK(bus_read8(&S.bus, SIM_RX) == 0x11);
    bus_write16(&S.bus, SIM_BYTE_IC, 0);
    cemu_soc_tick(&S, 256);
    CHECK(bus_read8(&S.bus, SIM_RX) == 0x00);
    cemu_soc_free(&S);
}

static void arm_pec3(uint16_t count, uint16_t src, uint16_t dst) {
    bus_write16(&S.bus, PECSN3, 0);
    bus_write16(&S.bus, SRCP3, src);
    bus_write16(&S.bus, DSTP3, dst);
    bus_write16(&S.bus, PECC3, (uint16_t)(PECC_BWT | count));
}

static void test_tx_pec_schedules_byte_request(void) {
    setup();
    ready_stub_card();
    arm_pec3(1, 0xE100, SIM_TX);
    bus_write16(&S.bus, SIM_BYTE_IC, 0x007B); /* IE, ILVL=14, GLVL=3 */
    CHECK(cemu_soc_idle_wake_possible(&S));
    cemu_soc_tick(&S, 1);
    CHECK((bus_read16(&S.bus, SIM_BYTE_IC) & XIC_IR_BIT) != 0);
    cemu_soc_free(&S);
}

static void issue_select_df_gsm(void) {
    static const uint8_t select_header[] = {0xA0, 0xA4, 0x00, 0x00, 0x02};
    for (size_t i = 0; i < sizeof select_header; i++)
        bus_write8(&S.bus, SIM_TX, select_header[i]);
    arm_pec3(2, 0xE100, SIM_TX);
    cemu_soc_tick(&S, 256);
    bus_write8(&S.bus, SIM_TX, 0x7F);
    bus_write8(&S.bus, SIM_TX, 0x20);
    bus_write16(&S.bus, PECC3, PECC_BWT);
    cemu_soc_tick(&S, 256);
}

static void issue_select_id(uint16_t id) {
    static const uint8_t select_header[] = {0xA0, 0xA4, 0x00, 0x00, 0x02};
    for (size_t i = 0; i < sizeof select_header; i++)
        bus_write8(&S.bus, SIM_TX, select_header[i]);
    arm_pec3(2, 0xE100, SIM_TX);
    cemu_soc_tick(&S, 256);
    bus_write8(&S.bus, SIM_TX, (uint8_t)(id >> 8));
    bus_write8(&S.bus, SIM_TX, (uint8_t)id);
    bus_write16(&S.bus, PECC3, PECC_BWT);
    cemu_soc_tick(&S, 256);
}

static void select_ef_under_gsm(uint16_t id) {
    issue_select_df_gsm();
    bus_write16(&S.bus, SIM_STATUS_IC, 0x0068);
    issue_select_id(id);
}

static void test_procedure_and_status_are_controller_internal(void) {
    setup();
    ready_stub_card();
    issue_select_df_gsm();

    CHECK(sim_state()->output_len == 0);
    CHECK(!sim_state()->rx_loaded);
    CHECK(!sim_state()->completion_pending);
    CHECK(bus_read16(&S.bus, 0xEF60) == 0x009F);
    CHECK(bus_read16(&S.bus, 0xEF62) == 0x000F);
    CHECK(bus_read16(&S.bus, SIM_STATUS) == 0x0008);
    CHECK((bus_read16(&S.bus, SIM_STATUS_IC) & XIC_IR_BIT) != 0);
    CHECK((bus_read16(&S.bus, SIM_EVENT_IC) & XIC_IR_BIT) == 0);
    cemu_soc_free(&S);
}

static void test_get_response_data_waits_for_rx_pec(void) {
    static const uint8_t get_response[] = {0xA0, 0xC0, 0x00, 0x00, 0x0F};
    setup();
    ready_stub_card();
    issue_select_df_gsm();
    bus_write16(&S.bus, SIM_STATUS_IC, 0x0068);

    for (size_t i = 0; i < sizeof get_response; i++)
        bus_write8(&S.bus, SIM_TX, get_response[i]);
    CHECK(sim_state()->output_len == 16);
    CHECK(sim_state()->completion_pending);

    arm_pec3(15, SIM_RX, 0xE200);
    bus_write16(&S.bus, PECC3, PECC_BWT | PECC_INC_DST | 15);
    cemu_soc_tick(&S, 256);
    CHECK(sim_state()->output_len == 15);
    CHECK(!sim_state()->rx_loaded);

    bus_write16(&S.bus, SIM_BYTE_IC, 0x007B);
    for (int i = 0; i < 15; i++) {
        cemu_soc_tick(&S, 256);
        CHECK(sim_state()->rx_loaded);
        CHECK(service_pec(SIM_BYTE_IC, 14) == 1);
    }
    CHECK(sim_state()->output_len == 0);
    CHECK(bus_read8(&S.bus, 0xE206) == 0x02);
    CHECK((bus_read16(&S.bus, SIM_STATUS_IC) & XIC_IR_BIT) == 0);
    CHECK((bus_read16(&S.bus, SIM_BYTE_IC) & XIC_IR_BIT) != 0);

    bus_write16(&S.bus, SIM_BYTE_IC, 0x007B);
    cemu_soc_tick(&S, 256);
    CHECK(bus_read16(&S.bus, 0xEF60) == 0x0090);
    CHECK(bus_read16(&S.bus, 0xEF62) == 0x0000);
    CHECK((bus_read16(&S.bus, SIM_STATUS_IC) & XIC_IR_BIT) != 0);
    cemu_soc_free(&S);
}

static void test_status_returns_df_gsm_profile_through_rx_pec(void) {
    static const uint8_t status[] = {0xA0, 0xF2, 0x00, 0x00, 0x14};
    setup();
    ready_stub_card();
    issue_select_df_gsm();
    bus_write16(&S.bus, SIM_STATUS_IC, 0x0068);

    for (size_t i = 0; i < sizeof status; i++)
        bus_write8(&S.bus, SIM_TX, status[i]);
    CHECK(sim_state()->output_len == 21);
    CHECK(sim_state()->completion_pending);

    arm_pec3(20, SIM_RX, 0xE300);
    bus_write16(&S.bus, PECC3, PECC_BWT | PECC_INC_DST | 20);
    cemu_soc_tick(&S, 256);
    CHECK(sim_state()->output_len == 20);
    CHECK(!sim_state()->rx_loaded);

    bus_write16(&S.bus, SIM_BYTE_IC, 0x007B);
    for (int i = 0; i < 20; i++) {
        cemu_soc_tick(&S, 256);
        CHECK(sim_state()->rx_loaded);
        CHECK(service_pec(SIM_BYTE_IC, 14) == 1);
    }
    CHECK(sim_state()->output_len == 0);
    CHECK(bus_read8(&S.bus, 0xE304) == 0x7F);
    CHECK(bus_read8(&S.bus, 0xE305) == 0x20);
    CHECK(bus_read8(&S.bus, 0xE306) == 0x02);
    CHECK(bus_read8(&S.bus, 0xE30D) == 0x93);
    CHECK(bus_read8(&S.bus, 0xE310) == 0x02);
    CHECK(bus_read8(&S.bus, 0xE312) == 0x83);
    CHECK(bus_read8(&S.bus, 0xE313) == 0x8A);
    CHECK((bus_read16(&S.bus, SIM_EVENT_IC) & XIC_IR_BIT) == 0);

    bus_write16(&S.bus, SIM_BYTE_IC, 0x007B);
    cemu_soc_tick(&S, 256);
    CHECK(bus_read16(&S.bus, 0xEF60) == 0x0090);
    CHECK(bus_read16(&S.bus, 0xEF62) == 0x0000);
    CHECK(bus_read16(&S.bus, SIM_STATUS) == 0x0008);
    CHECK((bus_read16(&S.bus, SIM_STATUS_IC) & XIC_IR_BIT) != 0);
    CHECK((bus_read16(&S.bus, SIM_EVENT_IC) & XIC_IR_BIT) == 0);
    cemu_soc_free(&S);
}

static void test_phase_file_returns_phase_2_plus(void) {
    static const uint8_t select_phase[] = {
        0xA0, 0xA4, 0x00, 0x00, 0x02, 0x6F, 0xAE,
    };
    static const uint8_t read_phase[] = {0xA0, 0xB0, 0x00, 0x00, 0x01};
    setup();
    ready_stub_card();
    issue_select_df_gsm();
    bus_write16(&S.bus, SIM_STATUS_IC, 0x0068);

    for (int i = 0; i < 5; i++)
        bus_write8(&S.bus, SIM_TX, select_phase[i]);
    arm_pec3(2, 0xE100, SIM_TX);
    cemu_soc_tick(&S, 256);
    bus_write8(&S.bus, SIM_TX, select_phase[5]);
    bus_write8(&S.bus, SIM_TX, select_phase[6]);
    bus_write16(&S.bus, PECC3, PECC_BWT);
    cemu_soc_tick(&S, 256);
    CHECK(sim_state()->selected_ef == 0x6FAE);
    CHECK(bus_read16(&S.bus, 0xEF60) == 0x009F);
    CHECK(bus_read16(&S.bus, 0xEF62) == 0x000F);
    bus_write16(&S.bus, SIM_STATUS_IC, 0x0068);

    for (size_t i = 0; i < sizeof read_phase; i++)
        bus_write8(&S.bus, SIM_TX, read_phase[i]);
    CHECK(sim_state()->output_len == 2);
    CHECK(sim_state()->completion_pending);

    arm_pec3(1, SIM_RX, 0xE400);
    cemu_soc_tick(&S, 256);
    CHECK(sim_state()->output_len == 1);
    bus_write16(&S.bus, SIM_BYTE_IC, 0x007B);
    cemu_soc_tick(&S, 256);
    CHECK(service_pec(SIM_BYTE_IC, 14) == 1);
    CHECK(bus_read8(&S.bus, 0xE400) == 0x02);

    bus_write16(&S.bus, SIM_BYTE_IC, 0x007B);
    cemu_soc_tick(&S, 256);
    CHECK(bus_read16(&S.bus, 0xEF60) == 0x0090);
    CHECK(bus_read16(&S.bus, 0xEF62) == 0x0000);
    CHECK((bus_read16(&S.bus, SIM_STATUS_IC) & XIC_IR_BIT) != 0);
    cemu_soc_free(&S);
}

static void test_directory_selection_never_sets_selected_ef(void) {
    static const uint16_t dirs[] = {0x7F10, 0x7F20, 0x7F21, 0x3F00};
    setup();
    ready_stub_card();
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        issue_select_id(dirs[i]);
        CHECK(sim_state()->selected_df == dirs[i]);
        CHECK(sim_state()->selected_ef == 0);
        CHECK(sim_state()->completion_sw == 0x9F0F);
        bus_write16(&S.bus, SIM_STATUS_IC, 0x0068);
    }
    cemu_soc_free(&S);
}

static void test_backed_file_descriptor_and_unbacked_rejection(void) {
    setup();
    ready_stub_card();
    select_ef_under_gsm(0x6F30);
    CHECK(sim_state()->selected_ef == 0x6F30);
    CHECK(sim_state()->response[2] == 0x00);
    CHECK(sim_state()->response[3] == 24);
    CHECK(sim_state()->response[4] == 0x6F);
    CHECK(sim_state()->response[5] == 0x30);
    CHECK(sim_state()->response[6] == 0x04);
    CHECK(sim_state()->response[12] == 0x02);
    CHECK(sim_state()->response[13] == SIM_FILE_TRANSPARENT);
    CHECK(sim_state()->response[14] == 0);
    bus_write16(&S.bus, SIM_STATUS_IC, 0x0068);

    issue_select_id(0x6F3C);
    CHECK(sim_state()->completion_sw == 0x9404);
    CHECK(sim_state()->response_len == 0);
    CHECK(sim_state()->selected_ef == 0x6F30);
    cemu_soc_free(&S);
}

static void check_transparent_read(uint16_t id, const uint8_t *expected, int len,
                                   uint32_t dst) {
    setup();
    ready_stub_card();
    select_ef_under_gsm(id);
    CHECK(sim_state()->completion_sw == 0x9F0F);
    bus_write16(&S.bus, SIM_STATUS_IC, 0x0068);

    const uint8_t read_header[] = {0xA0, 0xB0, 0x00, 0x00, (uint8_t)len};
    for (size_t i = 0; i < sizeof read_header; i++)
        bus_write8(&S.bus, SIM_TX, read_header[i]);
    CHECK(sim_state()->output_len == len + 1);
    CHECK(sim_state()->completion_sw == 0x9000);

    arm_pec3((uint16_t)len, SIM_RX, (uint16_t)dst);
    bus_write16(&S.bus, PECC3, PECC_BWT | PECC_INC_DST | (uint16_t)len);
    cemu_soc_tick(&S, 256);
    CHECK(sim_state()->output_len == len);
    bus_write16(&S.bus, SIM_BYTE_IC, 0x007B);
    for (int i = 0; i < len; i++) {
        cemu_soc_tick(&S, 256);
        CHECK(service_pec(SIM_BYTE_IC, 14) == 1);
    }
    for (int i = 0; i < len; i++) CHECK(bus_read8(&S.bus, dst + i) == expected[i]);
    bus_write16(&S.bus, SIM_BYTE_IC, 0x007B);
    cemu_soc_tick(&S, 256);
    CHECK((bus_read16(&S.bus, SIM_STATUS_IC) & XIC_IR_BIT) != 0);
    cemu_soc_free(&S);
}

static void test_lp_and_sst_read_through_rx_pec3(void) {
    static const uint8_t lp[] = {0xFF};
    static const uint8_t sst[] = {0x03, 0x00};
    check_transparent_read(0x6F05, lp, sizeof lp, 0xE500);
    check_transparent_read(0x6F38, sst, sizeof sst, 0xE510);
}

static void test_cphs_operator_name_profile(void) {
    static const uint8_t cphs_info[] = {0x01, 0x02, 0x00};
    static const uint8_t operator_name[] = {
        'U','A','-','K','Y','I','V','S','T','A','R',
        0xFF,0xFF,0xFF,0xFF,0xFF,
    };
    check_transparent_read(0x6F16, cphs_info, sizeof cphs_info, 0xE520);
    check_transparent_read(0x6F14, operator_name, sizeof operator_name, 0xE530);
}

static void test_p3_zero_means_256_response_bytes(void) {
    static const uint8_t status[] = {0xA0, 0xF2, 0x00, 0x00, 0x00};
    setup();
    ready_stub_card();
    issue_select_df_gsm();
    bus_write16(&S.bus, SIM_STATUS_IC, 0x0068);
    for (size_t i = 0; i < sizeof status; i++) bus_write8(&S.bus, SIM_TX, status[i]);
    CHECK(sim_state()->output_len == 257);
    CHECK(sim_state()->completion_sw == 0x9000);
    cemu_soc_free(&S);
}

static void test_no_data_error_completes_with_rx_pec_armed(void) {
    static const uint8_t read_256[] = {0xA0, 0xB0, 0x00, 0x00, 0x00};
    setup();
    ready_stub_card();
    select_ef_under_gsm(0x6F05);
    bus_write16(&S.bus, SIM_STATUS_IC, 0x0068);
    for (size_t i = 0; i < sizeof read_256; i++)
        bus_write8(&S.bus, SIM_TX, read_256[i]);
    CHECK(sim_state()->completion_sw == 0x9402);
    CHECK(sim_state()->output_len == 1);

    arm_pec3(17, SIM_RX, 0xE600);
    cemu_soc_tick(&S, 256);
    CHECK(sim_state()->output_len == 0);
    CHECK((bus_read16(&S.bus, PECC3) & 0xFF) == 17);
    cemu_soc_tick(&S, 256);
    CHECK(!sim_state()->completion_pending);
    CHECK((bus_read16(&S.bus, PECC3) & 0xFF) == 17);
    CHECK(bus_read16(&S.bus, 0xEF60) == 0x0094);
    CHECK(bus_read16(&S.bus, 0xEF62) == 0x0002);
    CHECK((bus_read16(&S.bus, SIM_STATUS_IC) & XIC_IR_BIT) != 0);
    cemu_soc_free(&S);
}

static void test_minimal_profile_contents(void) {
    setup();
    sim_state_t *st = sim_state();
    CHECK(st->file_count == 19);
    CHECK(st->lp[0] == 0xFF);
    CHECK(st->phase_ef[0] == 0x02);
    CHECK(st->sst[0] == 0x03 && st->sst[1] == 0x00);
    CHECK(!memcmp(st->ad, (uint8_t[]){0, 0, 0, 2}, 4));
    CHECK(!memcmp(st->kc, (uint8_t[]){0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x07}, 9));
    CHECK(st->hpplmn[0] == 0x00);
    for (size_t i = 0; i < sizeof st->plmn; i++) CHECK(st->plmn[i] == 0xFF);
    for (int i = 0; i < 10; i++) CHECK(st->loci[i] == 0xFF);
    CHECK(st->loci[10] == 0x01);
    for (size_t i = 0; i < sizeof st->bcch; i++) CHECK(st->bcch[i] == 0xFF);
    CHECK(st->acc[0] == 0x00 && st->acc[1] == 0x01);
    for (size_t i = 0; i < sizeof st->fplmn; i++) CHECK(st->fplmn[i] == 0xFF);
    CHECK(!memcmp(st->cphs_info, (uint8_t[]){0x01, 0x02, 0x00}, 3));
    CHECK(!memcmp(st->operator_name, "UA-KYIVSTAR", 11));
    for (int i = 11; i < 16; i++) CHECK(st->operator_name[i] == 0xFF);
    cemu_soc_free(&S);
}

static void test_mode_names(void) {
    CHECK(!strcmp(cemu_sim_mode_name(SIM_MODE_NONE), "none"));
    CHECK(!strcmp(cemu_sim_mode_name(SIM_MODE_STUB), "stub"));
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
    {"disabled_mode_ignores_activation", test_disabled_mode_ignores_activation},
    {"activation_delivers_atr_in_order", test_activation_delivers_atr_in_order},
    {"tx_pec_schedules_byte_request", test_tx_pec_schedules_byte_request},
    {"procedure_and_status_are_controller_internal", test_procedure_and_status_are_controller_internal},
    {"get_response_data_waits_for_rx_pec", test_get_response_data_waits_for_rx_pec},
    {"status_returns_df_gsm_profile_through_rx_pec", test_status_returns_df_gsm_profile_through_rx_pec},
    {"phase_file_returns_phase_2_plus", test_phase_file_returns_phase_2_plus},
    {"directory_selection_never_sets_selected_ef", test_directory_selection_never_sets_selected_ef},
    {"backed_file_descriptor_and_unbacked_rejection", test_backed_file_descriptor_and_unbacked_rejection},
    {"lp_and_sst_read_through_rx_pec3", test_lp_and_sst_read_through_rx_pec3},
    {"cphs_operator_name_profile", test_cphs_operator_name_profile},
    {"p3_zero_means_256_response_bytes", test_p3_zero_means_256_response_bytes},
    {"no_data_error_completes_with_rx_pec_armed", test_no_data_error_completes_with_rx_pec_armed},
    {"minimal_profile_contents", test_minimal_profile_contents},
    {"mode_names", test_mode_names},
};

int main(void) {
    int n = (int)(sizeof TESTS / sizeof TESTS[0]);
    for (int i = 0; i < n; i++) {
        g_fail = 0;
        TESTS[i].fn();
        g_total_run++;
        if (g_fail) {
            g_total_fail++;
            printf("[FAIL] %s (%d)\n", TESTS[i].name, g_fail);
        }
    }
    printf("\n%d tests, %d failed\n", g_total_run, g_total_fail);
    free(FL);
    return g_total_fail ? 1 : 0;
}
