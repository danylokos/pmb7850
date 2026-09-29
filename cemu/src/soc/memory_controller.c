/* PMB7850/C166S memory storage, routing, and bus-facing dispatch. */
#include "memory_controller.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc.h"
#include "names.h"
#include "flash.h"
#include "external_ram.h"

/* ---- address-class predicates (pmb7850.py) ---------------------------- */
static inline uint32_t norm(uint32_t a) { return a & 0xFFFFFF; }
static inline uint32_t sfr_word_addr(uint32_t a) { return a & 0xFFFFFE; }

static inline int is_sfr_addr(uint32_t a) {
    a = norm(a);
    return (ESFR_START <= a && a < ESFR_END) || (SFR_START <= a && a < SFR_END);
}
static inline int is_internal_io_addr(uint32_t a) {
    a = norm(a);
    return INTERNAL_IO_START <= a && a < INTERNAL_IO_END;
}
static inline int is_dpram_addr(uint32_t a) {
    a = norm(a);
    return DPRAM_START <= a && a < DPRAM_END;
}
static inline int is_internal_memory(uint32_t a) {
    return is_internal_io_addr(a) || is_dpram_addr(a);
}

cemu_status_t cemu_memory_controller_init(
    memory_controller_t *mc, soc_t *soc, const uint8_t *flash,
    size_t flash_len, const device_config_t *cfg) {
    if (!mc)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid memory-controller arguments");
    memset(mc, 0, sizeof(*mc));
    if (!cfg || (flash_len && !flash))
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid memory-controller arguments");
    if (cfg->flash.nchips < 0 || cfg->flash.nchips > MAX_FLASH_CHIPS ||
        cfg->flash.nwindows < 0 || cfg->flash.nwindows > MAX_FLASH_WINDOWS)
        return cemu_status_error(CEMU_STATUS_INVALID_CONFIGURATION,
                                 "flash topology exceeds configured limits");
    mc->soc = soc;
    mc->flash_data = flash;
    mc->flash_len = flash_len;
    mc->n_flash_chips = cfg->flash.nchips;
    for (int i = 0; i < mc->n_flash_chips; i++) {
        mc->flash_chips[i] = cfg->flash.chips[i];
        mc->flash_file_offsets[i] =
            cfg->flash_image.count == cfg->flash.nchips
                ? cfg->flash_image.file_offsets[i] : 0;
    }
    if (mc->n_flash_chips > 1 &&
        cfg->flash_image.count != cfg->flash.nchips) {
        return cemu_status_error(
            CEMU_STATUS_INVALID_CONFIGURATION,
            "unresolved multi-chip flash image mapping");
    }
    mc->n_flash_windows = cfg->flash.nwindows;
    for (int i = 0; i < mc->n_flash_windows; i++)
        mc->flash_windows[i] = cfg->flash.windows[i];
    mc->ram = cemu_calloc(ADDR_SPACE, 1);
    mc->present = cemu_calloc(ADDR_SPACE, 1);
    mc->lm_size = LM_SIZE;
    mc->lm = cemu_calloc(mc->lm_size, 1);
    if (!mc->ram || !mc->present || !mc->lm) {
        cemu_memory_controller_free(mc);
        return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                 "cannot allocate core memory backing");
    }
    return cemu_status_ok();
}

cemu_status_t cemu_memory_controller_register_endpoint(
    memory_controller_t *mc, peripheral_t *peripheral) {
    if (!mc || !peripheral)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid peripheral endpoint");
    if (peripheral->n_byte_ranges > 0 &&
        mc->n_byte_endpoints >= MAX_PERIPHERALS)
        return cemu_status_error(CEMU_STATUS_TOPOLOGY_FAILED,
                                 "too many byte-range endpoints");
    for (int i = 0; i < peripheral->n_sfr_words; i++)
        mc->sfr_hook[memory_controller_sfr_index(
            peripheral->sfr_words[i])] = peripheral;
    if (peripheral->n_byte_ranges > 0) {
        mc->byte_endpoints[mc->n_byte_endpoints++] = peripheral;
    }
    return cemu_status_ok();
}

cemu_status_t cemu_memory_controller_register_flash(
    memory_controller_t *mc, int chip_index, peripheral_t *peripheral) {
    if (!mc || !peripheral || chip_index < 0 ||
        chip_index >= mc->n_flash_chips)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid flash endpoint index %d", chip_index);
    mc->flash_endpoints[chip_index] = peripheral;
    return cemu_status_ok();
}

cemu_status_t cemu_memory_controller_register_external_ram(
    memory_controller_t *mc, peripheral_t *peripheral) {
    if (!mc || !peripheral)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid external-RAM endpoint");
    if (mc->n_external_ram >= MAX_EXTERNAL_RAM_DEVICES)
        return cemu_status_error(CEMU_STATUS_TOPOLOGY_FAILED,
                                 "too many external-RAM endpoints");
    mc->external_ram_endpoints[mc->n_external_ram++] = peripheral;
    return cemu_status_ok();
}

void cemu_memory_controller_sfr_put(
    memory_controller_t *mc, uint32_t word_addr, uint16_t value) {
    word_addr = sfr_word_addr(word_addr);
    int index = memory_controller_sfr_index(word_addr);
    uint16_t old_value = mc->sfr[index];
    mc->sfr[index] = value;
    if (mc->soc)
        cemu_interrupt_subsystem_sfr_changed(
            &mc->soc->interrupts, word_addr, old_value, value);
}

cemu_status_t cemu_memory_controller_enable_stats(memory_controller_t *mc) {
    if (!mc)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid statistics request");
    if (mc->sfr_read_counts) return cemu_status_ok();
    uint32_t *sfr_reads = cemu_calloc(SFR_WORDS, sizeof(uint32_t));
    uint32_t *sfr_writes = cemu_calloc(SFR_WORDS, sizeof(uint32_t));
    uint32_t *internal_reads =
        cemu_calloc(INTERNAL_IO_WORDS, sizeof(uint32_t));
    uint32_t *internal_writes =
        cemu_calloc(INTERNAL_IO_WORDS, sizeof(uint32_t));
    uint32_t *dpram_reads = cemu_calloc(DPRAM_WORDS, sizeof(uint32_t));
    uint32_t *dpram_writes = cemu_calloc(DPRAM_WORDS, sizeof(uint32_t));
    uint8_t *write_blocks = cemu_calloc(65536 / 8, 1);
    if (!sfr_reads || !sfr_writes || !internal_reads || !internal_writes ||
        !dpram_reads || !dpram_writes || !write_blocks) {
        free(sfr_reads); free(sfr_writes);
        free(internal_reads); free(internal_writes);
        free(dpram_reads); free(dpram_writes); free(write_blocks);
        return cemu_status_error(CEMU_STATUS_ALLOCATION_FAILED,
                                 "cannot allocate statistics counters");
    }
    mc->sfr_read_counts = sfr_reads;
    mc->sfr_write_counts = sfr_writes;
    mc->internal_io_read_counts = internal_reads;
    mc->internal_io_write_counts = internal_writes;
    mc->dpram_read_counts = dpram_reads;
    mc->dpram_write_counts = dpram_writes;
    mc->mem_write_blocks = write_blocks;
    return cemu_status_ok();
}

void cemu_memory_controller_free(memory_controller_t *mc) {
    for (int i = 0; i < mc->n_flash_chips; i++) {
        flash_state_t *state = cemu_memory_controller_flash_state(mc, i);
        if (state) cemu_flash_state_free(state);
    }
    for (int i = 0; i < mc->n_external_ram; i++) {
        peripheral_t *endpoint = mc->external_ram_endpoints[i];
        if (endpoint)
            cemu_external_ram_state_free((external_ram_state_t *)endpoint->state);
    }
    free(mc->ram);
    free(mc->present);
    free(mc->lm);
    free(mc->sfr_read_counts);
    free(mc->sfr_write_counts);
    free(mc->internal_io_read_counts);
    free(mc->internal_io_write_counts);
    free(mc->dpram_read_counts);
    free(mc->dpram_write_counts);
    free(mc->mem_write_blocks);
    memset(mc, 0, sizeof(*mc));
}

int cemu_memory_controller_flash_translate(
    const memory_controller_t *mc, uint32_t addr,
    memory_flash_target_t *target_out) {
    addr = norm(addr);
    if (!mc->flash_data) return 0;
    for (int i = 0; i < mc->n_flash_windows; i++) {
        const flash_window_config_t *w = &mc->flash_windows[i];
        if (w->cpu_base <= addr &&
            (uint64_t)addr < (uint64_t)w->cpu_base + w->cpu_size) {
            uint32_t off = w->chip_base +
                           ((addr - w->cpu_base) % w->mirror_period);
            if (w->chip_index < 0 || w->chip_index >= mc->n_flash_chips ||
                off >= mc->flash_chips[w->chip_index].chip_size)
                return 0;
            if (target_out) {
                target_out->chip_index = w->chip_index;
                target_out->chip_offset = off;
                target_out->command_visible = w->command_visible;
            }
            return 1;
        }
    }
    return 0;
}

flash_state_t *cemu_memory_controller_flash_state(
    const memory_controller_t *mc, int chip_index) {
    if (!mc || chip_index < 0 || chip_index >= mc->n_flash_chips ||
        !mc->flash_endpoints[chip_index])
        return NULL;
    return (flash_state_t *)mc->flash_endpoints[chip_index]->state;
}

static void flash_chip_data(const memory_controller_t *mc, int chip_index,
                            const uint8_t **data, size_t *len) {
    const flash_chip_config_t *chip = &mc->flash_chips[chip_index];
    size_t file_offset = mc->flash_file_offsets[chip_index];
    size_t available = file_offset < mc->flash_len
                     ? mc->flash_len - file_offset : 0;
    *data = available ? mc->flash_data + file_offset : mc->flash_data;
    *len = available < chip->chip_size ? available : chip->chip_size;
}

uint8_t cemu_memory_controller_flash_dump_read8(
    const memory_controller_t *mc, size_t file_offset) {
    if (!mc || !mc->flash_data || file_offset >= mc->flash_len) return 0xFF;
    for (int i = 0; i < mc->n_flash_chips; i++) {
        const flash_chip_config_t *chip = &mc->flash_chips[i];
        size_t chip_file_offset = mc->flash_file_offsets[i];
        if (file_offset >= chip_file_offset &&
            file_offset - chip_file_offset < chip->chip_size) {
            const uint8_t *data;
            size_t len;
            flash_chip_data(mc, i, &data, &len);
            return cemu_flash_array_read8(
                data, len, cemu_memory_controller_flash_state(mc, i),
                (uint32_t)(file_offset - chip_file_offset));
        }
    }
    return mc->flash_data[file_offset];
}

cemu_status_t cemu_memory_controller_flash_array_copy(
        const memory_controller_t *mc, int chip_index, size_t offset,
        uint8_t *bytes, size_t size) {
    if (!mc || chip_index < 0 || chip_index >= mc->n_flash_chips ||
        (!bytes && size))
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "invalid flash main-array copy");
    const flash_chip_config_t *chip = &mc->flash_chips[chip_index];
    if (offset > chip->chip_size || size > chip->chip_size - offset)
        return cemu_status_error(CEMU_STATUS_INVALID_ARGUMENT,
                                 "flash main-array copy is out of range");
    const uint8_t *data;
    size_t len;
    flash_chip_data(mc, chip_index, &data, &len);
    const flash_state_t *state =
        cemu_memory_controller_flash_state(mc, chip_index);
    if (!state)
        return cemu_status_error(CEMU_STATUS_INVALID_CONFIGURATION,
                                 "flash main-array endpoint is unavailable");
    for (size_t i = 0; i < size; i++)
        bytes[i] = cemu_flash_array_read8(
            data, len, state, (uint32_t)(offset + i));
    return cemu_status_ok();
}

int cemu_memory_controller_lm_translate(
    const memory_controller_t *mc, uint32_t addr, uint32_t *off_out) {
    addr = norm(addr);
    uint16_t syscon = memory_controller_sfr_get(mc, SYSCON_ADDR);
    if (!(syscon & SYSCON_ROMEN)) return 0;

    uint32_t off;
    if (syscon & SYSCON_ROMS1) {
        if (addr < LM_SEG1_START || addr >= LM_SEG1_END) return 0;
        off = addr - LM_SEG1_START;
    } else if (addr < LM_LOW_SIZE) {
        off = addr;
    } else {
        if (addr < LM_HIGH_START || addr >= LM_SEG1_END) return 0;
        off = addr - LM_SEG1_START;
    }
    if (off >= mc->lm_size) return 0;
    if (off_out) *off_out = off;
    return 1;
}

/* ---- peripheral dispatch helpers -------------------------------------- */
/* The peripheral claiming `addr` in a byte range, or NULL (mirror
 * _byte_range_peripheral). Flash is excluded (direct-call special case). */
static peripheral_t *byte_range_peripheral(memory_controller_t *mc,
                                           uint32_t addr) {
    for (int i = 0; i < mc->n_byte_endpoints; i++) {
        peripheral_t *p = mc->byte_endpoints[i];
        for (int r = 0; r < p->n_byte_ranges; r++)
            if (p->byte_ranges[r].start <= addr && addr <= p->byte_ranges[r].end)
                return p;
    }
    return NULL;
}

static flash_state_t *flash_st(
    const memory_controller_t *mc, int chip_index) {
    return cemu_memory_controller_flash_state(mc, chip_index);
}

/* ======================================================================= */
/* Native observability is skipped when no matching consumer exists. */
/* Mirrors pmb7850._event / _xbus_access_info / xbus_window.                */
/* ======================================================================= */
/* Decoded bus-controller windows. XBUS is the current behavior surface;
 * ADDRSEL/BUSCON and BUSCON0 are decode-only observability metadata. */
static const int ADDRSEL_PRIORITY[4] = {2, 4, 1, 3};

const char *cemu_memory_bus_window_kind_name(memory_bus_window_kind_t kind) {
    switch (kind) {
        case MEMORY_BUS_WINDOW_XBUS: return "xbus";
        case MEMORY_BUS_WINDOW_ADDRSEL: return "addrsel";
        case MEMORY_BUS_WINDOW_BUSCON0: return "buscon0";
    }
    return "unknown";
}

static int valid_bus_window_kind(memory_bus_window_kind_t kind, int index) {
    if (kind == MEMORY_BUS_WINDOW_XBUS) return 1 <= index && index <= 6;
    if (kind == MEMORY_BUS_WINDOW_ADDRSEL) return 1 <= index && index <= 4;
    if (kind == MEMORY_BUS_WINDOW_BUSCON0) return index == 0;
    return 0;
}

static void decode_control_fields(memory_bus_window_info_t *w) {
    uint16_t c = w->control;
    w->mctc = c & 0x000F;
    w->rwdc = (c >> 4) & 1;
    w->mttc = (c >> 5) & 1;
    w->btyp = (c >> 6) & 3;
    w->ewen = (c >> 8) & 1;
    w->alectl = (c >> 9) & 1;
    w->bus_active = (c & BUSCON_BUSACT) != 0;
    w->bswc = (c >> 11) & 1;
    w->rdyen = (c >> 12) & 1;
    w->csren = (c >> 14) & 1;
    w->cswen = (c >> 15) & 1;
}

static void decode_range_fields(memory_bus_window_info_t *w, int xbus_small) {
    w->rgsz = w->selector & 0x000F;
    w->rgsad = (w->selector >> 4) & 0x0FFF;
    w->start = w->end = w->size = -1;
    w->reserved = w->rgsz >= 12;
    if (w->reserved) return;
    uint32_t size = (xbus_small ? 0x100u : 0x1000u) << w->rgsz;
    uint32_t start = (uint32_t)w->rgsad << (xbus_small ? 8 : 12);
    start &= ~(size - 1u);
    w->start = start & 0xFFFFFFu;
    w->end = (start + size - 1u) & 0xFFFFFFu;
    w->size = size;
}

static void finish_bus_window_reason(memory_bus_window_info_t *w) {
    w->reason[0] = 0;
    if (!w->configured || w->active) return;
    if (w->reserved) snprintf(w->reason, sizeof w->reason, "reserved");
    else if (w->kind == MEMORY_BUS_WINDOW_XBUS && !w->xpen) snprintf(w->reason, sizeof w->reason, "XPEN not set");
    else if (w->kind == MEMORY_BUS_WINDOW_XBUS && !w->xper_enabled) snprintf(w->reason, sizeof w->reason, "XPERCON not set");
    else if (!w->bus_active) snprintf(w->reason, sizeof w->reason, "BUSACT not set");
}

int cemu_memory_controller_bus_window_decode(
    memory_controller_t *mc, memory_bus_window_kind_t kind, int index,
    memory_bus_window_info_t *out) {
    if (!out || !valid_bus_window_kind(kind, index)) return 0;
    memory_bus_window_info_t w;
    memset(&w, 0, sizeof w);
    w.kind = kind;
    w.index = index;
    w.start = w.end = w.size = -1;
    w.rgsad = w.rgsz = -1;

    if (kind == MEMORY_BUS_WINDOW_XBUS) {
        w.selector_addr = XADRS1_ADDR + (uint32_t)(index - 1) * 2u;
        w.control_addr = XBCON1_ADDR + (uint32_t)(index - 1) * 2u;
        w.selector = memory_controller_sfr_get(mc, w.selector_addr);
        w.control = memory_controller_sfr_get(mc, w.control_addr);
        decode_range_fields(&w, index <= 4);
        w.xpen =
            (memory_controller_sfr_get(mc, SYSCON_ADDR) & SYSCON_XPEN) != 0;
        w.xper_enabled =
            (memory_controller_sfr_get(mc, XPERCON_ADDR) &
             (1u << (index - 1))) != 0;
        w.configured = (w.selector != 0 || w.control != 0);
        decode_control_fields(&w);
        w.active = w.configured && !w.reserved && w.xpen && w.xper_enabled && w.bus_active;
    } else if (kind == MEMORY_BUS_WINDOW_ADDRSEL) {
        w.selector_addr = ADDRSEL1_ADDR + (uint32_t)(index - 1) * 2u;
        w.control_addr = BUSCON1_ADDR + (uint32_t)(index - 1) * 2u;
        w.selector = memory_controller_sfr_get(mc, w.selector_addr);
        w.control = memory_controller_sfr_get(mc, w.control_addr);
        decode_range_fields(&w, 0);
        w.configured = (w.selector != 0 || w.control != 0);
        decode_control_fields(&w);
        w.active = w.configured && !w.reserved && w.bus_active;
    } else {
        w.control_addr = BUSCON0_ADDR;
        w.control = memory_controller_sfr_get(mc, BUSCON0_ADDR);
        w.start = 0;
        w.end = 0xFFFFFF;
        w.size = 0x1000000;
        w.configured = w.control != 0;
        decode_control_fields(&w);
        w.active = w.configured && w.bus_active;
    }
    finish_bus_window_reason(&w);
    *out = w;
    return 1;
}

static int bus_window_contains(const memory_bus_window_info_t *w, uint32_t addr) {
    if (w->start < 0 || w->end < 0) return 0;
    addr &= 0xFFFFFFu;
    return (uint32_t)w->start <= addr && addr <= (uint32_t)w->end;
}

static peripheral_t *external_ram_resolve(
    memory_controller_t *mc, uint32_t addr, uint32_t *off_out) {
    for (int p = 0; p < 4; p++) {
        peripheral_t *ram = NULL;
        external_ram_state_t *st = NULL;
        for (int i = 0; i < mc->n_external_ram; i++) {
            peripheral_t *candidate = mc->external_ram_endpoints[i];
            external_ram_state_t *candidate_st =
                (external_ram_state_t *)candidate->state;
            if (candidate_st->addrsel_index != ADDRSEL_PRIORITY[p]) continue;
            ram = candidate;
            st = candidate_st;
            break;
        }
        if (!ram) continue;
        memory_bus_window_info_t w = {0};
        if (!cemu_memory_controller_bus_window_decode(
                mc, MEMORY_BUS_WINDOW_ADDRSEL, ADDRSEL_PRIORITY[p], &w))
            continue;
        if (!w.active || !bus_window_contains(&w, addr)) continue;
        if (off_out)
            *off_out = cemu_external_ram_offset(st, (uint32_t)w.start, addr);
        return ram;
    }
    return NULL;
}

/* First XBUS window containing `addr`, preserving the legacy match semantics
 * used by route labels and xbus_access events. */
static int xbus_match(
    memory_controller_t *mc, uint32_t addr, memory_bus_window_info_t *out) {
    for (int i = 1; i <= 6; i++) {
        memory_bus_window_info_t w;
        cemu_memory_controller_bus_window_decode(
            mc, MEMORY_BUS_WINDOW_XBUS, i, &w);
        if (bus_window_contains(&w, addr)) { if (out) *out = w; return 1; }
    }
    return 0;
}

int cemu_memory_controller_bus_route_decode(
    memory_controller_t *mc, uint32_t addr,
    memory_bus_window_info_t *out) {
    addr &= 0xFFFFFFu;
    memory_bus_window_info_t w;
    for (int i = 1; i <= 6; i++) {
        cemu_memory_controller_bus_window_decode(
            mc, MEMORY_BUS_WINDOW_XBUS, i, &w);
        if (w.configured && bus_window_contains(&w, addr)) { if (out) *out = w; return 1; }
    }
    if (is_sfr_addr(addr) || is_dpram_addr(addr) ||
        cemu_memory_controller_lm_translate(mc, addr, NULL))
        return 0;
    /* PMB internal I/O can carry explicit XBUS windows, but must never fall
     * through to generic ADDRSEL/BUSCON0 external routing. */
    if (is_internal_io_addr(addr)) return 0;
    for (int p = 0; p < 4; p++) {
        cemu_memory_controller_bus_window_decode(
            mc, MEMORY_BUS_WINDOW_ADDRSEL, ADDRSEL_PRIORITY[p], &w);
        if (w.configured && bus_window_contains(&w, addr)) { if (out) *out = w; return 1; }
    }
    cemu_memory_controller_bus_window_decode(
        mc, MEMORY_BUS_WINDOW_BUSCON0, 0, &w);
    if (w.configured) { if (out) *out = w; return 1; }
    return 0;
}

static void append_bus_route_info(soc_t *s, uint32_t addr, cemu_event_fields_t *info,
                                  int include_legacy_xbus) {
    memory_bus_window_info_t w;
    if (!cemu_memory_controller_bus_route_decode(&s->memory, addr, &w)) return;
    /* bus_* keys first; callers append device/mode/subtype before any xbus_* if
     * they need strict JSON sort order across mixed info dictionaries. */
    cemu_event_field_bool(info, "bus_active", w.bus_active);
    if (w.end >= 0) cemu_event_field_i64(info, "bus_end", w.end); else cemu_event_field_null(info, "bus_end");
    cemu_event_field_i64(info, "bus_index", w.index);
    cemu_event_field_string(info, "bus_kind", cemu_memory_bus_window_kind_name(w.kind));
    if (w.start >= 0) cemu_event_field_i64(info, "bus_start", w.start); else cemu_event_field_null(info, "bus_start");
    cemu_event_field_bool(info, "bus_window_active", w.active);
    if (include_legacy_xbus && w.kind == MEMORY_BUS_WINDOW_XBUS) {
        cemu_event_field_bool(info, "xbus_active", w.active);
        cemu_event_field_bool(info, "xbus_bus_active", w.bus_active);
        if (w.end >= 0) cemu_event_field_i64(info, "xbus_end", w.end); else cemu_event_field_null(info, "xbus_end");
        if (w.start >= 0) cemu_event_field_i64(info, "xbus_start", w.start); else cemu_event_field_null(info, "xbus_start");
        cemu_event_field_i64(info, "xbus_window", w.index);
        cemu_event_field_bool(info, "xbus_xpen", w.xpen);
        cemu_event_field_bool(info, "xbus_xper_enabled", w.xper_enabled);
    }
}

/* Build XBUS route metadata only when a recorded memory event can consume it.
 * Debugger bus consumers receive transactions directly and need no info dict. */
static void fill_xbus_access_info(soc_t *s, uint32_t addr, cemu_event_fields_t *info) {
    info->n = 0;
    if (!cemu_event_native_trace_active(&s->instrumentation, "mem_read") &&
        !cemu_event_native_trace_active(&s->instrumentation, "mem_write") &&
        !cemu_event_native_trace_active(&s->instrumentation, "unmapped"))
        return;
    append_bus_route_info(s, addr, info, 1);
}

/* Build the xbus_access event info. Unlike generic mem events, this fires only
 * for configured XBUS windows so --trace xbus is a protocol stream, not address noise. */
static int fill_xbus_access_event_info(soc_t *s, uint32_t addr, const char *access,
                                       cemu_event_fields_t *info) {
    memory_bus_window_info_t w;
    info->n = 0;
    if (!xbus_match(&s->memory, addr, &w) || !w.configured) return 0;
    /* alphabetical: access, then the existing xbus_* keys. */
    cemu_event_field_string(info, "access", access);
    cemu_event_field_bool(info, "xbus_active", w.active);
    cemu_event_field_bool(info, "xbus_bus_active", w.bus_active);
    if (w.end >= 0) cemu_event_field_i64(info, "xbus_end", w.end); else cemu_event_field_null(info, "xbus_end");
    if (w.start >= 0) cemu_event_field_i64(info, "xbus_start", w.start); else cemu_event_field_null(info, "xbus_start");
    cemu_event_field_i64(info, "xbus_window", w.index);
    cemu_event_field_bool(info, "xbus_xpen", w.xpen);
    cemu_event_field_bool(info, "xbus_xper_enabled", w.xper_enabled);
    return 1;
}

/* Count one access landing in a configured bus-controller window. */
static void note_ebi_access(soc_t *s, uint32_t addr) {
    memory_bus_window_info_t w;
    if (!cemu_memory_controller_bus_route_decode(&s->memory, addr, &w)) return;
    if (w.kind == MEMORY_BUS_WINDOW_XBUS) {
        s->memory.xbus_configured_access[w.index & 7]++;
        if (!w.active) return;
        s->memory.xbus_window_access[w.index & 7]++;
        if (byte_range_peripheral(&s->memory, addr) == NULL) s->memory.xbus_unmodeled_access[w.index & 7]++;
    } else if (w.kind == MEMORY_BUS_WINDOW_ADDRSEL) {
        s->memory.addrsel_configured_access[w.index]++;
        if (w.active) s->memory.addrsel_window_access[w.index]++;
    } else if (w.kind == MEMORY_BUS_WINDOW_BUSCON0) {
        s->memory.buscon0_access++;
    }
}

/* Emit one event stamped with the CPU's live icount/pc (mirror _event). */
/* ======================================================================= */
/* Memory decode — unified through one transaction resolver/dispatcher.      */
/* ======================================================================= */

typedef enum {
    MEMORY_ROUTE_SFR = 0,
    MEMORY_ROUTE_RAM,
    MEMORY_ROUTE_LM,
    MEMORY_ROUTE_EXTERNAL_RAM,
    MEMORY_ROUTE_FLASH,
    MEMORY_ROUTE_UNMAPPED,
} memory_route_t;

typedef struct {
    memory_route_t route;
    peripheral_t *periph;
#if CEMU_INSTRUMENTED
    bus_route_kind_t route_kind;
#endif
    memory_flash_target_t flash;
    int ram_present;
    uint32_t lm_off;
    uint32_t external_ram_off;
} memory_resolution_t;

#if CEMU_INSTRUMENTED
static bus_route_kind_t resolved_route_kind(soc_t *s, uint32_t addr) {
    if (cemu_memory_controller_lm_translate(&s->memory, addr, NULL)) return BUS_ROUTE_LM;
    memory_bus_window_info_t w;
    if (xbus_match(&s->memory, addr, &w)) return BUS_ROUTE_XBUS;
    if (is_internal_io_addr(addr)) return BUS_ROUTE_INTERNAL_IO;
    if (is_dpram_addr(addr)) return BUS_ROUTE_INTERNAL_RAM;
    return BUS_ROUTE_EXTERNAL;
}

static bus_transaction_t observed_txn(bus_access_kind_t kind, uint32_t addr, int size,
                                      uint32_t value, bus_route_kind_t route,
                                      const char *device, const char *subtype) {
    bus_transaction_t txn;
    memset(&txn, 0, sizeof txn);
    txn.kind = kind;
    txn.addr = addr & 0xFFFFFF;
    txn.size = (uint8_t)size;
    txn.value = value;
    txn.route = route;
    txn.device = device;
    txn.subtype = subtype;
    return txn;
}

static void observe_access(soc_t *s, const bus_transaction_t *txn,
                           const char *trace_kind, const char *detail,
                           const cemu_event_fields_t *info) {
    if (!cemu_event_active(&s->instrumentation, CEMU_EVENT_BUS)) return;
    cemu_native_trace_event_t trace;
    const cemu_native_trace_event_t *trace_ptr = NULL;
    if (txn->kind != BUS_ACCESS_FETCH &&
        cemu_event_native_trace_active(&s->instrumentation, trace_kind)) {
        memset(&trace, 0, sizeof trace);
        trace.kind = trace_kind;
        trace.icount = s->cpu ? s->cpu->icount : 0;
        trace.pc = s->cpu ? cpu_pc(s->cpu) : 0;
        trace.has_addr = 1; trace.addr = txn->addr & 0xFFFFFF;
        trace.has_size = 1; trace.size = txn->size;
        trace.has_value = 1; trace.value = txn->value;
        trace.detail = detail ? detail : "";
        if (info) trace.info = *info;
        trace_ptr = &trace;
    }
    cemu_event_t event = { .type = CEMU_EVENT_BUS };
    event.as.bus.transaction = txn;
    event.as.bus.trace = trace_ptr;
    event.as.bus.tick = s->ticks;
    event.as.bus.icount = s->cpu ? s->cpu->icount : 0;
    event.as.bus.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    cemu_event_emit(&s->instrumentation, &event);
}

static void observe_flash_access(soc_t *s, const bus_transaction_t *txn,
                                 const char *trace_kind, const char *detail,
                                 const cemu_event_fields_t *info,
                                 const memory_flash_target_t *target,
                                 int array_data) {
    if (!cemu_event_active(&s->instrumentation, CEMU_EVENT_BUS)) return;
    cemu_native_trace_event_t trace;
    const cemu_native_trace_event_t *trace_ptr = NULL;
    if (txn->kind != BUS_ACCESS_FETCH &&
        cemu_event_native_trace_active(&s->instrumentation, trace_kind)) {
        memset(&trace, 0, sizeof trace);
        trace.kind = trace_kind;
        trace.icount = s->cpu ? s->cpu->icount : 0;
        trace.pc = s->cpu ? cpu_pc(s->cpu) : 0;
        trace.has_addr = 1; trace.addr = txn->addr & 0xFFFFFF;
        trace.has_size = 1; trace.size = txn->size;
        trace.has_value = 1; trace.value = txn->value;
        trace.detail = detail ? detail : "";
        if (info) trace.info = *info;
        trace_ptr = &trace;
    }
    peripheral_t *periph =
        s->memory.flash_endpoints[target->chip_index];
    flash_state_t *state = flash_st(&s->memory, target->chip_index);
    cemu_event_t event = { .type = CEMU_EVENT_BUS };
    event.as.bus.transaction = txn;
    event.as.bus.trace = trace_ptr;
    event.as.bus.tick = s->ticks;
    event.as.bus.icount = s->cpu ? s->cpu->icount : 0;
    event.as.bus.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    event.as.bus.has_flash_target = 1;
    event.as.bus.flash_array_data = array_data != 0;
    event.as.bus.chip_index = target->chip_index;
    event.as.bus.chip_name = periph->id;
    event.as.bus.model = cemu_flash_model_str(state);
    event.as.bus.chip_offset = target->chip_offset;
    cemu_event_emit(&s->instrumentation, &event);
}
#else
#define observe_access(s, txn, trace_kind, detail, info) ((void)0)
#define observe_flash_access(s, txn, trace_kind, detail, info, target, array) ((void)0)
#endif

static void bus_window_detail(const memory_bus_window_info_t *w, char *detail, int cap) {
    if (w->kind == MEMORY_BUS_WINDOW_XBUS) snprintf(detail, cap, "XADRS%d/XBCON%d", w->index, w->index);
    else if (w->kind == MEMORY_BUS_WINDOW_ADDRSEL) snprintf(detail, cap, "ADDRSEL%d/BUSCON%d", w->index, w->index);
    else snprintf(detail, cap, "BUSCON0");
}

/* Emit the legacy xbus_window event for compatibility with existing traces and
 * tools. The broader bus_window event below carries the full EBI decode. */
static void trace_xbus_window(soc_t *s, int index) {
    memory_bus_window_info_t w = {0};
    if (!cemu_memory_controller_bus_window_decode(
            &s->memory, MEMORY_BUS_WINDOW_XBUS, index, &w))
        return;
    if (!w.configured) return;
    char detail[24];
    bus_window_detail(&w, detail, sizeof detail);
    cemu_event_fields_t info; info.n = 0;
    cemu_event_field_bool(&info, "active", w.active);
    cemu_event_field_bool(&info, "bus_active", w.bus_active);
    cemu_event_field_bool(&info, "configured", w.configured);
    if (w.end >= 0) cemu_event_field_i64(&info, "end", w.end); else cemu_event_field_null(&info, "end");
    cemu_event_field_i64(&info, "index", w.index);
    cemu_event_field_bool(&info, "reserved", w.reserved);
    if (w.size >= 0) cemu_event_field_i64(&info, "size", w.size); else cemu_event_field_null(&info, "size");
    if (w.start >= 0) cemu_event_field_i64(&info, "start", w.start); else cemu_event_field_null(&info, "start");
    cemu_event_field_i64(&info, "xadrs", w.selector);
    cemu_event_field_i64(&info, "xbcon", w.control);
    cemu_event_field_bool(&info, "xpen", w.xpen);
    cemu_event_field_bool(&info, "xper_enabled", w.xper_enabled);
    int has_addr = w.start >= 0, has_size = w.size >= 0;
    cemu_soc_emit_native_trace(s, "xbus_window", has_addr, has_addr ? (uint32_t)w.start : 0,
         has_size, has_size ? (int)w.size : 0, 1, w.control, detail, &info);
}

static void trace_bus_window(soc_t *s, memory_bus_window_kind_t kind, int index) {
    memory_bus_window_info_t w;
    if (!cemu_memory_controller_bus_window_decode(&s->memory, kind, index, &w) || !w.configured) return;
    char detail[32];
    bus_window_detail(&w, detail, sizeof detail);
    cemu_event_fields_t info; info.n = 0;
    cemu_event_field_bool(&info, "active", w.active);
    cemu_event_field_bool(&info, "bus_active", w.bus_active);
    cemu_event_field_i64(&info, "btyp", w.btyp);
    cemu_event_field_bool(&info, "configured", w.configured);
    cemu_event_field_i64(&info, "control", w.control);
    cemu_event_field_i64(&info, "control_addr", w.control_addr);
    cemu_event_field_i64(&info, "csren", w.csren);
    cemu_event_field_i64(&info, "cswen", w.cswen);
    if (w.end >= 0) cemu_event_field_i64(&info, "end", w.end); else cemu_event_field_null(&info, "end");
    cemu_event_field_i64(&info, "ewen", w.ewen);
    cemu_event_field_i64(&info, "index", w.index);
    cemu_event_field_string(&info, "kind", cemu_memory_bus_window_kind_name(w.kind));
    cemu_event_field_i64(&info, "mctc", w.mctc);
    cemu_event_field_i64(&info, "mttc", w.mttc);
    cemu_event_field_i64(&info, "rdyen", w.rdyen);
    cemu_event_field_bool(&info, "reserved", w.reserved);
    cemu_event_field_i64(&info, "rgsad", w.rgsad);
    cemu_event_field_i64(&info, "rgsz", w.rgsz);
    cemu_event_field_i64(&info, "rwdc", w.rwdc);
    cemu_event_field_i64(&info, "selector", w.selector);
    cemu_event_field_i64(&info, "selector_addr", w.selector_addr);
    if (w.size >= 0) cemu_event_field_i64(&info, "size", w.size); else cemu_event_field_null(&info, "size");
    if (w.start >= 0) cemu_event_field_i64(&info, "start", w.start); else cemu_event_field_null(&info, "start");
    if (w.kind == MEMORY_BUS_WINDOW_XBUS) {
        cemu_event_field_bool(&info, "xpen", w.xpen);
        cemu_event_field_bool(&info, "xper_enabled", w.xper_enabled);
    } else {
        cemu_event_field_null(&info, "xpen");
        cemu_event_field_null(&info, "xper_enabled");
    }
    int has_addr = w.start >= 0, has_size = w.size >= 0;
    cemu_soc_emit_native_trace(s, "bus_window", has_addr, has_addr ? (uint32_t)w.start : 0,
         has_size, has_size ? (int)w.size : 0, 1, w.control, detail, &info);
}

/* On a write to bus-control words, re-trace affected windows. */
static void trace_bus_change(soc_t *s, uint32_t wa) {
    if (wa >= XADRS1_ADDR && wa <= XADRS1_ADDR + 10u && !((wa - XADRS1_ADDR) & 1)) {
        int index = (int)((wa - XADRS1_ADDR) / 2) + 1;
        trace_xbus_window(s, index);
        trace_bus_window(s, MEMORY_BUS_WINDOW_XBUS, index);
        return;
    }
    if (wa >= XBCON1_ADDR && wa <= XBCON1_ADDR + 10u && !((wa - XBCON1_ADDR) & 1)) {
        int index = (int)((wa - XBCON1_ADDR) / 2) + 1;
        trace_xbus_window(s, index);
        trace_bus_window(s, MEMORY_BUS_WINDOW_XBUS, index);
        return;
    }
    if (wa >= ADDRSEL1_ADDR && wa <= ADDRSEL1_ADDR + 6u && !((wa - ADDRSEL1_ADDR) & 1)) {
        trace_bus_window(s, MEMORY_BUS_WINDOW_ADDRSEL, (int)((wa - ADDRSEL1_ADDR) / 2) + 1);
        return;
    }
    if (wa >= BUSCON1_ADDR && wa <= BUSCON1_ADDR + 6u && !((wa - BUSCON1_ADDR) & 1)) {
        trace_bus_window(s, MEMORY_BUS_WINDOW_ADDRSEL, (int)((wa - BUSCON1_ADDR) / 2) + 1);
        return;
    }
    if (wa == BUSCON0_ADDR) {
        trace_bus_window(s, MEMORY_BUS_WINDOW_BUSCON0, 0);
        return;
    }
    if (wa == SYSCON_ADDR || wa == XPERCON_ADDR) {
        for (int i = 1; i <= 6; i++) {
            memory_bus_window_info_t w;
            cemu_memory_controller_bus_window_decode(&s->memory, MEMORY_BUS_WINDOW_XBUS, i, &w);
            if (w.configured) {
                trace_xbus_window(s, i);
                trace_bus_window(s, MEMORY_BUS_WINDOW_XBUS, i);
            }
        }
    }
}

/* ---- SFR read/write hooks (route to the claiming peripheral) ---------- */
static inline const char *instrument_sfr_name(soc_t *s, uint32_t addr) {
#if CEMU_INSTRUMENTED
    return cemu_soc_sfr_name(s, addr);
#else
    (void)s; (void)addr;
    return NULL;
#endif
}
static uint16_t sfr_word_value(soc_t *s, uint32_t word_addr) {
    uint16_t stored = memory_controller_sfr_get(&s->memory, word_addr);
    peripheral_t *p = s->memory.sfr_hook[memory_controller_sfr_index(word_addr)];
    if (p && p->read_sfr_word) return p->read_sfr_word(p, s, word_addr, stored) & 0xFFFF;
    return stored;
}
static void on_sfr_poll(soc_t *s, uint32_t word_addr) {
    peripheral_t *p = s->memory.sfr_hook[memory_controller_sfr_index(word_addr)];
    if (p && p->on_sfr_poll) p->on_sfr_poll(p, s, word_addr);
}

static uint8_t read_sfr8(soc_t *s, uint32_t addr, bus_access_kind_t access_kind) {
    uint32_t wa = sfr_word_addr(addr);
    on_sfr_poll(s, wa);
    if (cemu_event_statistics_enabled(&s->instrumentation)) s->memory.sfr_read_counts[memory_controller_sfr_index(wa)]++;
    uint16_t word = sfr_word_value(s, wa);
    uint8_t value = (addr & 1) ? (word >> 8) & 0xFF : word & 0xFF;
#if CEMU_INSTRUMENTED
    bus_transaction_t txn = observed_txn(access_kind, addr, 1, value, BUS_ROUTE_SFR, NULL, NULL);
    observe_access(s, &txn, "sfr_read", instrument_sfr_name(s, wa), NULL);
#else
    (void)access_kind;
#endif
    return value;
}
static uint16_t read_sfr16(soc_t *s, uint32_t addr, bus_access_kind_t access_kind) {
    uint32_t wa = sfr_word_addr(addr);
    on_sfr_poll(s, wa);
    if (cemu_event_statistics_enabled(&s->instrumentation)) s->memory.sfr_read_counts[memory_controller_sfr_index(wa)]++;
    uint16_t value = sfr_word_value(s, wa);
#if CEMU_INSTRUMENTED
    bus_transaction_t txn = observed_txn(access_kind, wa, 2, value, BUS_ROUTE_SFR, NULL, NULL);
    observe_access(s, &txn, "sfr_read", instrument_sfr_name(s, wa), NULL);
#else
    (void)access_kind;
#endif
    return value;
}
/* size/event_addr let a byte write emit at the byte address like Python. */
static void write_sfr16_ev(soc_t *s, uint32_t wa, uint16_t val, int size, uint32_t event_addr) {
    if (cemu_event_statistics_enabled(&s->instrumentation)) s->memory.sfr_write_counts[memory_controller_sfr_index(wa)]++;
    uint16_t stored = val;
    if (wa == SYSCON_ADDR && s->init_locked) stored = memory_controller_sfr_get(&s->memory, wa);
    if (wa == 0xFF1C) stored = 0x0000;        /* ZEROS read-only constant */
    else if (wa == 0xFF1E) stored = 0xFFFF;   /* ONES read-only constant */
    cemu_memory_controller_sfr_put(&s->memory, wa, stored);
#if CEMU_INSTRUMENTED
    bus_transaction_t txn = observed_txn(BUS_ACCESS_WRITE, event_addr, size, stored, BUS_ROUTE_SFR, NULL, NULL);
    observe_access(s, &txn, "sfr_write", instrument_sfr_name(s, wa), NULL);
#else
    (void)size;
    (void)event_addr;
#endif
    if (cemu_event_native_trace_active(&s->instrumentation, "xbus_window") ||
        cemu_event_native_trace_active(&s->instrumentation, "bus_window"))
        trace_bus_change(s, wa);
    peripheral_t *p = s->memory.sfr_hook[memory_controller_sfr_index(wa)];
    if (p && p->on_sfr_write) p->on_sfr_write(p, s, wa, stored);
}
static void write_sfr16_word(soc_t *s, uint32_t wa, uint16_t val) {
    write_sfr16_ev(s, wa, val, 2, wa);
}
static void write_sfr8(soc_t *s, uint32_t addr, uint8_t val) {
    uint32_t wa = sfr_word_addr(addr);
    uint16_t old = memory_controller_sfr_get(&s->memory, wa);
    uint16_t nw = (addr & 1) ? ((old & 0x00FF) | ((uint16_t)val << 8))
                             : ((old & 0xFF00) | val);
    write_sfr16_ev(s, wa, nw, 1, addr);
}

/* ======================================================================= */
/* Observability counters (mirror pmb7850's always-on Counters).            */
/* All gated on cemu_event_statistics_enabled(&s->instrumentation): a bare benchmark leaves the arrays NULL and the  */
/* branches fall straight through, so the hot path pays one predictable test.*/
/* ======================================================================= */
static inline int dpram_index(uint32_t word_addr) {
    return (int)((word_addr - DPRAM_START) >> 1);
}
static inline int internal_io_index(uint32_t word_addr) {
    return (int)((word_addr - INTERNAL_IO_START) >> 1);
}
static void note_internal_io_read(soc_t *s, uint32_t addr) {
    s->memory.internal_io_read_counts[internal_io_index(addr & 0xFFFFFE)]++;
}
static void note_internal_io_write(soc_t *s, uint32_t addr) {
    s->memory.internal_io_write_counts[internal_io_index(addr & 0xFFFFFE)]++;
}
static void note_dpram_read(soc_t *s, uint32_t addr) {
    uint32_t wa = addr & 0xFFFFFE;
    s->memory.dpram_read_counts[dpram_index(wa)]++;
    s->memory.dpram_recent[s->memory.dpram_recent_head] = wa;
    s->memory.dpram_recent_head = (s->memory.dpram_recent_head + 1) & 15;
    if (s->memory.dpram_recent_len < 16) s->memory.dpram_recent_len++;
}
static void note_dpram_write(soc_t *s, uint32_t addr) {
    s->memory.dpram_write_counts[dpram_index(addr & 0xFFFFFE)]++;
}
static void note_internal_read(soc_t *s, uint32_t addr) {
    if (is_internal_io_addr(addr)) note_internal_io_read(s, addr);
    else if (is_dpram_addr(addr)) note_dpram_read(s, addr);
}
static void note_internal_write(soc_t *s, uint32_t addr) {
    if (is_internal_io_addr(addr)) note_internal_io_write(s, addr);
    else if (is_dpram_addr(addr)) note_dpram_write(s, addr);
}
/* Record a non-SFR memory write's progress signals (mirror the mem_write_seq/
 * blocks/segs bumps shared by write8/write16). */
static void note_mem_write(soc_t *s, uint32_t addr) {
    s->memory.mem_write_seq++;
    uint32_t blk = (addr >> 8) & 0xFFFF;
    uint8_t mask = (uint8_t)(1u << (blk & 7));
    if (!(s->memory.mem_write_blocks[blk >> 3] & mask)) {
        s->memory.mem_write_blocks[blk >> 3] |= mask;
        s->memory.mem_write_footprint++;
    }
    s->memory.mem_write_segs[(addr >> 16) & 0xFF]++;
}

static memory_resolution_t resolve_load8(soc_t *s, uint32_t addr) {
    memory_resolution_t r;
    memset(&r, 0, sizeof r);
    addr = norm(addr);
    if (is_sfr_addr(addr)) {
        r.route = MEMORY_ROUTE_SFR;
#if CEMU_INSTRUMENTED
        r.route_kind = BUS_ROUTE_SFR;
#endif
        return r;
    }
    r.periph = byte_range_peripheral(&s->memory, addr);
    r.ram_present = memory_controller_ram_present(&s->memory, addr);
    if (r.periph || is_internal_memory(addr)) {
        r.route = MEMORY_ROUTE_RAM;
#if CEMU_INSTRUMENTED
        r.route_kind = resolved_route_kind(s, addr);
#endif
        return r;
    }
    if (cemu_memory_controller_lm_translate(&s->memory, addr, &r.lm_off)) {
        r.route = MEMORY_ROUTE_LM;
#if CEMU_INSTRUMENTED
        r.route_kind = BUS_ROUTE_LM;
#endif
        return r;
    }
    r.periph = external_ram_resolve(&s->memory, addr, &r.external_ram_off);
    if (r.periph) {
        r.route = MEMORY_ROUTE_EXTERNAL_RAM;
#if CEMU_INSTRUMENTED
        r.route_kind = BUS_ROUTE_EXTERNAL;
#endif
        return r;
    }
    if (r.ram_present) {
        r.route = MEMORY_ROUTE_RAM;
#if CEMU_INSTRUMENTED
        r.route_kind = resolved_route_kind(s, addr);
#endif
        return r;
    }
    if (cemu_memory_controller_flash_translate(&s->memory, addr, &r.flash)) {
        r.route = MEMORY_ROUTE_FLASH;
#if CEMU_INSTRUMENTED
        r.route_kind = BUS_ROUTE_EXTERNAL;
#endif
        return r;
    }
    r.route = MEMORY_ROUTE_UNMAPPED;
#if CEMU_INSTRUMENTED
    r.route_kind = BUS_ROUTE_UNMAPPED;
#endif
    return r;
}

static memory_resolution_t resolve_store8(soc_t *s, uint32_t addr) {
    memory_resolution_t r;
    memset(&r, 0, sizeof r);
    addr = norm(addr);
    if (is_sfr_addr(addr)) {
        r.route = MEMORY_ROUTE_SFR;
#if CEMU_INSTRUMENTED
        r.route_kind = BUS_ROUTE_SFR;
#endif
        return r;
    }
    if (is_internal_memory(addr)) {
        r.route = MEMORY_ROUTE_RAM;
        r.periph = byte_range_peripheral(&s->memory, addr);
#if CEMU_INSTRUMENTED
        r.route_kind = resolved_route_kind(s, addr);
#endif
        return r;
    }
    if (cemu_memory_controller_lm_translate(&s->memory, addr, &r.lm_off)) {
        r.route = MEMORY_ROUTE_LM;
#if CEMU_INSTRUMENTED
        r.route_kind = BUS_ROUTE_LM;
#endif
        return r;
    }
    r.periph = external_ram_resolve(&s->memory, addr, &r.external_ram_off);
    if (r.periph) {
        r.route = MEMORY_ROUTE_EXTERNAL_RAM;
#if CEMU_INSTRUMENTED
        r.route_kind = BUS_ROUTE_EXTERNAL;
#endif
        return r;
    }
    if (cemu_memory_controller_flash_translate(&s->memory, addr, &r.flash)) {
        r.route = MEMORY_ROUTE_FLASH;
#if CEMU_INSTRUMENTED
        r.route_kind = BUS_ROUTE_EXTERNAL;
#endif
        return r;
    }
    r.route = MEMORY_ROUTE_RAM;
    r.periph = NULL;
#if CEMU_INSTRUMENTED
    r.route_kind = resolved_route_kind(s, addr);
#endif
    return r;
}

#if CEMU_INSTRUMENTED
static void observe_mem_access(soc_t *s, const bus_transaction_t *txn, const char *detail) {
    const int is_write = txn->kind == BUS_ACCESS_WRITE;
    const char *trace_kind = is_write ? "mem_write" : "mem_read";
    cemu_event_fields_t info;
    fill_xbus_access_info(s, txn->addr, &info);
    observe_access(s, txn, trace_kind, detail, &info);

    if (cemu_event_native_trace_active(&s->instrumentation, "xbus_access")) {
        cemu_event_fields_t xinfo;
        if (fill_xbus_access_event_info(s, txn->addr, is_write ? "write" : "read", &xinfo)) {
            cemu_soc_emit_native_trace(s, "xbus_access", 1, txn->addr, 1, txn->size, 1, txn->value,
                 detail ? detail : "", &xinfo);
        }
    }
}

static void observe_external_ram_access(soc_t *s,
                                        const bus_transaction_t *txn,
                                        peripheral_t *ram,
                                        uint32_t physical_offset) {
    cemu_event_fields_t info;
    fill_xbus_access_info(s, txn->addr, &info);
    cemu_event_field_string(&info, "device", ram->id);
    cemu_event_field_string(&info, "model", ram->model);
    cemu_event_field_i64(&info, "physical_offset", physical_offset);
    observe_access(s, txn,
                   txn->kind == BUS_ACCESS_WRITE ? "mem_write" : "mem_read",
                   ram->id, &info);
}

static void append_flash_info(cemu_event_fields_t *info, soc_t *s,
                              const memory_flash_target_t *target,
                              const char *subtype, int include_mode) {
    peripheral_t *periph = s->memory.flash_endpoints[target->chip_index];
    flash_state_t *state = flash_st(&s->memory, target->chip_index);
    if (s->memory.n_flash_chips > 1) {
        cemu_event_field_i64(info, "chip_index", target->chip_index);
        cemu_event_field_string(info, "chip_name", periph->id);
        cemu_event_field_i64(info, "chip_offset", target->chip_offset);
    }
    cemu_event_field_string(info, "device", periph->id);
    if (include_mode) cemu_event_field_string(info, "mode", cemu_flash_mode_str(state));
    cemu_event_field_string(info, "model", cemu_flash_model_str(state));
    if (subtype) cemu_event_field_string(info, "subtype", subtype);
}
static void fill_flash_info(cemu_event_fields_t *info, soc_t *s, uint32_t addr,
                            const memory_flash_target_t *target,
                            const char *subtype, int include_mode) {
    info->n = 0;
    append_bus_route_info(s, addr, info, 0);
    append_flash_info(info, s, target, subtype, include_mode);
}

static void observe_flash_protocol(soc_t *s, const bus_transaction_t *txn,
                                   const memory_flash_target_t *target,
                                   const flash_access_t *access) {
    if (!access || !access->subtype) return;
    const char *kind = NULL;
    if (!strcmp(access->subtype, "status")) kind = "flash_status";
    else if (!strcmp(access->detail, "sector-erase-confirm") ||
             !strcmp(access->detail, "block-erase-confirm"))
        kind = "flash_erase_start";
    else if (!strcmp(access->detail, "erase-suspend"))
        kind = "flash_erase_suspend";
    else if (!strcmp(access->detail, "erase-resume"))
        kind = "flash_erase_resume";
    else if (!strcmp(access->detail, "program-suspend"))
        kind = "flash_program_suspend";
    else if (!strcmp(access->detail, "program-resume"))
        kind = "flash_program_resume";
    else if (!strcmp(access->detail, "word-program-start") ||
             !strcmp(access->detail, "program-data") ||
             (!strcmp(access->detail, "program") &&
              !strcmp(access->subtype, "program")) ||
             (!strcmp(access->detail, "confirm/resume") &&
              access->mutation_count) ||
             !strcmp(access->detail, "write-buffer-confirm"))
        kind = "flash_program_start";
    else if (!strcmp(access->detail, "protection-program-start") ||
             !strcmp(access->detail, "protection-lock"))
        kind = "flash_protection_program";
    else if (!strcmp(access->detail, "block-lock") ||
             !strcmp(access->detail, "block-unlock") ||
             !strcmp(access->detail, "block-lock-down"))
        kind = "flash_lock_change";
    else if (!strcmp(access->detail, "command-sequence-error") ||
             !strcmp(access->detail, "enhanced-factory-program-unsupported") ||
             !strcmp(access->detail, "rejected-command") ||
             !strcmp(access->detail, "program-locked") ||
             !strcmp(access->detail, "erase-locked") ||
             !strcmp(access->detail, "protection-locked") ||
             !strcmp(access->detail, "program-in-suspended-erase-block"))
        kind = "flash_rejected_command";
    else if (!strcmp(access->subtype, "write-absorbed"))
        kind = "flash_write_absorbed";
    else if (!strcmp(access->subtype, "cmd") ||
             !strcmp(access->subtype, "unlock"))
        kind = "flash_command";
    if (!kind) return;

    flash_state_t *state = flash_st(&s->memory, target->chip_index);
    peripheral_t *periph = s->memory.flash_endpoints[target->chip_index];
    cemu_event_fields_t info = {0};
    flash_operation_info_t operation;
    int has_operation = cemu_flash_operation_info(state, &operation);
    int suspend_transition = !strcmp(kind, "flash_erase_suspend") ||
                             !strcmp(kind, "flash_erase_resume") ||
                             !strcmp(kind, "flash_program_suspend") ||
                             !strcmp(kind, "flash_program_resume");
    if (s->memory.n_flash_chips > 1) {
        cemu_event_field_i64(&info, "chip_index", target->chip_index);
        cemu_event_field_string(&info, "chip_name", periph->id);
    }
    cemu_event_field_i64(&info, "chip_offset", target->chip_offset);
    cemu_event_field_string(&info, "device", periph->id);
    if (suspend_transition && has_operation) {
        cemu_event_field_bool(&info, "operation_suspended", operation.suspended);
        if (!strcmp(operation.kind, "erase"))
        cemu_event_field_bool(&info, "erase_suspended", operation.suspended);
    }
    cemu_event_field_string(&info, "mode", cemu_flash_mode_str(state));
    cemu_event_field_string(&info, "model", cemu_flash_model_str(state));
    if (has_operation) {
        cemu_event_field_string(&info, "operation", operation.kind);
        cemu_event_field_i64(&info, "operation_offset", operation.offset);
        if (operation.partition >= 0)
            cemu_event_field_i64(&info, "partition", operation.partition);
        cemu_event_field_i64(&info, "remaining_ticks", operation.remaining_ticks);
        if (state->kind == FLASH_MODEL_AM29LV)
            cemu_event_field_i64(&info, "sector_offset", operation.offset);
    }
    cemu_event_field_string(&info, "subtype", access->subtype);
    cemu_soc_emit_native_trace(s, kind, 1, txn->addr, 1, txn->size, 1, txn->value,
         access->detail, &info);
}

#else
#define observe_mem_access(s, txn, detail) ((void)0)
#define observe_external_ram_access(s, txn, ram, off) ((void)0)
#endif

static uint8_t memory_load8(soc_t *s, bus_transaction_t *txn) {
    uint32_t addr = txn->addr;
    memory_resolution_t r = resolve_load8(s, addr);
    if (cemu_event_statistics_enabled(&s->instrumentation) && !s->memory.suppress_dpram_count && r.route != MEMORY_ROUTE_SFR)
        note_ebi_access(s, addr);
    uint8_t value = 0;
#if CEMU_INSTRUMENTED
    txn->route = r.route_kind;
#endif
#if CEMU_INSTRUMENTED
    txn->device = NULL;
#endif
#if CEMU_INSTRUMENTED
    txn->subtype = NULL;
#endif
    switch (r.route) {
        case MEMORY_ROUTE_SFR:
            return read_sfr8(s, addr, txn->kind);
        case MEMORY_ROUTE_RAM:
            if (r.periph && r.periph->read8) {
                int v = r.periph->read8(r.periph, s, addr);
                if (v >= 0) {
                    value = (uint8_t)v;
                    txn->value = value;
#if CEMU_INSTRUMENTED
                    txn->route = r.route_kind;
#endif
#if CEMU_INSTRUMENTED
                    txn->device = r.periph->id;
#endif
                    if (cemu_event_statistics_enabled(&s->instrumentation) && !s->memory.suppress_dpram_count && is_internal_memory(addr)) {
                        note_internal_read(s, addr);
                    }
                    observe_mem_access(s, txn, r.periph->id ? r.periph->id : "ram");
                    return value;
                }
            }
            if (r.ram_present) {
                value = memory_controller_ram_get(&s->memory, addr);
                txn->value = value;
                if (cemu_event_statistics_enabled(&s->instrumentation) && !s->memory.suppress_dpram_count && is_internal_memory(addr)) {
                    note_internal_read(s, addr);
                }
                observe_mem_access(s, txn, "ram");
                return value;
            }
            if (is_internal_memory(addr)) {
                txn->value = 0;
                if (cemu_event_statistics_enabled(&s->instrumentation) && !s->memory.suppress_dpram_count) {
                    note_internal_read(s, addr);
                }
                observe_mem_access(s, txn, "ram-default");
                return 0;
            }
            break;
        case MEMORY_ROUTE_LM:
            value = s->memory.lm[r.lm_off];
            txn->value = value;
            observe_access(s, txn, "mem_read", "lm", NULL);
            return value;
        case MEMORY_ROUTE_EXTERNAL_RAM: {
            external_ram_state_t *st = (external_ram_state_t *)r.periph->state;
            value = st->bytes[r.external_ram_off];
            txn->value = value;
#if CEMU_INSTRUMENTED
            txn->device = r.periph->id;
            txn->subtype = r.periph->model;
#endif
            observe_external_ram_access(s, txn, r.periph,
                                        r.external_ram_off);
            return value;
        }
        case MEMORY_ROUTE_FLASH: {
            const uint8_t *chip_data;
            size_t chip_len;
            flash_chip_data(&s->memory, r.flash.chip_index, &chip_data, &chip_len);
            flash_state_t *state = flash_st(&s->memory, r.flash.chip_index);
            peripheral_t *periph = s->memory.flash_endpoints[r.flash.chip_index];
#if CEMU_INSTRUMENTED
            flash_access_t access;
            value = cemu_flash_read8(chip_data, chip_len, state,
                                r.flash.chip_offset,
                                r.flash.command_visible, &access);
            txn->value = value;
            txn->device = periph->id;
            txn->subtype = access.subtype;
            if (access.subtype || s->memory.n_flash_chips > 1) {
                cemu_event_fields_t info;
                fill_flash_info(&info, s, addr, &r.flash, access.subtype,
                                /*include_mode=*/0);
                observe_flash_access(s, txn, "mem_read", access.detail,
                                     &info, &r.flash,
                                     access.subtype == NULL);
            } else {
                observe_flash_access(s, txn, "mem_read", access.detail,
                                     NULL, &r.flash,
                                     access.subtype == NULL);
            }
            observe_flash_protocol(s, txn, &r.flash, &access);
#else
            (void)periph;
            value = cemu_flash_read8(chip_data, chip_len, state,
                                r.flash.chip_offset,
                                r.flash.command_visible, NULL);
            txn->value = value;
#endif
            return value;
        }
        case MEMORY_ROUTE_UNMAPPED:
            txn->value = 0xFF;
            observe_access(s, txn, "unmapped_read", "", NULL);
            return 0xFF;
    }
    txn->value = 0xFF;
    observe_access(s, txn, "mem_read", "", NULL);
    return 0xFF;
}

static uint16_t memory_load16(soc_t *s, bus_transaction_t *txn) {
    uint32_t addr = norm(txn->addr);
    txn->addr = addr;
#if CEMU_INSTRUMENTED
    txn->device = NULL;
#endif
#if CEMU_INSTRUMENTED
    txn->subtype = NULL;
#endif
    if (txn->kind != BUS_ACCESS_FETCH && is_sfr_addr(addr)) {
#if CEMU_INSTRUMENTED
        txn->route = BUS_ROUTE_SFR;
#endif
        txn->value = read_sfr16(s, addr, txn->kind);
        return (uint16_t)txn->value;
    }
    if (cemu_event_statistics_enabled(&s->instrumentation)) {
        if (is_internal_memory(addr)) note_internal_read(s, addr);
        note_ebi_access(s, addr);
    }
    s->memory.suppress_dpram_count = 1;
    bus_transaction_t lo = *txn;
    lo.size = 1;
    uint8_t lob = memory_load8(s, &lo);
    bus_transaction_t hi = *txn;
    hi.addr = norm(addr + 1);
    hi.size = 1;
    uint8_t hib = memory_load8(s, &hi);
    s->memory.suppress_dpram_count = 0;
#if CEMU_INSTRUMENTED
    txn->route = lo.route;
#endif
#if CEMU_INSTRUMENTED
    txn->device = lo.device;
#endif
#if CEMU_INSTRUMENTED
    txn->subtype = lo.subtype;
#endif
    txn->value = lob | ((uint16_t)hib << 8);
    return (uint16_t)txn->value;
}

static void memory_store8(soc_t *s, bus_transaction_t *txn) {
    uint32_t addr = norm(txn->addr);
    uint8_t value = (uint8_t)txn->value;
    txn->addr = addr;
#if CEMU_INSTRUMENTED
    txn->device = NULL;
#endif
#if CEMU_INSTRUMENTED
    txn->subtype = NULL;
#endif
    if (is_sfr_addr(addr)) {
#if CEMU_INSTRUMENTED
        txn->route = BUS_ROUTE_SFR;
#endif
        write_sfr8(s, addr, value);
        return;
    }
    if (cemu_event_statistics_enabled(&s->instrumentation)) {
        note_mem_write(s, addr);
        note_ebi_access(s, addr);
    }
    memory_resolution_t r = resolve_store8(s, addr);
#if CEMU_INSTRUMENTED
    txn->route = r.route_kind;
#endif
    switch (r.route) {
        case MEMORY_ROUTE_SFR:
            write_sfr8(s, addr, value);
            return;
        case MEMORY_ROUTE_LM:
            s->memory.lm[r.lm_off] = value;
            observe_access(s, txn, "mem_write", "lm", NULL);
            return;
        case MEMORY_ROUTE_EXTERNAL_RAM: {
            external_ram_state_t *st = (external_ram_state_t *)r.periph->state;
            if (st->bytes[r.external_ram_off] != value)
                st->mutation_seq++;
            st->bytes[r.external_ram_off] = value;
#if CEMU_INSTRUMENTED
            txn->device = r.periph->id;
            txn->subtype = r.periph->model;
#endif
            observe_external_ram_access(s, txn, r.periph,
                                        r.external_ram_off);
            return;
        }
        case MEMORY_ROUTE_FLASH: {
            const uint8_t *chip_data;
            size_t chip_len;
            flash_chip_data(&s->memory, r.flash.chip_index, &chip_data, &chip_len);
            flash_state_t *state = flash_st(&s->memory, r.flash.chip_index);
#if CEMU_INSTRUMENTED
            flash_access_t access;
            cemu_flash_write8(chip_data, chip_len, state, r.flash.chip_offset,
                         value, r.flash.command_visible, &access);
            txn->device = s->memory.flash_endpoints[r.flash.chip_index]->id;
            txn->subtype = access.subtype;
            cemu_event_fields_t info;
            fill_flash_info(&info, s, addr, &r.flash, txn->subtype,
                            access.include_mode);
            observe_flash_access(s, txn, "mem_write", access.detail, &info,
                                 &r.flash, 0);
            observe_flash_protocol(s, txn, &r.flash, &access);
            for (size_t i = 0; i < access.mutation_count; i++) {
                const flash_mutation_t *mutation = &access.mutations[i];
                cemu_flash_emit_mutation(
                    s, s->memory.flash_endpoints[r.flash.chip_index],
                    mutation->kind, mutation->offset, mutation->size);
            }
#else
            cemu_flash_write8(chip_data, chip_len, state, r.flash.chip_offset,
                         value, r.flash.command_visible, NULL);
#endif
            return;
        }
        case MEMORY_ROUTE_RAM:
            if (cemu_event_statistics_enabled(&s->instrumentation) && is_internal_memory(addr)) {
                note_internal_write(s, addr);
            }
            memory_controller_ram_set(&s->memory, addr, value);
            if (is_internal_memory(addr) && r.periph && r.periph->write8) {
#if CEMU_INSTRUMENTED
                txn->device = r.periph->id;
#endif
                r.periph->write8(r.periph, s, addr, value);
            }
            observe_mem_access(s, txn, "ram");
            return;
        case MEMORY_ROUTE_UNMAPPED:
            memory_controller_ram_set(&s->memory, addr, value);
            observe_mem_access(s, txn, "ram");
            return;
    }
}

static void memory_store16(soc_t *s, bus_transaction_t *txn) {
    uint32_t addr = norm(txn->addr);
    uint16_t value = (uint16_t)txn->value;
    txn->addr = addr;
#if CEMU_INSTRUMENTED
    txn->device = NULL;
#endif
#if CEMU_INSTRUMENTED
    txn->subtype = NULL;
#endif
    if (is_sfr_addr(addr)) {
#if CEMU_INSTRUMENTED
        txn->route = BUS_ROUTE_SFR;
#endif
        write_sfr16_word(s, sfr_word_addr(addr), value);
        return;
    }
    if (cemu_event_statistics_enabled(&s->instrumentation)) {
        note_mem_write(s, addr);
        note_ebi_access(s, addr);
    }
    memory_resolution_t r = resolve_store8(s, addr);
#if CEMU_INSTRUMENTED
    txn->route = r.route_kind;
#endif
    switch (r.route) {
        case MEMORY_ROUTE_SFR:
            write_sfr16_word(s, sfr_word_addr(addr), value);
            return;
        case MEMORY_ROUTE_LM:
            s->memory.lm[r.lm_off] = value & 0xFF;
            s->memory.lm[r.lm_off + 1u] = (value >> 8) & 0xFF;
            observe_access(s, txn, "mem_write", "lm", NULL);
            return;
        case MEMORY_ROUTE_EXTERNAL_RAM: {
            external_ram_state_t *st = (external_ram_state_t *)r.periph->state;
            uint32_t hi_off = (r.external_ram_off + 1u) % st->chip_size;
            if (st->bytes[r.external_ram_off] != (value & 0xFF))
                st->mutation_seq++;
            if (st->bytes[hi_off] != ((value >> 8) & 0xFF))
                st->mutation_seq++;
            st->bytes[r.external_ram_off] = value & 0xFF;
            st->bytes[hi_off] = (value >> 8) & 0xFF;
#if CEMU_INSTRUMENTED
            txn->device = r.periph->id;
            txn->subtype = r.periph->model;
#endif
            observe_external_ram_access(s, txn, r.periph,
                                        r.external_ram_off);
            return;
        }
        case MEMORY_ROUTE_FLASH: {
            const uint8_t *chip_data;
            size_t chip_len;
            flash_chip_data(&s->memory, r.flash.chip_index, &chip_data, &chip_len);
            flash_state_t *state = flash_st(&s->memory, r.flash.chip_index);
#if CEMU_INSTRUMENTED
            flash_access_t access;
            cemu_flash_write16(chip_data, chip_len, state, r.flash.chip_offset,
                          value, r.flash.command_visible, &access);
            txn->device = s->memory.flash_endpoints[r.flash.chip_index]->id;
            txn->subtype = access.subtype;
            cemu_event_fields_t info;
            fill_flash_info(&info, s, addr, &r.flash, txn->subtype,
                            access.include_mode);
            observe_flash_access(s, txn, "mem_write", access.detail, &info,
                                 &r.flash, 0);
            observe_flash_protocol(s, txn, &r.flash, &access);
            for (size_t i = 0; i < access.mutation_count; i++) {
                const flash_mutation_t *mutation = &access.mutations[i];
                cemu_flash_emit_mutation(
                    s, s->memory.flash_endpoints[r.flash.chip_index],
                    mutation->kind, mutation->offset, mutation->size);
            }
#else
            cemu_flash_write16(chip_data, chip_len, state, r.flash.chip_offset,
                          value, r.flash.command_visible, NULL);
#endif
            return;
        }
        case MEMORY_ROUTE_RAM:
            if (cemu_event_statistics_enabled(&s->instrumentation) && is_internal_memory(addr)) {
                note_internal_write(s, addr);
            }
            memory_controller_ram_set(&s->memory, addr, value & 0xFF);
            memory_controller_ram_set(&s->memory, norm(addr + 1), (value >> 8) & 0xFF);
            if (is_internal_memory(addr) && r.periph && r.periph->write8) {
#if CEMU_INSTRUMENTED
                txn->device = r.periph->id;
#endif
                r.periph->write8(r.periph, s, addr, value & 0xFF);
            }
            observe_mem_access(s, txn, "ram");
            return;
        case MEMORY_ROUTE_UNMAPPED:
            memory_controller_ram_set(&s->memory, addr, value & 0xFF);
            memory_controller_ram_set(&s->memory, norm(addr + 1), (value >> 8) & 0xFF);
            observe_mem_access(s, txn, "ram");
            return;
    }
}

void cemu_memory_controller_access(
    memory_controller_t *mc, bus_transaction_t *txn) {
    soc_t *s = mc->soc;
    txn->addr = norm(txn->addr);
    if (txn->kind == BUS_ACCESS_WRITE) {
        if (txn->size == 2) memory_store16(s, txn);
        else memory_store8(s, txn);
        return;
    }
    if (txn->size == 2) txn->value = memory_load16(s, txn);
    else txn->value = memory_load8(s, txn);
}

/* ---- event-free peeks (mirror pmb7850._peek8 / peek16) ---------------- */
uint8_t cemu_memory_controller_peek8(memory_controller_t *mc, uint32_t addr) {
    soc_t *s = mc->soc;
    addr = norm(addr);
    if (is_sfr_addr(addr)) {
        uint16_t word = sfr_word_value(s, sfr_word_addr(addr));
        return (addr & 1) ? (word >> 8) & 0xFF : word & 0xFF;
    }
    peripheral_t *p = byte_range_peripheral(&s->memory, addr);
    if (p && p->peek8) { int v = p->peek8(p, s, addr); if (v >= 0) return (uint8_t)v; }
    if (is_internal_memory(addr))
        return memory_controller_ram_get(&s->memory, addr);
    uint32_t lm_off = 0;
    if (cemu_memory_controller_lm_translate(&s->memory, addr, &lm_off)) return s->memory.lm[lm_off];
    uint32_t ram_off = 0;
    peripheral_t *ram = external_ram_resolve(&s->memory, addr, &ram_off);
    if (ram)
        return ((external_ram_state_t *)ram->state)->bytes[ram_off];
    if (memory_controller_ram_present(&s->memory, addr)) return memory_controller_ram_get(&s->memory, addr);
    memory_flash_target_t target;
    if (cemu_memory_controller_flash_translate(mc, addr, &target)) {
        const uint8_t *chip_data;
        size_t chip_len;
        flash_chip_data(mc, target.chip_index, &chip_data, &chip_len);
        return cemu_flash_peek8(chip_data, chip_len,
                           flash_st(&s->memory, target.chip_index),
                           target.chip_offset, target.command_visible);
    }
    return 0xFF;
}
uint16_t cemu_memory_controller_peek16(memory_controller_t *mc, uint32_t addr) {
    soc_t *s = mc->soc;
    addr = norm(addr);
    if (is_sfr_addr(addr)) return sfr_word_value(s, sfr_word_addr(addr));
    return cemu_memory_controller_peek8(mc, addr) |
           ((uint16_t)cemu_memory_controller_peek8(mc, addr + 1) << 8);
}

/* ---- event-free pokes (debugger writes; no bus / no hooks / no watch) --- */
void cemu_memory_controller_poke8(
    memory_controller_t *mc, uint32_t addr, uint8_t value) {
    soc_t *s = mc->soc;
    addr = norm(addr);
    if (is_sfr_addr(addr)) {
        uint32_t wa = sfr_word_addr(addr);
        uint16_t old = memory_controller_sfr_get(&s->memory, wa);
        uint16_t nw = (addr & 1) ? ((old & 0x00FF) | ((uint16_t)value << 8))
                                 : ((old & 0xFF00) | value);
        mc->sfr[memory_controller_sfr_index(wa)] = nw;
        cemu_interrupt_subsystem_resync_addr(&s->interrupts, wa);
        return;
    }
    if (is_internal_memory(addr)) {
        memory_controller_ram_set(&s->memory, addr, value);
        return;
    }
    uint32_t lm_off = 0;
    if (cemu_memory_controller_lm_translate(&s->memory, addr, &lm_off)) {
        s->memory.lm[lm_off] = value;
        return;
    }
    uint32_t ram_off = 0;
    peripheral_t *ram = external_ram_resolve(&s->memory, addr, &ram_off);
    if (ram) {
        external_ram_state_t *st = (external_ram_state_t *)ram->state;
        if (st->bytes[ram_off] != value) st->mutation_seq++;
        st->bytes[ram_off] = value;
        return;
    }
    memory_controller_ram_set(&s->memory, addr, value);
}
void cemu_memory_controller_poke16(
    memory_controller_t *mc, uint32_t addr, uint16_t value) {
    soc_t *s = mc->soc;
    addr = norm(addr);
    if (is_sfr_addr(addr)) {
        uint32_t wa = sfr_word_addr(addr);
        mc->sfr[memory_controller_sfr_index(wa)] = value;
        cemu_interrupt_subsystem_resync_addr(&s->interrupts, wa);
        return;
    }
    if (is_internal_memory(addr)) {
        memory_controller_ram_set(&s->memory, addr, value & 0xFF);
        memory_controller_ram_set(&s->memory, norm(addr + 1), (value >> 8) & 0xFF);
        return;
    }
    uint32_t lm_off = 0;
    if (cemu_memory_controller_lm_translate(&s->memory, addr, &lm_off)) {
        s->memory.lm[lm_off] = value & 0xFF;
        s->memory.lm[lm_off + 1u] = (value >> 8) & 0xFF;
        return;
    }
    uint32_t ram_off = 0;
    peripheral_t *ram = external_ram_resolve(&s->memory, addr, &ram_off);
    if (ram) {
        external_ram_state_t *st = (external_ram_state_t *)ram->state;
        uint32_t hi_off = (ram_off + 1u) % st->chip_size;
        if (st->bytes[ram_off] != (value & 0xFF)) st->mutation_seq++;
        if (st->bytes[hi_off] != ((value >> 8) & 0xFF)) st->mutation_seq++;
        st->bytes[ram_off] = value & 0xFF;
        st->bytes[hi_off] = (value >> 8) & 0xFF;
        return;
    }
    memory_controller_ram_set(&s->memory, addr, value & 0xFF);
    memory_controller_ram_set(&s->memory, norm(addr + 1), (value >> 8) & 0xFF);
}

external_ram_state_t *cemu_memory_controller_external_ram_state(
    const memory_controller_t *mc, int index) {
    if (!mc || index < 0 || index >= mc->n_external_ram) return NULL;
    return (external_ram_state_t *)mc->external_ram_endpoints[index]->state;
}

size_t cemu_memory_controller_external_ram_count(
        const memory_controller_t *mc) {
    return mc && mc->n_external_ram > 0 ? (size_t)mc->n_external_ram : 0u;
}

int cemu_memory_controller_external_ram_view(
        const memory_controller_t *mc, size_t index,
        memory_external_ram_view_t *view) {
    if (view) memset(view, 0, sizeof *view);
    if (!mc || !view || index >= (size_t)mc->n_external_ram) return 0;
    const external_ram_state_t *state =
        (const external_ram_state_t *)mc->external_ram_endpoints[index]->state;
    if (!state || !state->bytes || !state->chip_size) return 0;
    *view = (memory_external_ram_view_t){
        .model = state->model,
        .bytes = state->bytes,
        .size = state->chip_size,
    };
    return 1;
}

uint64_t cemu_memory_controller_external_ram_mutation_seq(
    const memory_controller_t *mc) {
    uint64_t total = 0;
    for (int i = 0; mc && i < mc->n_external_ram; i++)
        total += cemu_memory_controller_external_ram_state(mc, i)->mutation_seq;
    return total;
}

void cemu_memory_controller_xbus_decode(
    memory_controller_t *mc, int index, long *start, long *end,
    int *active, int *configured, char *reason, int reason_cap) {
    memory_bus_window_info_t window;
    cemu_memory_controller_bus_window_decode(
        mc, MEMORY_BUS_WINDOW_XBUS, index, &window);
    *start = window.start;
    *end = window.end;
    *active = window.active;
    *configured = window.configured;
    if (reason_cap > 0)
        snprintf(reason, reason_cap, "%s", window.reason);
}
