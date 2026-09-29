/* Peripheral registration wiring — verifies the table-driven dispatch the SoC
 * builds at init (mirror of what pmb7850._register / _build_interrupt_maps set up):
 * SFR-word hooks resolve to the right peripheral, byte ranges dispatch, the merged
 * IC table carries both modeled and residual nodes, and typed handles are set. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "soc.h"
#include "synth.h"
#include "cpu.h"

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint8_t *make_flash(size_t len) {
    uint8_t *f = calloc(len, 1);
    memset(f, 0xFF, len);
    f[0] = 0xFA; f[1] = 0x80; f[2] = 0xC4; f[3] = 0x2F;   /* C55 reset vector */
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

static int pending_interrupt(int *trap, uint32_t *ic_addr, int *ilvl) {
    interrupt_request_t request;
    if (!cemu_interrupt_subsystem_pending(&S.interrupts, &request)) return 0;
    *trap = request.trap;
    *ic_addr = request.ic_addr;
    *ilvl = request.ilvl;
    return 1;
}

/* C55 registers shared peripherals, external RAM, and its board-attached LCD. */
static void test_registers_default_set(void) {
    setup();
    int found_capcom2 = 0, found_keypad = 0, found_tdma = 0, found_sim = 0;
    int found_gsm_stub = 0, found_gsm_legacy = 0, found_battery = 0;
    int found_ports = 0, found_ssc0 = 0, found_pec = 0, found_speaker = 0;
    CHECK(S.n_peripherals == 19);
    CHECK(S.memory.flash_endpoints[0] != NULL && strcmp(S.memory.flash_endpoints[0]->id, "flash") == 0);
    CHECK(S.xbus_unknown1_periph != NULL &&
          strcmp(S.xbus_unknown1_periph->id, "xbus-unknown-1") == 0);
    CHECK(S.keypad_periph != NULL &&
          strcmp(S.keypad_periph->id, "keypad") == 0);
    CHECK(S.ports_periph != NULL && strcmp(S.ports_periph->id, "ports") == 0);
    for (int i = 0; i < S.n_peripherals; i++) {
        if (strcmp(S.peripherals[i]->id, "capcom2") == 0) found_capcom2 = 1;
        if (strcmp(S.peripherals[i]->id, "keypad") == 0) found_keypad = 1;
        if (strcmp(S.peripherals[i]->id, "tdma") == 0) found_tdma = 1;
        if (strcmp(S.peripherals[i]->id, "sim") == 0) found_sim = 1;
        if (strcmp(S.peripherals[i]->id, "gsm-stub") == 0) found_gsm_stub = 1;
        if (strcmp(S.peripherals[i]->id, "gsm-legacy-adapter") == 0)
            found_gsm_legacy = 1;
        if (strcmp(S.peripherals[i]->id, "battery") == 0) found_battery = 1;
        if (strcmp(S.peripherals[i]->id, "ports") == 0) found_ports = 1;
        if (strcmp(S.peripherals[i]->id, "ssc0") == 0) found_ssc0 = 1;
        if (strcmp(S.peripherals[i]->id, "pec") == 0) found_pec = 1;
        if (strcmp(S.peripherals[i]->id, "speaker") == 0) found_speaker = 1;
    }
    CHECK(found_capcom2 && found_keypad && found_tdma && found_sim &&
          found_gsm_stub && found_gsm_legacy &&
          found_battery &&
          found_ports && found_ssc0 && found_pec && found_speaker);
    CHECK(S.speaker_periph == S.peripherals[S.n_peripherals - 1]);
    cemu_soc_free(&S);
}

/* SFR-word hooks resolve to the owning peripheral. */
static void test_sfr_hook_resolves_to_owners(void) {
    setup();
    /* S0TBUF/S0RIC are claimed by ASC0; P7 by ports; SSC0 data/control by SSC0. */
    peripheral_t *p1 = S.memory.sfr_hook[memory_controller_sfr_index(0xFEB0)];
    peripheral_t *p2 = S.memory.sfr_hook[memory_controller_sfr_index(0xFF6E)];
    peripheral_t *p3 = S.memory.sfr_hook[memory_controller_sfr_index(0xFFD0)];
    peripheral_t *p4 = S.memory.sfr_hook[memory_controller_sfr_index(0xF0B0)];
    peripheral_t *p5 = S.memory.sfr_hook[memory_controller_sfr_index(0xFFB2)];
    CHECK(p1 != NULL && strcmp(p1->id, "asc0") == 0);
    CHECK(p2 != NULL && strcmp(p2->id, "asc0") == 0);
    CHECK(p3 != NULL && strcmp(p3->id, "ports") == 0);
    CHECK(p4 != NULL && strcmp(p4->id, "ssc0") == 0);
    CHECK(p5 != NULL && strcmp(p5->id, "ssc0") == 0);
    /* An unclaimed SFR word has no hook. */
    CHECK(S.memory.sfr_hook[memory_controller_sfr_index(0xFE00)] == NULL);   /* DPP0 */
    cemu_soc_free(&S);
}

/* The merged IC table carries modeled timer/serial/CAPCOM/SSC0 nodes. */
static void test_ic_table_merges_modeled_nodes(void) {
    setup();
    int found_t6 = 0, found_s0ric = 0, found_s0eic = 0, found_t4 = 0, found_t5 = 0;
    int found_t1 = 0, found_t8 = 0, found_ssc0tic = 0, found_ssc0ric = 0, found_ssc0eic = 0, found_eopic = 0;
    int found_adeic = 0, found_xp0ic = 0, found_xp1ic = 0, found_irq80 = 0;
    int found_sim_byte = 0, found_sim_status = 0, found_sim_event = 0;
    for (int i = 0; i < S.interrupts.n_sources; i++) {
        uint32_t a = S.interrupts.sources[i].addr;
        if (a == 0xFF68) { found_t6 = 1; CHECK(S.interrupts.sources[i].trap == 0x26); CHECK(S.interrupts.sources[i].is_timer); }
        if (a == 0xFF6E) { found_s0ric = 1; CHECK(S.interrupts.sources[i].trap == 0x2B); CHECK(S.interrupts.sources[i].owner != NULL && strcmp(S.interrupts.sources[i].owner->id, "asc0") == 0); }
        if (a == 0xFF70) { found_s0eic = 1; CHECK(S.interrupts.sources[i].trap == 0x2C); CHECK(S.interrupts.sources[i].owner != NULL && strcmp(S.interrupts.sources[i].owner->id, "asc0") == 0); }
        if (a == 0xFF64) { found_t4 = 1; CHECK(S.interrupts.sources[i].trap == 0x24); CHECK(S.interrupts.sources[i].is_timer); CHECK(S.interrupts.sources[i].owner != NULL); }
        if (a == 0xFF66) { found_t5 = 1; CHECK(S.interrupts.sources[i].trap == 0x25); CHECK(S.interrupts.sources[i].is_timer); CHECK(S.interrupts.sources[i].owner != NULL); }
        if (a == 0xFF72) { found_ssc0tic = 1; CHECK(S.interrupts.sources[i].trap == 0x2D); CHECK(S.interrupts.sources[i].owner != NULL && strcmp(S.interrupts.sources[i].owner->id, "ssc0") == 0); }
        if (a == 0xFF74) { found_ssc0ric = 1; CHECK(S.interrupts.sources[i].trap == 0x2E); CHECK(S.interrupts.sources[i].owner != NULL && strcmp(S.interrupts.sources[i].owner->id, "ssc0") == 0); }
        if (a == 0xFF76) { found_ssc0eic = 1; CHECK(S.interrupts.sources[i].trap == 0x2F); CHECK(S.interrupts.sources[i].owner != NULL && strcmp(S.interrupts.sources[i].owner->id, "ssc0") == 0); }
        if (a == 0xFF9E) { found_t1 = 1; CHECK(S.interrupts.sources[i].trap == 0x21); CHECK(S.interrupts.sources[i].is_timer); }
        if (a == 0xF17C) { found_t8 = 1; CHECK(S.interrupts.sources[i].trap == 0x3E); CHECK(S.interrupts.sources[i].is_timer); }
        if (a == 0xF180) { found_eopic = 1; CHECK(S.interrupts.sources[i].trap == 0x4C); CHECK(S.interrupts.sources[i].owner != NULL && strcmp(S.interrupts.sources[i].owner->id, "pec") == 0); }
        if (a == 0xFF9A) {
            found_adeic = 1;
            CHECK(S.interrupts.sources[i].trap == 0x29);
            CHECK(!S.interrupts.sources[i].is_timer);
            CHECK(S.interrupts.sources[i].owner == NULL);
        }
        if (a == 0xF186) {
            found_xp0ic = 1;
            CHECK(S.interrupts.sources[i].trap == 0x40);
            CHECK(S.interrupts.sources[i].is_timer);
            CHECK(S.interrupts.sources[i].owner != NULL &&
                  strcmp(S.interrupts.sources[i].owner->id, "tdma") == 0);
        }
        if (a == 0xF140) {
            found_irq80 = 1;
            CHECK(S.interrupts.sources[i].trap == 0x50);
            CHECK(!S.interrupts.sources[i].is_timer);
            CHECK(S.interrupts.sources[i].owner != NULL &&
                  strcmp(S.interrupts.sources[i].owner->id, "xbus-unknown-1") == 0);
        }
        if (a == 0xF18E) {
            found_xp1ic = 1;
            CHECK(S.interrupts.sources[i].trap == 0x41);
            CHECK(S.interrupts.sources[i].is_timer);
            CHECK(S.interrupts.sources[i].owner != NULL &&
                  strcmp(S.interrupts.sources[i].owner->id, "tdma") == 0);
        }
        if (a == 0xF184 || a == 0xF18C || a == 0xF194) {
            int *found = a == 0xF184 ? &found_sim_byte :
                         a == 0xF18C ? &found_sim_status : &found_sim_event;
            int trap = a == 0xF184 ? 0x44 : a == 0xF18C ? 0x45 : 0x46;
            *found = 1;
            CHECK(S.interrupts.sources[i].trap == trap);
            CHECK(!S.interrupts.sources[i].is_timer);
            CHECK(S.interrupts.sources[i].owner != NULL &&
                  strcmp(S.interrupts.sources[i].owner->id, "sim") == 0);
        }
    }
    CHECK(found_t6 && found_s0ric && found_s0eic && found_t4 && found_t5 &&
          found_ssc0tic && found_ssc0ric && found_ssc0eic && found_t1 && found_t8 &&
          found_eopic && found_adeic && found_xp0ic && found_xp1ic && found_irq80 &&
          found_sim_byte && found_sim_status && found_sim_event);
    cemu_soc_free(&S);
}

/* The XBUS mailbox latches IRQ80 even while masked; enabling the configured
 * node later makes trap 0x50 deliver through the generic C166S xIC path. */
static void test_xbus_mailbox_irq80_delivery(void) {
    setup();
    int trap = -1, ilvl = -1;
    uint32_t ic = 0;

    bus_write16(&S.bus, 0xF140, 0x0011);  /* configured, IE=0 */
    bus_write16(&S.bus, 0xEC10, 0x1704);  /* C55 legacy IRQ mode */
    bus_write16(&S.bus, 0xEC12, 0x0001);
    CHECK(bus_read16(&S.bus, 0xF140) == 0x0011);
    cemu_soc_tick(&S, 31);
    CHECK(bus_read16(&S.bus, 0xF140) == 0x0011);
    cemu_soc_tick(&S, 1);
    CHECK(bus_read16(&S.bus, 0xF140) == 0x0091);
    CHECK(!pending_interrupt(&trap, &ic, &ilvl));

    bus_write16(&S.bus, 0xF140, 0x00D1);  /* preserve IR, enable */
    CHECK(pending_interrupt(&trap, &ic, &ilvl));
    CHECK(trap == 0x50);
    CHECK(ic == 0xF140);
    CHECK(ilvl == 4);

    cpu_t cpu;
    cemu_cpu_init(&cpu, &S.bus);
    cemu_soc_attach_cpu(&S, &cpu);
    cemu_memory_controller_poke8(&S.memory, 0x0140, 0xCC);          /* NOP at IRQ80 vector */
    cemu_memory_controller_poke8(&S.memory, 0x0141, 0x00);
    cpu.csp = 0;
    cpu.ip = 0x0200;
    cemu_cpu_set_psw(&cpu, 1u << 11);          /* IEN=1, CPU ILVL=0 */

    CHECK(cemu_cpu_step(&cpu) == STEP_OK);
    CHECK(cpu.interrupts_delivered == 1);
    CHECK(cpu_pc(&cpu) == 0x000142);
    CHECK((bus_read16(&S.bus, 0xF140) & XIC_IR_BIT) == 0);
    CHECK(!pending_interrupt(&trap, &ic, &ilvl));
    cemu_soc_free(&S);
}

/* C166S permits software to set xxIR. Firmware uses ADEIC this way to enter
 * the scheduler ISR, so delivery needs only the classic xIC node, not an ADC. */
static void test_adeic_software_request_is_deliverable(void) {
    setup();
    int trap = -1, ilvl = -1;
    uint32_t ic = 0;

    bus_write16(&S.bus, 0xFF9A, 0x00C4);  /* IR=1, IE=1, ILVL=1, GLVL=0 */
    CHECK(pending_interrupt(&trap, &ic, &ilvl));
    CHECK(trap == 0x29);
    CHECK(ic == 0xFF9A);
    CHECK(ilvl == 1);
    CHECK(cemu_soc_idle_wake_possible(&S));

    cpu_t cpu;
    cemu_cpu_init(&cpu, &S.bus);
    cemu_soc_attach_cpu(&S, &cpu);
    cemu_memory_controller_poke8(&S.memory, 0x00A4, 0xCC);  /* NOP at ADEIC trap 0x29 vector */
    cemu_memory_controller_poke8(&S.memory, 0x00A5, 0x00);
    cpu.csp = 0;
    cpu.ip = 0x0100;
    cemu_cpu_set_psw(&cpu, 1u << 11);  /* IEN=1, CPU ILVL=0 */

    CHECK(cemu_cpu_step(&cpu) == STEP_OK);
    CHECK(cpu.interrupts_delivered == 1);
    CHECK(cpu_pc(&cpu) == 0x0000A6);
    CHECK((bus_read16(&S.bus, 0xFF9A) & XIC_IR_BIT) == 0);
    CHECK(!pending_interrupt(&trap, &ic, &ilvl));
    CHECK(!cemu_soc_idle_wake_possible(&S));
    cemu_soc_free(&S);
}

/* IRQ36IC/IRQ37IC (firmware aliases XP0IC/XP1IC) retain generic xIC software
 * request semantics in addition to their autonomous TDMA source. */
static void test_xp_software_requests_are_deliverable(void) {
    setup();
    int trap = -1, ilvl = -1;
    uint32_t ic = 0;

    bus_write16(&S.bus, 0xF186, 0x00C4);  /* XP0: IR=1, IE=1, ILVL=1 */
    CHECK(pending_interrupt(&trap, &ic, &ilvl));
    CHECK(trap == 0x40);
    CHECK(ic == 0xF186);
    CHECK(ilvl == 1);
    CHECK(cemu_soc_idle_wake_possible(&S));

    cpu_t cpu;
    cemu_cpu_init(&cpu, &S.bus);
    cemu_soc_attach_cpu(&S, &cpu);
    cemu_memory_controller_poke8(&S.memory, 0x0100, 0xCC);  /* NOP at XP0 trap 0x40 vector */
    cemu_memory_controller_poke8(&S.memory, 0x0101, 0x00);
    cpu.csp = 0;
    cpu.ip = 0x0200;
    cemu_cpu_set_psw(&cpu, 1u << 11);

    CHECK(cemu_cpu_step(&cpu) == STEP_OK);
    CHECK(cpu.interrupts_delivered == 1);
    CHECK(cpu_pc(&cpu) == 0x000102);
    CHECK((bus_read16(&S.bus, 0xF186) & XIC_IR_BIT) == 0);

    bus_write16(&S.bus, 0xF18E, 0x00C8);  /* XP1: IR=1, IE=1, ILVL=2 */
    cemu_memory_controller_poke8(&S.memory, 0x0104, 0xCC);          /* NOP at XP1 trap 0x41 vector */
    cemu_memory_controller_poke8(&S.memory, 0x0105, 0x00);
    cemu_cpu_set_psw(&cpu, 1u << 11);

    CHECK(cemu_cpu_step(&cpu) == STEP_OK);
    CHECK(cpu.interrupts_delivered == 2);
    CHECK(cpu_pc(&cpu) == 0x000106);
    CHECK((bus_read16(&S.bus, 0xF18E) & XIC_IR_BIT) == 0);
    CHECK(!pending_interrupt(&trap, &ic, &ilvl));
    CHECK(!cemu_soc_idle_wake_possible(&S));
    cemu_soc_free(&S);
}

/* Product-band words 0xF120..0xF13E remain excluded until their owner is proven. */
static void test_product_band_ic_words_stay_excluded(void) {
    setup();
    for (uint32_t a = 0xF120; a <= 0xF13E; a += 2) {
        bus_write16(&S.bus, a, 0x00E0);  /* IR=1, IE=1 lookalike */
        CHECK(!cemu_soc_idle_wake_possible(&S));
        bus_write16(&S.bus, a, 0x0000);
    }
    cemu_soc_free(&S);
}

/* Register names are indexed at registration and drive cemu_soc_sfr_modeled: a word
 * a peripheral NAMES is modeled (green); one that resolves only via the residual
 * unmodeled table (P4) is not. Mirrors _peripheral_names / _sfr_modeled — the
 * naming lives in each peripheral's reg_names, not in a central table. */
static void test_register_names_drive_modeled(void) {
    setup();
    /* Peripheral-named SFRs (from ASC0's reg_names) are modeled. */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFEB0));   /* S0TBUF */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFF6C));   /* S0TIC  */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFFB0));   /* S0CON  */
    /* Core CSFRs are modeled without any peripheral. */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFF10));   /* PSW */
    /* P6/P7 are now owned and named by the ports peripheral. */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFFCC) && !strcmp(cemu_soc_sfr_name(&S, 0xFFCC), "P6"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFFCE) && !strcmp(cemu_soc_sfr_name(&S, 0xFFCE), "DP6"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFFD0) && !strcmp(cemu_soc_sfr_name(&S, 0xFFD0), "P7"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFFD2) && !strcmp(cemu_soc_sfr_name(&S, 0xFFD2), "DP7"));
    /* Residual-name-only cells now cover the still-unmodeled summary labels we
     * want to see by name rather than as unknown@... */
    CHECK(!cemu_soc_sfr_modeled(&S, 0xF038) && !strcmp(cemu_soc_sfr_name(&S, 0xF038), "PP0"));
    CHECK(!cemu_soc_sfr_modeled(&S, 0xF03A) && !strcmp(cemu_soc_sfr_name(&S, 0xF03A), "PP1"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF0B0) && !strcmp(cemu_soc_sfr_name(&S, 0xF0B0), "SSC0TB"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF0B2) && !strcmp(cemu_soc_sfr_name(&S, 0xF0B2), "SSC0RB"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF0B4) && !strcmp(cemu_soc_sfr_name(&S, 0xF0B4), "SSC0BR"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF0B6) && !strcmp(cemu_soc_sfr_name(&S, 0xF0B6), "SSC0PISEL"));
    CHECK(!cemu_soc_sfr_modeled(&S, 0xF0C6) && !strcmp(cemu_soc_sfr_name(&S, 0xF0C6), "IRQ103IC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF190) && !strcmp(cemu_soc_sfr_name(&S, 0xF190), "IRQ45IC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF196) && !strcmp(cemu_soc_sfr_name(&S, 0xF196), "XP2IC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF180) && !strcmp(cemu_soc_sfr_name(&S, 0xF180), "EOPIC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF184) && !strcmp(cemu_soc_sfr_name(&S, 0xF184), "SIM_BYTE_IC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF18C) && !strcmp(cemu_soc_sfr_name(&S, 0xF18C), "SIM_STATUS_IC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF194) && !strcmp(cemu_soc_sfr_name(&S, 0xF194), "SIM_EVENT_IC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFEC4) && !strcmp(cemu_soc_sfr_name(&S, 0xFEC4), "PECC2"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFEC8) && !strcmp(cemu_soc_sfr_name(&S, 0xFEC8), "PECC4"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFED0) && !strcmp(cemu_soc_sfr_name(&S, 0xFED0), "PECSN0"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFED4) && !strcmp(cemu_soc_sfr_name(&S, 0xFED4), "PECSN2"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFED8) && !strcmp(cemu_soc_sfr_name(&S, 0xFED8), "PECSN4"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFFA8) && !strcmp(cemu_soc_sfr_name(&S, 0xFFA8), "PECISNC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFF70) && !strcmp(cemu_soc_sfr_name(&S, 0xFF70), "S0EIC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFF72) && !strcmp(cemu_soc_sfr_name(&S, 0xFF72), "SSC0TIC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFF74) && !strcmp(cemu_soc_sfr_name(&S, 0xFF74), "SSC0RIC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFF76) && !strcmp(cemu_soc_sfr_name(&S, 0xFF76), "SSC0EIC"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFFB2) && !strcmp(cemu_soc_sfr_name(&S, 0xFFB2), "SSC0CON"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xF140) && !strcmp(cemu_soc_sfr_name(&S, 0xF140), "IRQ80IC"));
    CHECK(!cemu_soc_sfr_modeled(&S, 0xFF98) && !strcmp(cemu_soc_sfr_name(&S, 0xFF98), "ADCIC"));
    CHECK(!cemu_soc_sfr_modeled(&S, 0xFFC0) && !strcmp(cemu_soc_sfr_name(&S, 0xFFC0), "P2"));
    CHECK(!cemu_soc_sfr_modeled(&S, 0xFFC2) && !strcmp(cemu_soc_sfr_name(&S, 0xFFC2), "DP2"));
    CHECK(!cemu_soc_sfr_modeled(&S, 0xFFC8) && !strcmp(cemu_soc_sfr_name(&S, 0xFFC8), "P4"));
    CHECK(cemu_soc_sfr_modeled(&S, 0xFFD4) && !strcmp(cemu_soc_sfr_name(&S, 0xFFD4), "P8"));
    CHECK(!cemu_soc_sfr_modeled(&S, 0xFFEE) && !strcmp(cemu_soc_sfr_name(&S, 0xFFEE), "(RSVD)"));
    CHECK(!cemu_soc_sfr_modeled(&S, 0xFFF0) && !strcmp(cemu_soc_sfr_name(&S, 0xFFF0), "(RSVD)"));
    /* Timer registers are named+modeled by their own blocks, not by a central
     * table — so they resolve green and carry the block's name. */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFE42) && !strcmp(cemu_soc_sfr_name(&S, 0xFE42), "T3"));      /* GPT1 */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFE44) && !strcmp(cemu_soc_sfr_name(&S, 0xFE44), "T4"));      /* GPT1 */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFE46) && !strcmp(cemu_soc_sfr_name(&S, 0xFE46), "T5"));      /* GPT2 */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFE48) && !strcmp(cemu_soc_sfr_name(&S, 0xFE48), "T6"));      /* GPT2 */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFF9C) && !strcmp(cemu_soc_sfr_name(&S, 0xFF9C), "T0IC"));    /* CAPCOM1 */
    CHECK(cemu_soc_sfr_modeled(&S, 0xF052) && !strcmp(cemu_soc_sfr_name(&S, 0xF052), "T8"));      /* CAPCOM2 */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFE6C) && !strcmp(cemu_soc_sfr_name(&S, 0xFE6C), "CC22"));    /* CAPCOM2 */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFF84) && !strcmp(cemu_soc_sfr_name(&S, 0xFF84), "CC6IC"));   /* CAPCOM1 */
    CHECK(cemu_soc_sfr_modeled(&S, 0xF16C) && !strcmp(cemu_soc_sfr_name(&S, 0xF16C), "CC22IC"));  /* CAPCOM2 */
    CHECK(cemu_soc_sfr_modeled(&S, 0xF16E) && !strcmp(cemu_soc_sfr_name(&S, 0xF16E), "IRQ55IC")); /* PMB7850 */
    CHECK(cemu_soc_sfr_modeled(&S, 0xFF7E) && !strcmp(cemu_soc_sfr_name(&S, 0xFF7E), "CC3IC"));   /* CAPCOM1 */
    /* A residual DPRAM name resolves via the unmodeled table; a peripheral DPRAM
     * cell resolves via its block. */
    CHECK(!strcmp(cemu_soc_dpram_name(&S, 0xFCE0), "SRCP0"));         /* PEC-owned pointer */
    CHECK(!strcmp(cemu_soc_dpram_name(&S, 0xFCC0), "SRCP12"));
    CHECK(!strcmp(cemu_soc_internal_io_name(&S, 0xE800), "XBUS_UNKNOWN1_ID"));
    CHECK(!strcmp(cemu_soc_internal_io_name(&S, 0xEF1A), "KEYPAD_SCAN_RESULT"));
    CHECK(!strcmp(cemu_soc_internal_io_name(&S, 0xEF1C), "KEYPAD_SCAN_COMMAND"));
    /* Firmware-private FDxx bookkeeping is ordinary DPRAM, not keypad-owned. */
    CHECK(cemu_soc_dpram_name(&S, 0xFD12) == NULL);
    cemu_soc_free(&S);
}

/* Byte-range dispatch: an unknown-XBUS address routes to its model. */
static void test_byte_range_dispatch(void) {
    setup();
    /* Reading the ID goes through the unknown-XBUS read8 hook. */
    CHECK(bus_read16(&S.bus, 0xE800) == 0x1202);
    /* Flash peripheral claims no byte ranges (direct-call special case). */
    CHECK(S.memory.flash_endpoints[0]->n_byte_ranges == 0);
    /* Timer tick still advances T6 through the registry tick loop. */
    bus_write16(&S.bus, 0xFF48, 0x0040);   /* T6CON: run + Timer Mode */
    bus_write16(&S.bus, 0xFE48, 0x0000);
    for (int i = 0; i < 8; i++) cemu_soc_tick(&S, 1);  /* div 4 -> +2 */
    CHECK(bus_read16(&S.bus, 0xFE48) == 0x0002);
    cemu_soc_free(&S);
}

typedef struct {
    const uint8_t *data;
    size_t len;
    uint32_t bases[2];
    int nbases;
} flash_image_t;

enum { FLASH_VIEW_LOW_MIRROR, FLASH_VIEW_NATIVE };

static void flash_image_init(flash_image_t *img, const uint8_t *data, size_t len,
                             const uint32_t *bases, int nbases) {
    img->data = data;
    img->len = len;
    img->nbases = nbases;
    memcpy(img->bases, bases, (size_t)nbases * sizeof(*bases));
}

static int flash_image_offset(const flash_image_t *img, uint32_t addr,
                              uint32_t *off) {
    for (int i = 0; i < img->nbases; i++)
        if (img->bases[i] <= addr && addr - img->bases[i] < img->len) {
            if (off) *off = addr - img->bases[i];
            return 1;
        }
    return 0;
}

static int flash_image_contains(const flash_image_t *img, uint32_t addr) {
    return flash_image_offset(img, addr, NULL);
}

static uint8_t flash_image_read8(const flash_image_t *img, uint32_t addr) {
    uint32_t off;
    return flash_image_offset(img, addr, &off) ? img->data[off] : 0xFF;
}

static uint8_t legacy_m58lw064d_read8(const flash_image_t *img,
                                      m58lw064d_state_t *st, uint32_t addr,
                                      int view, m58lw064d_access_t *access) {
    uint32_t off = 0;
    flash_image_offset(img, addr, &off);
    return cemu_m58lw064d_read8(img->data, img->len, st, off,
                           view == FLASH_VIEW_NATIVE, access);
}

static void legacy_m58lw064d_write8(const flash_image_t *img,
                                    m58lw064d_state_t *st, uint32_t addr,
                                    uint8_t value, int view,
                                    m58lw064d_access_t *access) {
    uint32_t off = 0;
    flash_image_offset(img, addr, &off);
    cemu_m58lw064d_write8(img->data, img->len, st, off, value,
                     view == FLASH_VIEW_NATIVE, access);
}

static void legacy_m58lw064d_write16(const flash_image_t *img,
                                     m58lw064d_state_t *st, uint32_t addr,
                                     uint16_t value, int view,
                                     m58lw064d_access_t *access) {
    uint32_t off = 0;
    flash_image_offset(img, addr, &off);
    cemu_m58lw064d_write16(img->data, img->len, st, off, value,
                      view == FLASH_VIEW_NATIVE, access);
}

/* Controller behavior is tested here independently from the SoC map. */
static void test_flash_image_and_commands(void) {
    uint8_t data[0x200];
    memset(data, 0xEE, sizeof data);
    data[0x10] = 0xAB;
    flash_image_t img;
    uint32_t bases[2] = {0x000000, 0x800000};
    m58lw064d_access_t access;
    flash_image_init(&img, data, sizeof data, bases, 2);
    /* mapped at both bases */
    CHECK(flash_image_contains(&img, 0x000010));
    CHECK(flash_image_contains(&img, 0x800010));
    CHECK(flash_image_read8(&img, 0x000010) == 0xAB);
    CHECK(flash_image_read8(&img, 0x800010) == 0xAB);
    /* outside the image */
    CHECK(!flash_image_contains(&img, 0x000200));
    CHECK(!flash_image_contains(&img, 0x123456));

    m58lw064d_state_t st; peripheral_t p; cemu_m58lw064d_periph_init(&p, &st);
    CHECK(st.read_mode == FLASH_ARRAY);
    CHECK(st.cmd_writes == 0);
    CHECK(!strcmp(cemu_m58lw064d_mode_str(&st), "array"));

    /* Native-window writes are command writes; native reads honor the active
     * read mode, while low-mirror reads stay plain array bytes. */
    legacy_m58lw064d_write8(&img, &st, 0x800000, 0x90, FLASH_VIEW_NATIVE, &access);
    CHECK(st.cmd_writes == 1);
    CHECK(st.read_mode == FLASH_ID);
    CHECK(!strcmp(access.subtype, "cmd"));
    CHECK(!strcmp(access.detail, "read-id"));
    CHECK(access.include_mode == 1);

    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800000, FLASH_VIEW_NATIVE, &access) == 0x20);
    CHECK(!strcmp(access.subtype, "id"));
    CHECK(!strcmp(access.detail, "flash"));
    CHECK(access.include_mode == 0);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800002, FLASH_VIEW_NATIVE, NULL) == 0x17);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800004, FLASH_VIEW_NATIVE, NULL) == 0x00);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800100, FLASH_VIEW_NATIVE, NULL) == 0xFE);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800101, FLASH_VIEW_NATIVE, NULL) == 0x00);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800102, FLASH_VIEW_NATIVE, NULL) == 0x00);
    uint8_t uid[8];
    CHECK(cemu_m58lw064d_factory_uid_parse("0xDF64412CCA0346FB", uid) == 0);
    CHECK(cemu_m58lw064d_factory_uid_parse("DF64412CCA0346F", uid) != 0);
    CHECK(cemu_m58lw064d_factory_uid_parse("DF64412CCA0346FG", uid) != 0);
    cemu_m58lw064d_factory_uid_set(&st, uid);
    static const uint8_t expected_uid[8] = {0xDF, 0x64, 0x41, 0x2C,
                                             0xCA, 0x03, 0x46, 0xFB};
    for (int i = 0; i < 8; i++) {
        CHECK(legacy_m58lw064d_read8(&img, &st, 0x800102u + (uint32_t)i,
                              FLASH_VIEW_NATIVE, &access) == expected_uid[i]);
        CHECK(!strcmp(access.subtype, "id"));
        CHECK(!strcmp(access.detail, "factory-uid"));
    }

    m58lw064d_state_t copy; peripheral_t copy_p; cemu_m58lw064d_periph_init(&copy_p, &copy);
    CHECK(cemu_m58lw064d_state_copy(&copy, &st));
    CHECK(copy.factory_uid_set);
    CHECK(memcmp(copy.factory_uid, expected_uid, sizeof(expected_uid)) == 0);
    CHECK(cemu_m58lw064d_state_restore(&copy, FLASH_ARRAY, FLASH_PHASE_IDLE,
                              0x80, 0, 0, FLASH_WB_IDLE, 0,
                              0, UINT32_MAX, NULL, 0, NULL, 0));
    CHECK(copy.factory_uid_set);
    CHECK(memcmp(copy.factory_uid, expected_uid, sizeof(expected_uid)) == 0);
    cemu_m58lw064d_state_free(&copy);

    /* User-programmable words 0x85..0x88 remain distinct from the factory UID. */
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x80010a, FLASH_VIEW_NATIVE, NULL) == 0x00);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x000000, FLASH_VIEW_LOW_MIRROR, &access) == 0xEE);
    CHECK(access.subtype == NULL);
    CHECK(!strcmp(access.detail, "flash"));

    legacy_m58lw064d_write8(&img, &st, 0x800000, 0x70, FLASH_VIEW_NATIVE, &access);
    CHECK(st.cmd_writes == 2);
    CHECK(st.read_mode == FLASH_STATUS);
    CHECK(!strcmp(access.subtype, "cmd"));
    CHECK(!strcmp(access.detail, "read-status"));
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800000, FLASH_VIEW_NATIVE, NULL) == 0x80);

    legacy_m58lw064d_write8(&img, &st, 0x000000, 0x40, FLASH_VIEW_LOW_MIRROR, &access);
    CHECK(st.cmd_writes == 2);
    CHECK(st.read_mode == FLASH_STATUS);
    CHECK(!strcmp(access.subtype, "write-absorbed"));
    CHECK(!strcmp(access.detail, "not-modeled"));
    CHECK(access.include_mode == 1);

    legacy_m58lw064d_write8(&img, &st, 0x800000, 0xFF, FLASH_VIEW_NATIVE, &access);
    CHECK(st.cmd_writes == 3);
    CHECK(st.read_mode == FLASH_ARRAY);
    CHECK(!strcmp(access.subtype, "cmd"));
    CHECK(!strcmp(access.detail, "read-array"));
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800000, FLASH_VIEW_NATIVE, &access) == 0xEE);
    CHECK(access.subtype == NULL);
    CHECK(!strcmp(access.detail, "flash"));
}

static void test_flash_write_buffer_payload_keeps_status_mode(void) {
    uint8_t data[0x40];
    memset(data, 0xFF, sizeof data);
    flash_image_t img;
    uint32_t bases[2] = {0x000000, 0x800000};
    m58lw064d_access_t access;
    flash_image_init(&img, data, sizeof data, bases, 2);

    m58lw064d_state_t st; peripheral_t p; cemu_m58lw064d_periph_init(&p, &st);
    legacy_m58lw064d_write8(&img, &st, 0x800000, 0x70, FLASH_VIEW_NATIVE, &access);
    CHECK(st.read_mode == FLASH_STATUS);
    CHECK(st.write_buffer_state == FLASH_WB_IDLE);

    legacy_m58lw064d_write8(&img, &st, 0x800000, 0xE8, FLASH_VIEW_NATIVE, &access);
    CHECK(st.read_mode == FLASH_STATUS);
    CHECK(st.write_buffer_state == FLASH_WB_EXPECT_COUNT);
    CHECK(!strcmp(access.detail, "buffer-program"));
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800000, FLASH_VIEW_NATIVE, NULL) == 0x80);

    legacy_m58lw064d_write16(&img, &st, 0x800000, 0x0005, FLASH_VIEW_NATIVE, &access);
    CHECK(st.write_buffer_state == FLASH_WB_LOADING);
    CHECK(st.write_buffer_words_left == 6);
    CHECK(!strcmp(access.detail, "write-buffer-count"));

    static const uint16_t payload[] = {0x01FE, 0x0034, 0x2A68, 0x00FF, 0x13AF, 0xFE00};
    for (size_t i = 0; i < sizeof(payload) / sizeof(payload[0]); i++)
        legacy_m58lw064d_write16(&img, &st, 0x800000 + (uint32_t)(i * 2), payload[i], FLASH_VIEW_NATIVE, &access);
    CHECK(st.write_buffer_state == FLASH_WB_EXPECT_CONFIRM);
    CHECK(st.read_mode == FLASH_STATUS);
    CHECK(!strcmp(access.detail, "write-buffer-data"));
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x80000E, FLASH_VIEW_NATIVE, &access) == 0x80);
    CHECK(!strcmp(access.subtype, "status"));

    legacy_m58lw064d_write16(&img, &st, 0x80000E, 0x00D0, FLASH_VIEW_NATIVE, &access);
    CHECK(st.write_buffer_state == FLASH_WB_IDLE);
    CHECK(st.read_mode == FLASH_STATUS);
    CHECK(!strcmp(access.detail, "confirm/resume"));
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x80000E, FLASH_VIEW_NATIVE, NULL) == 0x80);

    legacy_m58lw064d_write8(&img, &st, 0x800000, 0xFF, FLASH_VIEW_NATIVE, &access);
    CHECK(st.read_mode == FLASH_ARRAY);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800000, FLASH_VIEW_NATIVE, NULL) == 0xFE);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800001, FLASH_VIEW_NATIVE, NULL) == 0x01);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x80000A, FLASH_VIEW_NATIVE, NULL) == 0x00);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x80000B, FLASH_VIEW_NATIVE, NULL) == 0xFE);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x000000, FLASH_VIEW_LOW_MIRROR, NULL) == 0xFE);

    legacy_m58lw064d_write8(&img, &st, 0x800000, 0xE8, FLASH_VIEW_NATIVE, &access);
    legacy_m58lw064d_write16(&img, &st, 0x800000, 0x0000, FLASH_VIEW_NATIVE, &access);
    legacy_m58lw064d_write16(&img, &st, 0x800000, 0xF0FF, FLASH_VIEW_NATIVE, &access);
    legacy_m58lw064d_write16(&img, &st, 0x800000, 0x00D0, FLASH_VIEW_NATIVE, &access);
    legacy_m58lw064d_write8(&img, &st, 0x800000, 0xFF, FLASH_VIEW_NATIVE, &access);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800000, FLASH_VIEW_NATIVE, NULL) == 0xFE);
    CHECK(legacy_m58lw064d_read8(&img, &st, 0x800001, FLASH_VIEW_NATIVE, NULL) == 0x00);
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
    {"registers_default_set", test_registers_default_set},
    {"flash_image_and_commands", test_flash_image_and_commands},
    {"flash_write_buffer_payload_keeps_status_mode", test_flash_write_buffer_payload_keeps_status_mode},
    {"sfr_hook_resolves_to_owners", test_sfr_hook_resolves_to_owners},
    {"ic_table_merges_modeled_nodes", test_ic_table_merges_modeled_nodes},
    {"xbus_mailbox_irq80_delivery", test_xbus_mailbox_irq80_delivery},
    {"adeic_software_request_is_deliverable", test_adeic_software_request_is_deliverable},
    {"xp_software_requests_are_deliverable", test_xp_software_requests_are_deliverable},
    {"product_band_ic_words_stay_excluded", test_product_band_ic_words_stay_excluded},
    {"register_names_drive_modeled", test_register_names_drive_modeled},
    {"byte_range_dispatch", test_byte_range_dispatch},
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
