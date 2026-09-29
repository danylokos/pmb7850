/* C166S interrupt arbitration, caching, wake prediction, and PEC routing. */
#include <string.h>
#include "interrupts.h"
#include "soc.h"

#define XIC_IR (1u << 7)
#define XIC_IE (1u << 6)

static uint32_t word_addr(uint32_t addr) {
    return addr & 0xFFFFFEu;
}

static int source_map_index(uint32_t addr) {
    addr = word_addr(addr);
    if (addr < SFR_BASE || addr >= SFR_BASE + 2u * SFR_WORDS) return -1;
    return (int)((addr - SFR_BASE) >> 1);
}

static void invalidate(interrupt_subsystem_t *ic) {
    ic->generation++;
    ic->cache_valid = 0;
    if (ic->collect_cache_stats) ic->cache_stats.invalidations++;
}

static int port_pending(void *ctx, interrupt_request_t *request) {
    return cemu_interrupt_subsystem_pending(ctx, request);
}

static interrupt_service_t port_begin_service(
    void *ctx, const interrupt_request_t *request) {
    return cemu_interrupt_subsystem_begin_service(ctx, request);
}

static void port_acknowledge(
    void *ctx, const interrupt_request_t *request) {
    cemu_interrupt_subsystem_acknowledge(ctx, request);
}

void cemu_interrupt_subsystem_init(interrupt_subsystem_t *ic, soc_t *soc) {
    memset(ic, 0, sizeof(*ic));
    ic->soc = soc;
    for (int i = 0; i < INTERRUPT_SFR_WORDS; i++)
        ic->source_by_sfr[i] = -1;
    ic->generation = 1;
    ic->port.ctx = ic;
    ic->port.pending = port_pending;
    ic->port.begin_service = port_begin_service;
    ic->port.acknowledge = port_acknowledge;
    cemu_pec_endpoint_init(&ic->pec_endpoint, &ic->pec);
}

int cemu_interrupt_subsystem_register(
    interrupt_subsystem_t *ic, uint32_t addr, int trap, int is_timer,
    peripheral_t *owner) {
    int map_index = source_map_index(addr);
    if (map_index < 0 || ic->n_sources >= INTERRUPT_MAX_SOURCES ||
        ic->source_by_sfr[map_index] >= 0)
        return 0;

    int token = ic->n_sources++;
    interrupt_source_t *source = &ic->sources[token];
    source->addr = word_addr(addr);
    source->trap = trap;
    source->has_trap = trap > 0;
    source->is_timer = is_timer;
    source->owner = owner;
    ic->source_by_sfr[map_index] = (int16_t)token;
    ic->source_values[token] = memory_controller_sfr_get(&ic->soc->memory, source->addr);
    invalidate(ic);
    return 1;
}

int cemu_interrupt_subsystem_source_token(
    const interrupt_subsystem_t *ic, uint32_t addr) {
    int map_index = source_map_index(addr);
    if (map_index < 0) return -1;
    return ic->source_by_sfr[map_index];
}

void cemu_interrupt_subsystem_sfr_changed(
    interrupt_subsystem_t *ic, uint32_t addr, uint16_t old_value,
    uint16_t new_value) {
    if (!ic || old_value == new_value) return;
    addr = word_addr(addr);
    int token = cemu_interrupt_subsystem_source_token(ic, addr);
    if (token >= 0) {
        ic->source_values[token] = new_value;
        invalidate(ic);
    }
    if (ic->debug_valid && addr == ic->debug_addr) {
        ic->debug_value = new_value;
        if (token < 0) invalidate(ic);
    }
}

void cemu_interrupt_subsystem_resync_addr(
    interrupt_subsystem_t *ic, uint32_t addr) {
    addr = word_addr(addr);
    int token = cemu_interrupt_subsystem_source_token(ic, addr);
    if (token < 0 && (!ic->debug_valid || ic->debug_addr != addr)) return;
    uint16_t value = memory_controller_sfr_get(&ic->soc->memory, addr);
    int changed = 0;
    if (token >= 0 && ic->source_values[token] != value) {
        ic->source_values[token] = value;
        changed = 1;
    }
    if (ic->debug_valid && ic->debug_addr == addr &&
        ic->debug_value != value) {
        ic->debug_value = value;
        changed = 1;
    }
    if (changed) invalidate(ic);
}

void cemu_interrupt_subsystem_resync(interrupt_subsystem_t *ic) {
    int changed = 0;
    for (int i = 0; i < ic->n_sources; i++) {
        uint16_t value = memory_controller_sfr_get(&ic->soc->memory, ic->sources[i].addr);
        if (ic->source_values[i] == value) continue;
        ic->source_values[i] = value;
        changed = 1;
    }
    if (ic->debug_valid) {
        uint16_t value = memory_controller_sfr_get(&ic->soc->memory, ic->debug_addr);
        if (ic->debug_value != value) {
            ic->debug_value = value;
            changed = 1;
        }
    }
    if (changed) invalidate(ic);
}

static int debug_pending(
    interrupt_subsystem_t *ic, interrupt_request_t *request) {
    if (!ic->debug_valid ||
        (ic->debug_value & (XIC_IR | XIC_IE)) != (XIC_IR | XIC_IE))
        return 0;
    request->source_token = INTERRUPT_NO_SOURCE;
    request->ic_addr = ic->debug_addr;
    request->trap = ic->debug_trap;
    request->ilvl = ic->debug_ilvl;
    return 1;
}

int cemu_interrupt_subsystem_pending(
    interrupt_subsystem_t *ic, interrupt_request_t *request) {
    if (ic->collect_cache_stats) ic->cache_stats.queries++;
    if (ic->cache_valid && ic->cached_generation == ic->generation) {
        if (ic->collect_cache_stats) ic->cache_stats.hits++;
        if (ic->cached_pending) *request = ic->cached_request;
        return ic->cached_pending;
    }

    if (ic->collect_cache_stats) ic->cache_stats.scans++;
    interrupt_request_t winner;
    int found = debug_pending(ic, &winner);
    if (!found) {
        int best = -1, best_ilvl = -1, best_xglvl = -1;
        for (int i = 0; i < ic->n_sources; i++) {
            interrupt_source_t *source = &ic->sources[i];
            uint16_t value = ic->source_values[i];
            if (!source->has_trap ||
                (value & (XIC_IR | XIC_IE)) != (XIC_IR | XIC_IE))
                continue;
            int ilvl = (value >> 2) & 0xF;
            int xglvl = (((value >> 8) & 1) << 2) | (value & 0x3);
            if (ilvl > best_ilvl ||
                (ilvl == best_ilvl && xglvl > best_xglvl)) {
                best = i;
                best_ilvl = ilvl;
                best_xglvl = xglvl;
            }
        }
        if (best >= 0) {
            winner.source_token = (uint32_t)best;
            winner.ic_addr = ic->sources[best].addr;
            winner.trap = ic->sources[best].trap;
            winner.ilvl = best_ilvl;
            found = 1;
        }
    }

    ic->cached_generation = ic->generation;
    ic->cached_pending = found;
    ic->cache_valid = 1;
    if (found) {
        ic->cached_request = winner;
        *request = winner;
    }
    return found;
}

static int request_is_registered(
    interrupt_subsystem_t *ic, const interrupt_request_t *request) {
    if (request->source_token >= (uint32_t)ic->n_sources) return 0;
    interrupt_source_t *source = &ic->sources[request->source_token];
    return source->addr == word_addr(request->ic_addr) &&
           source->trap == request->trap;
}

interrupt_service_t cemu_interrupt_subsystem_begin_service(
    interrupt_subsystem_t *ic, const interrupt_request_t *request) {
    if (!request_is_registered(ic, request))
        return INTERRUPT_SERVICE_CPU;
    pec_transfer_t transfer;
    if (!cemu_pec_prepare_interrupt(
            &ic->pec, ic->soc, request->ic_addr, request->ilvl, &transfer))
        return INTERRUPT_SERVICE_CPU;
    if (transfer.clear_source_ir)
        cemu_memory_controller_sfr_put(&ic->soc->memory, request->ic_addr,
                    memory_controller_sfr_get(&ic->soc->memory, request->ic_addr) &
                    (uint16_t)~XIC_IR);
    cemu_pec_execute_transfer(&ic->pec, ic->soc, &transfer);
    return INTERRUPT_SERVICE_PEC;
}

void cemu_interrupt_subsystem_acknowledge(
    interrupt_subsystem_t *ic, const interrupt_request_t *request) {
    if (request->source_token != INTERRUPT_NO_SOURCE &&
        !request_is_registered(ic, request))
        return;
    uint32_t addr = word_addr(request->ic_addr);
    cemu_memory_controller_sfr_put(&ic->soc->memory, addr,
                memory_controller_sfr_get(&ic->soc->memory, addr) & (uint16_t)~XIC_IR);
}

int cemu_interrupt_subsystem_idle_wake_possible(interrupt_subsystem_t *ic) {
    interrupt_request_t request;
    if (cemu_interrupt_subsystem_pending(ic, &request)) return 1;
    for (int i = 0; i < ic->n_sources; i++) {
        interrupt_source_t *source = &ic->sources[i];
        if (!source->has_trap || !source->owner ||
            !source->owner->ic_will_fire ||
            !(ic->source_values[i] & XIC_IE))
            continue;
        if (source->owner->ic_will_fire(
                source->owner, ic->soc, source->addr))
            return 1;
    }
    return 0;
}

int cemu_interrupt_subsystem_finite_pec_will_progress(
    interrupt_subsystem_t *ic, uint16_t psw) {
    if (!(psw & (1u << 11))) return 0;
    int cpu_ilvl = (psw >> 12) & 0xF;
    for (int i = 0; i < ic->n_sources; i++) {
        interrupt_source_t *source = &ic->sources[i];
        uint16_t value = ic->source_values[i];
        if (!source->has_trap || !(value & XIC_IE)) continue;
        int ilvl = (value >> 2) & 0xF;
        if (ilvl <= cpu_ilvl ||
            !cemu_pec_channel_has_finite_transfer(
                &ic->pec, ic->soc, source->addr, ilvl))
            continue;
        if (value & XIC_IR) return 1;
        if (source->owner && source->owner->ic_will_fire &&
            source->owner->ic_will_fire(
                source->owner, ic->soc, source->addr))
            return 1;
    }
    return 0;
}

int cemu_interrupt_subsystem_awaited_source_will_fire(
    interrupt_subsystem_t *ic, const uint32_t *polled, int n) {
    for (int i = 0; i < n; i++) {
        int token = cemu_interrupt_subsystem_source_token(ic, polled[i]);
        if (token < 0) continue;
        interrupt_source_t *source = &ic->sources[token];
        if (source->owner && source->owner->ic_will_fire &&
            source->owner->ic_will_fire(
                source->owner, ic->soc, source->addr))
            return 1;
    }
    return 0;
}

void cemu_interrupt_subsystem_debug_inject(
    interrupt_subsystem_t *ic, uint32_t addr, int trap, int ilvl) {
    addr = word_addr(addr);
    if (ilvl < 0) ilvl = 0;
    if (ilvl > 15) ilvl = 15;
    int changed = !ic->debug_valid || ic->debug_addr != addr ||
                  ic->debug_trap != trap || ic->debug_ilvl != ilvl;
    ic->debug_valid = 1;
    ic->debug_addr = addr;
    ic->debug_trap = trap;
    ic->debug_ilvl = ilvl;
    ic->debug_value = memory_controller_sfr_get(&ic->soc->memory, addr);
    if (changed) invalidate(ic);
}

void cemu_interrupt_subsystem_debug_restore(
    interrupt_subsystem_t *ic, int valid, uint32_t addr, int trap, int ilvl) {
    addr = word_addr(addr);
    uint16_t value = valid ? memory_controller_sfr_get(&ic->soc->memory, addr) : 0;
    int changed = ic->debug_valid != valid ||
                  (valid && (ic->debug_addr != addr ||
                             ic->debug_trap != trap ||
                             ic->debug_ilvl != ilvl ||
                             ic->debug_value != value));
    ic->debug_valid = valid;
    ic->debug_addr = addr;
    ic->debug_trap = trap;
    ic->debug_ilvl = ilvl;
    ic->debug_value = value;
    if (changed) invalidate(ic);
    cemu_interrupt_subsystem_resync(ic);
}

void cemu_interrupt_subsystem_collect_cache_stats(
    interrupt_subsystem_t *ic, int enabled) {
    ic->collect_cache_stats = enabled;
}

interrupt_cache_stats_t cemu_interrupt_subsystem_cache_stats(
    const interrupt_subsystem_t *ic) {
    return ic->cache_stats;
}
