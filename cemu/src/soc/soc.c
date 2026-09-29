/* PMB7850 SoC — C port of emu/src/soc/pmb7850.py. Memory decode + a peripheral
 * registry; the peripherals themselves live under src/peripherals/ (mirroring the
 * Python soc/peripherals/ split). Dispatch is table-driven: an SFR-word->peripheral
 * index (mirror _sfr_hook), each peripheral's byte ranges (mirror _byte_ranges),
 * and a peripheral tick loop. Flash remains special only at the topology level:
 * the SoC decides which mapped flash view an access used, while flash.c owns the
 * command/read-mode semantics. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "soc.h"
#include "names.h"
#include "flash.h"
#include "external_ram.h"
#include "serial.h"
#include "ssc0.h"
#include "lcd.h"
#include "pec.h"
#include "xbus_unknown1.h"
#include "battery.h"
#include "gsm_stub.h"
#include "gsm_legacy_adapter.h"
#include "keypad.h"
#include "tdma.h"
#include "sim.h"
#include "gpt1.h"
#include "gpt2.h"
#include "capcom1.h"
#include "capcom2.h"
#include "ports.h"
#include "twi_gpio.h"
#include "synth.h"

#define IRQ55IC 0xF16Eu

/* ---- core CSFR reset values (cpu/sfr.py RESET_VALUES) ------------------ */
struct sfr_reset { uint32_t addr; uint16_t val; };
static const struct sfr_reset CORE_RESET[] = {
    {0xFE00, 0x0000}, {0xFE02, 0x0001}, {0xFE04, 0x0002}, {0xFE06, 0x0003}, /* DPP0-3 */
    {0xFE08, 0x0000},                                                       /* CSP */
    {0xFE0C, 0x0000}, {0xFE0E, 0x0000},                                     /* MDH/MDL */
    {0xFE10, 0xFC00}, {0xFE12, 0xFC00},                                     /* CP/SP */
    {0xFE14, 0xFA00}, {0xFE16, 0xFC00},                                     /* STKOV/STKUN */
    {0xFF0E, 0x0000},                                                       /* MDC */
    {0xFF10, 0x0000}, {0xFF1C, 0x0000}, {0xFF1E, 0xFFFF}, {0xFFAC, 0x0000}, /* PSW/ZEROS/ONES/TFR */
};

/* Residual deliverable-xIC nodes whose driving peripheral is NOT modeled
 * (mirror irqmap.UNMODELED_TRAP_BY_IC). Product nodes 0xF120..0xF13E are
 * intentionally absent (not deliverable). */
static const ic_node_t IC_RESIDUAL[] = {
    {0xFF9A, 0x29, 0},  /* ADEIC/IRQ35IC: firmware-triggered scheduler IRQ */
};
#define IC_RESIDUAL_N ((int)(sizeof(IC_RESIDUAL) / sizeof(IC_RESIDUAL[0])))

void cemu_soc_emit_native_trace(soc_t *s, const char *kind, int has_addr, uint32_t addr,
                    int has_size, int size, int has_value, uint32_t value,
                    const char *detail, const cemu_event_fields_t *info) {
    if (!cemu_event_native_trace_active(&s->instrumentation, kind))
        return;
    cemu_native_trace_event_t event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.icount = s->cpu ? s->cpu->icount : 0;
    event.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    event.has_addr = has_addr;
    event.addr = addr & 0xFFFFFFu;
    event.has_size = has_size;
    event.size = size;
    event.has_value = has_value;
    event.value = value;
    event.detail = detail ? detail : "";
    if (info) event.info = *info;
    cemu_event_emit_native_trace(
        &s->instrumentation, &event, CEMU_EVENT_PERIPHERAL);
}

#if !CEMU_INSTRUMENTED
#undef cemu_soc_trace_serial
#undef cemu_soc_instrument_instruction
#undef cemu_soc_instrument_idle_span
#endif

void cemu_soc_instrument_idle_span(soc_t *s, uint32_t pc, const char *detail,
                              uint64_t first_icount, uint64_t ticks) {
    if (!cemu_event_active(
            &s->instrumentation, CEMU_EVENT_INSTRUCTION))
        return;
    cemu_event_t event = { .type = CEMU_EVENT_INSTRUCTION };
    event.as.instruction.pc_before = pc & 0xFFFFFFu;
    event.as.instruction.pc_after = pc & 0xFFFFFFu;
    event.as.instruction.size = 0;
    event.as.instruction.detail = detail;
    for (uint64_t i = 0; i < ticks; i++) {
        event.as.instruction.icount = first_icount + i;
        cemu_event_emit(&s->instrumentation, &event);
    }
}

void cemu_soc_trace_serial(soc_t *s, int is_tx, uint8_t byte) {
    const char *kind = is_tx ? "serial_tx" : "serial_rx";
    if (!cemu_event_native_trace_active(&s->instrumentation, kind)) return;
    static char character[2];
    cemu_event_fields_t info;
    info.n = 0;
    if (byte >= 0x20 && byte < 0x7F) {
        character[0] = (char)byte;
        character[1] = 0;
        cemu_event_field_string(&info, "char", character);
    } else {
        cemu_event_field_null(&info, "char");
    }
    cemu_soc_emit_native_trace(s, kind, 1, is_tx ? 0xFEB0u : 0xFEB2u,
                   1, 1, 1, byte,
                   is_tx ? "S0TBUF" : "S0RBUF", &info);
}

void cemu_soc_attach_cpu(soc_t *s, cpu_t *cpu) {
    s->cpu = cpu;
    cpu->instrumentation = &s->instrumentation;
    cemu_cpu_attach_interrupt_port(cpu, &s->interrupts.port);
}

void cemu_soc_instrument_instruction(
    soc_t *s, uint32_t pc0, const char *detail, int size) {
    if (!cemu_event_active(
            &s->instrumentation, CEMU_EVENT_INSTRUCTION))
        return;
    cemu_event_t event = { .type = CEMU_EVENT_INSTRUCTION };
    event.as.instruction.pc_before = pc0 & 0xFFFFFFu;
    event.as.instruction.pc_after = s->cpu ? cpu_pc(s->cpu) : 0;
    event.as.instruction.icount = s->cpu ? s->cpu->icount : 0;
    event.as.instruction.size = size;
    event.as.instruction.detail = detail;
    cemu_event_emit(&s->instrumentation, &event);
}

cemu_status_code_t cemu_soc_serial_tx_push(soc_t *s, uint8_t byte) {
    if (!s) return CEMU_STATUS_INVALID_ARGUMENT;
    if (s->serial_tx_len == s->serial_tx_cap) {
        if (s->serial_tx_cap > SIZE_MAX / 2) {
            s->runtime_error = cemu_status_error(
                CEMU_STATUS_ALLOCATION_FAILED,
                "serial transmit size overflow");
            return s->runtime_error.code;
        }
        size_t capacity = s->serial_tx_cap ? s->serial_tx_cap * 2 : 256;
        uint8_t *replacement = cemu_realloc(s->serial_tx, capacity);
        if (!replacement) {
            s->runtime_error = cemu_status_error(
                CEMU_STATUS_ALLOCATION_FAILED,
                "cannot grow serial transmit buffer");
            return s->runtime_error.code;
        }
        s->serial_tx = replacement;
        s->serial_tx_cap = capacity;
    }
    s->serial_tx[s->serial_tx_len++] = byte;
    return CEMU_STATUS_OK;
}

void cemu_soc_tick(void *ctx, int n) {
    soc_t *s = (soc_t *)ctx;
    for (int step = 0; step < n; step++) {
        s->ticks++;
        s->capcom_t6_pulse = 0;
        for (int i = 0; i < s->n_peripherals; i++) {
            peripheral_t *p = s->peripherals[i];
            if (p->tick) p->tick(p, s, 1);
        }
    }
}

uint64_t cemu_soc_next_event_ticks(soc_t *s) {
    uint64_t earliest = UINT64_MAX;
    for (int i = 0; i < s->n_peripherals; i++) {
        peripheral_t *p = s->peripherals[i];
        if (!p->tick) continue;
        if (!p->next_event_ticks || !p->advance_quiet) return 0;
        uint64_t next = p->next_event_ticks(p, s);
        if (next == 0) return 0;
        if (next < earliest) earliest = next;
    }
    return earliest;
}

int cemu_soc_advance_quiet(soc_t *s, uint64_t ticks) {
    if (!ticks) return 0;
    for (int i = 0; i < s->n_peripherals; i++) {
        peripheral_t *p = s->peripherals[i];
        if (p->tick && (!p->next_event_ticks || !p->advance_quiet))
            return -1;
    }
    s->capcom_t6_pulse = 0;
    for (int i = 0; i < s->n_peripherals; i++) {
        peripheral_t *p = s->peripherals[i];
        if (p->tick) p->advance_quiet(p, s, ticks);
    }
    s->ticks += ticks;
    return 0;
}

int cemu_soc_idle_wake_possible(soc_t *s) {
    return cemu_interrupt_subsystem_idle_wake_possible(&s->interrupts);
}

int cemu_soc_finite_pec_will_progress(soc_t *s) {
    if (!s->cpu) return 0;
    return cemu_interrupt_subsystem_finite_pec_will_progress(
        &s->interrupts, memory_controller_sfr_get(&s->memory, 0xFF10u));
}

uint64_t cemu_soc_batch_idle(soc_t *s, uint64_t max_ticks) {
    cpu_t *cpu = s->cpu;
    interrupt_request_t request;
    if (!cpu || max_ticks < 2 || !cpu->idle || cpu->halted ||
        cpu->ext_kind != EXT_NONE || cpu->extr || cpu->ext_count ||
        cemu_interrupt_subsystem_pending(&s->interrupts, &request) ||
        !cemu_soc_idle_wake_possible(s))
        return 0;
    uint64_t next = cemu_soc_next_event_ticks(s);
    if (next == 0 || next == 1 ||
        (next == UINT64_MAX && max_ticks == UINT64_MAX)) return 0;
    uint64_t ticks = max_ticks;
    if (next != UINT64_MAX && ticks >= next) ticks = next - 1;
    if (ticks < 2 || cemu_soc_advance_quiet(s, ticks) != 0) return 0;
    cpu->icount += ticks;
    cpu->last_ran = 0;
    return ticks;
}

void cemu_soc_debug_inject_irq(soc_t *s, uint32_t addr, int trap, int ilvl) {
    addr = memory_controller_word_addr(addr);
    if (ilvl < 0) ilvl = 0;
    if (ilvl > 15) ilvl = 15;
    cemu_interrupt_subsystem_debug_inject(&s->interrupts, addr, trap, ilvl);
    if (cemu_event_active(&s->instrumentation, CEMU_EVENT_DEBUG_IRQ)) {
        cemu_event_t event = { .type = CEMU_EVENT_DEBUG_IRQ };
        event.as.debug_irq.addr = addr; event.as.debug_irq.trap = trap;
        event.as.debug_irq.ilvl = s->interrupts.debug_ilvl;
        cemu_event_emit(&s->instrumentation, &event);
    }
    uint16_t old = memory_controller_sfr_get(&s->memory, addr);
    uint16_t v = (uint16_t)((old & 0xFF00u) | XIC_IR_BIT | XIC_IE_BIT |
                            ((uint16_t)ilvl << 2) | 0x3u);
    cemu_memory_controller_sfr_put(&s->memory, addr, v);
}

static void bus_reset_core(void *ctx) { cemu_soc_reset_core((soc_t *)ctx); }
static void bus_end_init(void *ctx);
static void bus_memory_access(void *ctx, bus_transaction_t *transaction) {
    cemu_memory_controller_access(ctx, transaction);
}

/* ---- registration (mirror _register / _build_interrupt_maps) ---------- */
static cemu_status_t soc_register(soc_t *s, peripheral_t *p) {
    if (!p || s->n_peripherals >= MAX_PERIPHERALS)
        return cemu_status_error(CEMU_STATUS_TOPOLOGY_FAILED,
                                 "too many peripheral endpoints");
    if (p->n_reg_names < 0 ||
        p->n_reg_names > MAX_PERIPHERAL_NAMES - s->n_periph_names)
        return cemu_status_error(CEMU_STATUS_TOPOLOGY_FAILED,
                                 "too many peripheral register names");
    cemu_status_t status =
        cemu_memory_controller_register_endpoint(&s->memory, p);
    if (status.code != CEMU_STATUS_OK) return status;
    s->peripherals[s->n_peripherals++] = p;
    for (int i = 0; i < p->n_ic_nodes; i++) {
        if (!cemu_interrupt_subsystem_register(
                &s->interrupts, p->ic_nodes[i].addr,
                p->ic_nodes[i].trap, p->ic_nodes[i].is_timer, p)) {
            return cemu_status_error(
                CEMU_STATUS_TOPOLOGY_FAILED,
                "duplicate/invalid xIC source 0x%04x",
                (unsigned)p->ic_nodes[i].addr);
        }
    }
    /* Index this peripheral's named registers (mirror _peripheral_names). */
    for (int i = 0; i < p->n_reg_names; i++)
        s->periph_names[s->n_periph_names++] = &p->reg_names[i];
    return cemu_status_ok();
}

/* Name a register a peripheral OWNS (mirror the SFR/DPRAM subset of
 * _peripheral_names), or NULL. Only these are "modeled". */
static const char *soc_periph_name(soc_t *s, uint32_t addr) {
    addr &= 0xFFFFFF;
    for (int i = 0; i < s->n_periph_names; i++)
        if (s->periph_names[i]->addr == addr) return s->periph_names[i]->name;
    return NULL;
}

/* SFR/ESFR name, full resolution order core > peripheral > unmodeled residual,
 * else "unknown" (mirror pmb7850._sfr_name). */
const char *cemu_soc_sfr_name(soc_t *s, uint32_t addr) {
    uint32_t wa = memory_controller_word_addr(addr);
    const char *n = cemu_core_sfr_name(wa);
    if (n) return n;
    n = soc_periph_name(s, wa);
    if (n) return n;
    n = cemu_unmodeled_sfr_name(wa);
    return n ? n : "unknown";
}

/* Internal-memory cell names: peripheral > residual, else NULL. */
const char *cemu_soc_internal_io_name(soc_t *s, uint32_t addr) {
    const char *n = soc_periph_name(s, addr);
    return n ? n : cemu_unmodeled_dpram_name(addr);
}
const char *cemu_soc_dpram_name(soc_t *s, uint32_t addr) {
    const char *n = soc_periph_name(s, addr);
    return n ? n : cemu_unmodeled_dpram_name(addr);
}

/* One storage block per SoC; descriptors and all mutable controller state share
 * the same lifetime as their owning machine. */
typedef struct soc_peripheral_storage {
    flash_state_t flash_st[MAX_FLASH_CHIPS];
    external_ram_state_t external_ram_st[MAX_EXTERNAL_RAM_DEVICES];
    xbus_unknown1_state_t xbus_unknown1_st;
    keypad_state_t keypad_st;
    gpt1_state_t gpt1_st;
    gpt2_state_t gpt2_st;
    capcom1_state_t capcom1_st;
    capcom2_state_t capcom2_st;
    ports_state_t ports_st;
    twi_gpio_state_t twi_gpio_st;
    serial_state_t serial_st;
    ssc0_state_t ssc0_st;
    lcd_state_storage_t lcd_st;
    sim_state_t sim_st;
    gsm_stub_state_t gsm_stub_st;
    gsm_legacy_adapter_state_t gsm_legacy_adapter_st;
    battery_state_t battery_st;
    cemu_audio_state_storage_t audio_st;
    peripheral_t flash[MAX_FLASH_CHIPS], external_ram[MAX_EXTERNAL_RAM_DEVICES];
    peripheral_t xbus_unknown1, keypad, tdma, sim, gsm_stub;
    peripheral_t gsm_legacy_adapter, battery, serial, ssc0;
    peripheral_t gpt1, gpt2, capcom1, capcom2, ports, twi_gpio, lcd, speaker;
} soc_peripheral_storage_t;

static cemu_status_t register_default_peripherals(
    soc_t *s, const device_config_t *cfg) {
    soc_peripheral_storage_t *ps = s->peripheral_storage;
    cemu_status_t status;
#define REGISTER(peripheral) do { \
        status = soc_register(s, (peripheral)); \
        if (status.code != CEMU_STATUS_OK) return status; \
    } while (0)
    /* Order mirrors the shared topology: unknown XBUS device, keypad, TDMA, gpt2, gpt1,
     * capcom1, capcom2, ports, asc0, flash, ssc0, pec, then optional board devices.
     * Byte-range lookup is order-independent; tick order matches. */
    cemu_xbus_unknown1_periph_init(&ps->xbus_unknown1, &ps->xbus_unknown1_st,
                              cfg->xbus_unknown1_id,
                              cemu_audio_xbus_available(&s->audio));
    REGISTER(&ps->xbus_unknown1);
    s->xbus_unknown1_periph = &ps->xbus_unknown1;
    cemu_keypad_periph_init(
        &ps->keypad, &ps->keypad_st, &cfg->keypad,
        (cfg->irq55_sources & DEVICE_IRQ55_SOURCE_KEYPAD_ACTIVITY)
            ? IRQ55IC : 0);
    REGISTER(&ps->keypad);
    s->keypad_periph = &ps->keypad;
    cemu_tdma_periph_init(&ps->tdma); REGISTER(&ps->tdma);
    cemu_sim_periph_init(&ps->sim, &ps->sim_st); REGISTER(&ps->sim);
    s->sim_periph = &ps->sim;
    cemu_gsm_stub_periph_init(&ps->gsm_stub, &ps->gsm_stub_st, &cfg->baseband);
    REGISTER(&ps->gsm_stub);
    s->gsm_stub_periph = &ps->gsm_stub;
    cemu_gsm_legacy_adapter_periph_init(&ps->gsm_legacy_adapter,
                                   &ps->gsm_legacy_adapter_st,
                                   &cfg->gsm_legacy_adapter);
    REGISTER(&ps->gsm_legacy_adapter);
    s->gsm_legacy_adapter_periph = &ps->gsm_legacy_adapter;
    cemu_battery_periph_init(&ps->battery, &ps->battery_st, &cfg->battery);
    REGISTER(&ps->battery);
    s->battery_periph = &ps->battery;
    cemu_gpt2_periph_init(&ps->gpt2, &ps->gpt2_st); REGISTER(&ps->gpt2);
    cemu_gpt1_periph_init(&ps->gpt1, &ps->gpt1_st); REGISTER(&ps->gpt1);
    cemu_capcom1_periph_init(&ps->capcom1, &ps->capcom1_st); REGISTER(&ps->capcom1);
    s->capcom1_periph = &ps->capcom1;
    cemu_capcom2_periph_init(
        &ps->capcom2, &ps->capcom2_st,
        (cfg->irq55_sources & DEVICE_IRQ55_SOURCE_CC23) != 0);
    REGISTER(&ps->capcom2);
    s->capcom2_periph = &ps->capcom2;
    cemu_ports_periph_init(&ps->ports, &ps->ports_st); REGISTER(&ps->ports);
    s->ports_periph = &ps->ports;
    if (cfg->twi.available) {
        cemu_twi_gpio_periph_init(&ps->twi_gpio, &ps->twi_gpio_st, &cfg->twi);
        REGISTER(&ps->twi_gpio);
        s->twi_gpio_periph = &ps->twi_gpio;
    }
    cemu_serial_periph_init(&ps->serial, &ps->serial_st); REGISTER(&ps->serial);
    s->serial_periph = &ps->serial;
    for (int i = 0; i < cfg->flash.nchips; i++) {
        const flash_chip_config_t *chip = &cfg->flash.chips[i];
        if (!cemu_flash_periph_init(&ps->flash[i], &ps->flash_st[i], chip->model)) {
            return cemu_status_error(CEMU_STATUS_UNSUPPORTED,
                                     "unsupported flash model: %s",
                                     chip->model ? chip->model : "(null)");
        }
        ps->flash[i].id = chip->name;
        cemu_flash_configure_am29(&ps->flash_st[i], chip->protected_start,
                             chip->protected_size,
                             chip->secsi_factory_locked);
        REGISTER(&ps->flash[i]);
        status = cemu_memory_controller_register_flash(
            &s->memory, i, &ps->flash[i]);
        if (status.code != CEMU_STATUS_OK) return status;
    }
    for (int i = 0; i < cfg->external_ram.count; i++) {
        status = cemu_external_ram_periph_init(
            &ps->external_ram[i], &ps->external_ram_st[i],
            &cfg->external_ram.devices[i]);
        if (status.code != CEMU_STATUS_OK) return status;
        status = cemu_memory_controller_register_external_ram(
            &s->memory, &ps->external_ram[i]);
        if (status.code != CEMU_STATUS_OK) {
            cemu_external_ram_state_free(&ps->external_ram_st[i]);
            return status;
        }
        REGISTER(&ps->external_ram[i]);
    }
    cemu_ssc0_periph_init(&ps->ssc0, &ps->ssc0_st); REGISTER(&ps->ssc0);
    s->ssc0_periph = &ps->ssc0;
    REGISTER(&s->interrupts.pec_endpoint);
    if (cfg->lcd.model) {
        if (!cemu_lcd_periph_init(&ps->lcd, &ps->lcd_st, &cfg->lcd)) {
            return cemu_status_error(CEMU_STATUS_UNSUPPORTED,
                                     "unsupported LCD model: %s",
                                     cfg->lcd.model);
        }
        REGISTER(&ps->lcd);
        s->lcd_periph = &ps->lcd;
        cemu_ssc0_attach_slave(s->ssc0_periph,
                          cemu_lcd_ssc_start,
                          cemu_lcd_ssc_complete,
                          cemu_lcd_ssc_abort,
                          s->lcd_periph);
    }

    /* The memoryless terminal is deliberately last: it observes every source
     * transition produced during the current peripheral tick. */
    if (cemu_audio_periph_init(&s->audio, &ps->speaker, &ps->audio_st)) {
        REGISTER(&ps->speaker);
        s->speaker_periph = &ps->speaker;
    }

    /* Merge the residual (unmodeled-driver) IC nodes. */
    for (int i = 0; i < IC_RESIDUAL_N; i++) {
        if (!cemu_interrupt_subsystem_register(
                &s->interrupts, IC_RESIDUAL[i].addr,
                IC_RESIDUAL[i].trap, IC_RESIDUAL[i].is_timer, NULL)) {
            return cemu_status_error(
                CEMU_STATUS_TOPOLOGY_FAILED,
                "duplicate residual xIC source 0x%04x",
                (unsigned)IC_RESIDUAL[i].addr);
        }
    }
    return cemu_status_ok();
#undef REGISTER
}

/* ---- lifecycle -------------------------------------------------------- */
void cemu_soc_reset_core(soc_t *s) {
    int old_rstout = s->rstout;
    s->init_locked = 0;
    s->rstout = 0;
    for (int i = 0; i < s->memory.n_flash_chips; i++)
        cemu_flash_reset(cemu_memory_controller_flash_state(&s->memory, i));
    cemu_memory_controller_sfr_put(&s->memory, SYSCON_ADDR, 0);
    for (size_t i = 0; i < sizeof(CORE_RESET) / sizeof(CORE_RESET[0]); i++)
        cemu_memory_controller_sfr_put(&s->memory, CORE_RESET[i].addr, CORE_RESET[i].val);
    for (int i = 0; i < s->nstraps; i++)
        cemu_memory_controller_sfr_put(
            &s->memory, memory_controller_word_addr(s->straps[i].addr),
            s->straps[i].val);
    cemu_interrupt_subsystem_resync(&s->interrupts);
    cemu_event_fields_t info; info.n = 0;
    cemu_event_field_bool(&info, "init_locked", s->init_locked);
    cemu_event_field_bool(&info, "rstout", s->rstout);
    cemu_event_field_bool(&info, "rstout_changed", old_rstout != s->rstout);
    cemu_soc_emit_native_trace(
        s, "lifecycle", 0, 0, 0, 0, 0, 0, "reset", &info);
}

static void bus_end_init(void *ctx) {
    soc_t *s = ctx;
    int old_rstout = s->rstout;
    s->init_locked = 1;
    s->rstout = 1;
    cemu_event_fields_t info; info.n = 0;
    cemu_event_field_bool(&info, "init_locked", s->init_locked);
    cemu_event_field_bool(&info, "rstout", s->rstout);
    cemu_event_field_bool(&info, "rstout_changed", old_rstout != s->rstout);
    cemu_soc_emit_native_trace(
        s, "lifecycle", 0, 0, 0, 0, 0, 0, "einit", &info);
}

cemu_status_t cemu_soc_init(soc_t *s, const uint8_t *flash, size_t flash_len,
                       const device_config_t *cfg, unsigned synth_mask,
                       int serial_autobaud_bypass) {
    if (!s)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid SoC construction arguments");
    memset(s, 0, sizeof(*s));
    if (!cfg || (flash_len && !flash))
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid SoC construction arguments");
    cemu_event_hub_init(&s->instrumentation);
    if (cfg->straps.count < 0 || cfg->straps.count > MAX_STRAPS ||
        cfg->external_ram.count < 0 ||
        cfg->external_ram.count > MAX_EXTERNAL_RAM_DEVICES ||
        cfg->ports.count < 0 || cfg->ports.count > MAX_PORT_INPUTS) {
        return cemu_status_error(CEMU_STATUS_INVALID_CONFIGURATION,
                                 "device topology exceeds configured limits");
    }
    cemu_status_t status =
        cemu_memory_controller_init(&s->memory, s, flash, flash_len, cfg);
    if (status.code != CEMU_STATUS_OK) return status;
    s->nstraps = cfg->straps.count;
    for (int i = 0; i < cfg->straps.count; i++) {
        s->straps[i].addr = cfg->straps.entries[i].addr;
        s->straps[i].val = cfg->straps.entries[i].val;
    }
    s->synth_mask = synth_mask;
    s->serial_autobaud_bypass = serial_autobaud_bypass;
    s->serial_link = cfg->serial_link;
    cemu_audio_init(&s->audio, &cfg->audio);

    s->peripheral_storage =
        cemu_calloc(1, sizeof(soc_peripheral_storage_t));
    if (!s->peripheral_storage) {
        cemu_soc_free(s);
        return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                 "cannot allocate peripheral storage");
    }

    cemu_interrupt_subsystem_init(&s->interrupts, s);
    status = register_default_peripherals(s, cfg);
    if (status.code != CEMU_STATUS_OK) {
        cemu_soc_free(s);
        return status;
    }
    for (int i = 0; i < cfg->ports.count; i++) {
        uint16_t mask = cfg->ports.inputs[i].mask;
        for (int bit = 0; bit < 16; bit++)
            if (mask & (1u << bit))
                cemu_soc_port_input_level(s, cfg->ports.inputs[i].port, bit,
                                     (cfg->ports.inputs[i].value >> bit) & 1u);
    }
    cemu_soc_reset_core(s);

    s->bus.ctx = &s->memory;
    s->bus.access = bus_memory_access;
    s->bus.tick = cemu_soc_tick;
    s->bus.reset_core = bus_reset_core;
    s->bus.end_init = bus_end_init;
    return cemu_status_ok();
}

void cemu_soc_audio_capcom_edge(soc_t *s, int channel, int level) {
    if (s) cemu_audio_route_ringer(&s->audio, s->ticks, channel, level);
}

int cemu_soc_audio_available(const soc_t *s) {
    return s && cemu_audio_available(&s->audio);
}

void cemu_soc_audio_reset(soc_t *s) {
    if (!s) return;
    if (s->xbus_unknown1_periph)
        cemu_xbus_unknown1_audio_reset(s->xbus_unknown1_periph, s);
    cemu_audio_reset(&s->audio);
}

cemu_status_t cemu_soc_enable_stats(soc_t *s) {
    if (!s)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid statistics request");
    if (cemu_event_statistics_enabled(&s->instrumentation))
        return cemu_status_ok();
    if (s->memory.sfr_read_counts) {
        cemu_event_set_statistics(&s->instrumentation, 1);
        cemu_interrupt_subsystem_collect_cache_stats(&s->interrupts, 1);
        return cemu_status_ok();
    }
    cemu_status_t status = cemu_memory_controller_enable_stats(&s->memory);
    if (status.code != CEMU_STATUS_OK) {
        s->runtime_error = status;
        return status;
    }
    cemu_event_set_statistics(&s->instrumentation, 1);
    cemu_interrupt_subsystem_collect_cache_stats(&s->interrupts, 1);
    return cemu_status_ok();
}

void cemu_soc_disable_stats(soc_t *s) {
    cemu_event_set_statistics(&s->instrumentation, 0);
    cemu_interrupt_subsystem_collect_cache_stats(&s->interrupts, 0);
}

void cemu_soc_free(soc_t *s) {
    if (!s) return;
    cemu_audio_free(&s->audio);
    cemu_memory_controller_free(&s->memory);
    free(s->peripheral_storage);
    free(s->serial_tx); free(s->serial_rx);
    memset(s, 0, sizeof(*s));
}

cemu_status_t cemu_soc_feed_serial(soc_t *s, const uint8_t *data, size_t n) {
    if (!s || (!data && n))
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid serial receive request");
    if (s->serial_rx_head &&
        (s->serial_rx_head == s->serial_rx_len ||
         s->serial_rx_head >= s->serial_rx_cap / 2)) {
        size_t remaining = s->serial_rx_len - s->serial_rx_head;
        memmove(s->serial_rx, s->serial_rx + s->serial_rx_head, remaining);
        s->serial_rx_head = 0;
        s->serial_rx_len = remaining;
    }
    if (s->serial_rx_len + n > s->serial_rx_cap) {
        if (n > SIZE_MAX - s->serial_rx_len) {
            s->runtime_error = cemu_status_error(
                CEMU_STATUS_ALLOCATION_FAILED,
                "serial receive size overflow");
            return s->runtime_error;
        }
        size_t capacity = s->serial_rx_len + n;
        uint8_t *replacement = cemu_realloc(s->serial_rx, capacity);
        if (!replacement) {
            s->runtime_error = cemu_status_error(
                CEMU_STATUS_ALLOCATION_FAILED,
                "cannot grow serial receive buffer");
            return s->runtime_error;
        }
        s->serial_rx = replacement;
        s->serial_rx_cap = capacity;
    }
    if (n) memcpy(s->serial_rx + s->serial_rx_len, data, n);
    s->serial_rx_len += n;
    cemu_serial_rx_kick(s->serial_periph, s);
    return cemu_status_ok();
}

void cemu_soc_serial_link_attached(soc_t *s, int attached) {
    attached = attached ? 1 : 0;
    if (s->serial_link_attached == attached) return;
    s->serial_link_attached = attached;
    if (!s->serial_link.available) return;
    if (attached) {
        cemu_soc_port_input_level(s, s->serial_link.port, s->serial_link.bit,
                             s->serial_link.idle_level ? 1 : 0);
    } else {
        cemu_soc_port_input_release(s, s->serial_link.port, s->serial_link.bit);
    }
}

int cemu_soc_port_input_level(soc_t *s, int port, int bit, int level) {
    if (!s->ports_periph) return SOC_PORT_EDGE_INVALID;
    int edge = cemu_ports_input_level((ports_state_t *)s->ports_periph->state, s, port, bit, level);
    if (edge != SOC_PORT_EDGE_INVALID && edge != SOC_PORT_EDGE_NONE && port == SOC_PORT_P6 &&
        0 <= bit && bit < 8)
        cemu_soc_capcom_input_edge(s, bit, edge == SOC_PORT_EDGE_RISING);
    return edge;
}

void cemu_soc_port_input_restore_level(soc_t *s, int port, int bit, int level) {
    if (!s->ports_periph) return;
    cemu_ports_input_restore_level((ports_state_t *)s->ports_periph->state,
                              port, bit, level);
}

void cemu_soc_port_input_release(soc_t *s, int port, int bit) {
    if (!s->ports_periph) return;
    cemu_ports_input_release((ports_state_t *)s->ports_periph->state, port, bit);
}

int cemu_soc_port_input_edge(soc_t *s, int port, int bit, int rising) {
    return cemu_soc_port_input_level(s, port, bit, rising ? 1 : 0);
}

void cemu_soc_capcom_input_edge(soc_t *s, int input_id, int rising) {
    if (input_id == SOC_CAPCOM_INPUT_T0IN) {
        cemu_capcom1_inject_timer_edge((capcom1_state_t *)s->capcom1_periph->state, s, 0, rising);
        return;
    }
    if (input_id == SOC_CAPCOM_INPUT_T7IN) {
        cemu_capcom2_inject_timer_edge((capcom2_state_t *)s->capcom2_periph->state, s, 0, rising);
        return;
    }
    if (0 <= input_id && input_id < 16) {
        cemu_capcom1_inject_channel_edge((capcom1_state_t *)s->capcom1_periph->state, s, input_id, rising);
        return;
    }
    if (16 <= input_id && input_id < 32)
        cemu_capcom2_inject_channel_edge((capcom2_state_t *)s->capcom2_periph->state, s, input_id - 16, rising);
}

int cemu_soc_capcom_output_level(soc_t *s, int channel) {
    if (0 <= channel && channel < 16)
        return cemu_capcom1_output_level((capcom1_state_t *)s->capcom1_periph->state, channel);
    if (16 <= channel && channel < 32)
        return cemu_capcom2_output_level((capcom2_state_t *)s->capcom2_periph->state, channel - 16);
    return 0;
}

int cemu_soc_capcom_output_driven(soc_t *s, int channel) {
    if (0 <= channel && channel < 16)
        return cemu_capcom1_output_driven(s, channel);
    if (16 <= channel && channel < 32)
        return cemu_capcom2_output_driven(s, channel - 16);
    return 0;
}

void cemu_soc_xbus_unknown1_counts(soc_t *s, uint64_t *id_reads,
                              uint64_t *status_reads,
                              uint64_t *doorbell_rings) {
    xbus_unknown1_state_t *st =
        (xbus_unknown1_state_t *)s->xbus_unknown1_periph->state;
    if (id_reads) *id_reads = st->id_reads;
    if (status_reads) *status_reads = st->status_reads;
    if (doorbell_rings) *doorbell_rings = st->doorbell_rings;
}

/* ---- observability: modeled predicate, interrupts, vectors ------------- */
int cemu_soc_sfr_modeled(soc_t *s, uint32_t addr) {
    /* Modeled iff a core CSFR OR a peripheral NAMES this register (its
     * reg_names / REGISTER_NAMES). NOT "a peripheral claims/hooks it" — a
     * claimed-but-unnamed word such as the BSL flag at 0xFF10 aliasing PSW has
     * no peripheral name, so it is not colored as peripheral-modeled. */
    uint32_t wa = memory_controller_word_addr(addr);
    return cemu_core_sfr_name(wa) != NULL || soc_periph_name(s, wa) != NULL;
}

/* Enumerate armed xIC registers from the merged IC table (mirror
 * armed_interrupts). The IC table is the C analogue of IC_REGISTERS (peripheral
 * ic_nodes + residual), so this scans exactly the deliverable-xIC set. */
int cemu_soc_armed_interrupts(soc_t *s, int pending_only, armed_ic_t *out, int cap) {
    int n = 0;
    interrupt_subsystem_t *ic = &s->interrupts;
    for (int i = 0; i < ic->n_sources && n < cap; i++) {
        uint32_t addr = ic->sources[i].addr;
        uint16_t v = ic->source_values[i];
        int ir = (v >> 7) & 1, ie = (v >> 6) & 1;
        int keep = pending_only ? (ir == 1) : (ie == 1);
        if (!keep) continue;
        out[n].addr = addr;
        out[n].name = cemu_soc_sfr_name(s, addr);
        out[n].value = v;
        out[n].ir = ir; out[n].ie = ie;
        out[n].ilvl = (v >> 2) & 0xF; out[n].glvl = v & 0x3;
        out[n].trap = ic->sources[i].has_trap ? ic->sources[i].trap : -1;
        out[n].has_trap = ic->sources[i].has_trap;
        n++;
    }
    /* sort by (ilvl, extended glvl) descending — arbitration order */
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (out[b].ilvl > out[a].ilvl ||
                (out[b].ilvl == out[a].ilvl &&
                 ((((out[b].value >> 8) & 1) << 2) | out[b].glvl) >
                 ((((out[a].value >> 8) & 1) << 2) | out[a].glvl))) {
                armed_ic_t t = out[a]; out[a] = out[b]; out[b] = t;
            }
    return n;
}

int cemu_soc_awaited_timer_will_fire(soc_t *s, const uint32_t *polled, int n) {
    return cemu_interrupt_subsystem_awaited_source_will_fire(
        &s->interrupts, polled, n);
}

/* Trap short label (mirror irqmap.trap_name / TRAP_NAMES). */
void cemu_soc_trap_name(int trap, char *buf, int cap) {
    const char *known = NULL;
    switch (trap) {
        case 0x00: known = "RESET"; break;
        case 0x02: known = "NMI"; break;
        case 0x04: known = "STKOV"; break;
        case 0x06: known = "STKUN"; break;
        case 0x08: known = "SBRK"; break;
        case 0x0A: known = "BTRAP"; break;
        case 0x0E: known = "ILLACC"; break;
    }
    if (known) snprintf(buf, cap, "%s", known);
    else snprintf(buf, cap, "TRAP#%#x", trap);
}

/* Condition-code mnemonics (M166) for describe_vector's cc-bearing forms. */
static const char *cc_name(int cc) {
    static const char *T[16] = {
        "cc_UC","cc_NET","cc_EQ","cc_NE","cc_V","cc_NV","cc_N","cc_NN",
        "cc_C","cc_NC","cc_SGT","cc_SLE","cc_SLT","cc_SGE","cc_UGT","cc_ULE"};
    return T[cc & 0xF];
}
static int sext8(int v) { return (v ^ 0x80) - 0x80; }

/* Describe the handler installed at trap*4 (mirror irqmap.describe_vector). */
void cemu_soc_describe_vector(soc_t *s, int trap, char *buf, int cap) {
    if (trap < 0) { snprintf(buf, cap, "n/a (trap# not in C166S table)"); return; }
    uint32_t base = (trap * 4) & 0xFFFF;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = cemu_memory_controller_peek8(&s->memory, base + i);
    uint8_t op = b[0];
    uint16_t off = cemu_memory_controller_peek16(&s->memory, base + 2);
    if (op == 0xFA || op == 0xDA) {                 /* JMPS / CALLS far seg:off */
        const char *mn = op == 0xFA ? "JMPS" : "CALLS";
        int seg = b[1];
        snprintf(buf, cap, "%s %#04x:%#06x -> %#08x", mn, seg, off, ((seg << 16) | off));
        return;
    }
    if (op == 0xEA || op == 0xCA) {                 /* JMPA / CALLA near cc,off */
        const char *mn = op == 0xEA ? "JMPA" : "CALLA";
        snprintf(buf, cap, "%s %s,%#06x -> %#08x", mn, cc_name((b[1] >> 4) & 0xF), off,
                 (base & 0xFF0000) | off);
        return;
    }
    if ((op & 0x0F) == 0x0D || op == 0xBB) {        /* JMPR cc,rel / CALLR rel */
        int rel = b[1];
        uint32_t tgt = (base + 2 + sext8(rel) * 2) & 0xFFFF;
        if (op == 0xBB) snprintf(buf, cap, "CALLR %#06x -> %#08x", tgt, (base & 0xFF0000) | tgt);
        else snprintf(buf, cap, "JMPR %s,%#06x -> %#08x", cc_name((op >> 4) & 0xF), tgt,
                      (base & 0xFF0000) | tgt);
        return;
    }
    if (op == 0x9C || op == 0xAB) {                 /* JMPI / CALLI cc,[rwm] */
        const char *mn = op == 0x9C ? "JMPI" : "CALLI";
        snprintf(buf, cap, "%s %s,[r%d] (indirect — target set at runtime)",
                 mn, cc_name((b[1] >> 4) & 0xF), b[1] & 0xF);
        return;
    }
    const char *ret = (op == 0xFB) ? "RETI" : (op == 0xCB) ? "RET" : (op == 0xDB) ? "RETS" : NULL;
    if (ret) { snprintf(buf, cap, "%s @ %#08x (null handler — returns immediately)", ret, base & 0xFFFFFF); return; }
    if (b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF && b[3] == 0xFF) {
        snprintf(buf, cap, "unfilled (0xFF fill — no handler installed)"); return;
    }
    snprintf(buf, cap, "inline handler @ %#08x", base & 0xFFFFFF);
}
