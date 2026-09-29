/* C166S interrupt and Peripheral Event Controller subsystem. */
#ifndef CEMU_INTERRUPTS_H
#define CEMU_INTERRUPTS_H

#include <stdint.h>
#include "interrupt.h"
#include "peripheral.h"
#include "pec.h"

#define INTERRUPT_MAX_SOURCES 64
#define INTERRUPT_SFR_WORDS   0x0800
#define INTERRUPT_NO_SOURCE   UINT32_MAX
#define XIC_IR_BIT            (1u << 7)
#define XIC_IE_BIT            (1u << 6)

typedef struct {
    uint32_t addr;
    int trap;
    int has_trap;
    int is_timer;
    peripheral_t *owner;
} interrupt_source_t;

typedef struct {
    uint64_t queries;
    uint64_t scans;
    uint64_t hits;
    uint64_t invalidations;
} interrupt_cache_stats_t;

typedef struct interrupt_subsystem {
    soc_t *soc;
    interrupt_source_t sources[INTERRUPT_MAX_SOURCES];
    uint16_t source_values[INTERRUPT_MAX_SOURCES];
    int16_t source_by_sfr[INTERRUPT_SFR_WORDS];
    int n_sources;

    uint64_t generation;
    uint64_t cached_generation;
    interrupt_request_t cached_request;
    int cache_valid;
    int cached_pending;
    int collect_cache_stats;
    interrupt_cache_stats_t cache_stats;

    int debug_valid;
    uint32_t debug_addr;
    int debug_trap;
    int debug_ilvl;
    uint16_t debug_value;

    pec_engine_t pec;
    peripheral_t pec_endpoint;
    interrupt_port_t port;
} interrupt_subsystem_t;

void cemu_interrupt_subsystem_init(interrupt_subsystem_t *ic, soc_t *soc);
int cemu_interrupt_subsystem_register(
    interrupt_subsystem_t *ic, uint32_t addr, int trap, int is_timer,
    peripheral_t *owner);
int cemu_interrupt_subsystem_source_token(
    const interrupt_subsystem_t *ic, uint32_t addr);

int cemu_interrupt_subsystem_pending(
    interrupt_subsystem_t *ic, interrupt_request_t *request);
interrupt_service_t cemu_interrupt_subsystem_begin_service(
    interrupt_subsystem_t *ic, const interrupt_request_t *request);
void cemu_interrupt_subsystem_acknowledge(
    interrupt_subsystem_t *ic, const interrupt_request_t *request);

void cemu_interrupt_subsystem_sfr_changed(
    interrupt_subsystem_t *ic, uint32_t addr, uint16_t old_value,
    uint16_t new_value);
void cemu_interrupt_subsystem_resync(interrupt_subsystem_t *ic);
void cemu_interrupt_subsystem_resync_addr(interrupt_subsystem_t *ic, uint32_t addr);

int cemu_interrupt_subsystem_idle_wake_possible(interrupt_subsystem_t *ic);
int cemu_interrupt_subsystem_finite_pec_will_progress(
    interrupt_subsystem_t *ic, uint16_t psw);
int cemu_interrupt_subsystem_awaited_source_will_fire(
    interrupt_subsystem_t *ic, const uint32_t *polled, int n);

void cemu_interrupt_subsystem_debug_inject(
    interrupt_subsystem_t *ic, uint32_t addr, int trap, int ilvl);
void cemu_interrupt_subsystem_debug_restore(
    interrupt_subsystem_t *ic, int valid, uint32_t addr, int trap, int ilvl);

void cemu_interrupt_subsystem_collect_cache_stats(
    interrupt_subsystem_t *ic, int enabled);
interrupt_cache_stats_t cemu_interrupt_subsystem_cache_stats(
    const interrupt_subsystem_t *ic);

#endif /* CEMU_INTERRUPTS_H */
