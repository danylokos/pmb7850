/* C166S xIC registration, arbitration, cache invalidation, and resync. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "soc.h"
#include "synth.h"

#define S0TIC 0xFF6Cu
#define S0RIC 0xFF6Eu
#define S0EIC 0xFF70u

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static soc_t S;
static uint8_t *flash;
static int future_source_ready;

static int future_source_will_fire(
    peripheral_t *self, soc_t *soc, uint32_t ic_addr) {
    (void)self;
    (void)soc;
    (void)ic_addr;
    return future_source_ready;
}

static void setup(void) {
    if (!flash) {
        flash = malloc(8u * 1024 * 1024);
        memset(flash, 0xFF, 8u * 1024 * 1024);
        flash[0] = 0xFA; flash[1] = 0x80;
        flash[2] = 0xC4; flash[3] = 0x2F;
    }
    device_config_t cfg = *cemu_device_by_name("c55");
    cemu_soc_init(&S, flash, 8u * 1024 * 1024, &cfg, synth_defaults(), 0);
}

static uint16_t xic(int ilvl, int xglvl, int ir, int ie) {
    return (uint16_t)((ir ? XIC_IR_BIT : 0) |
                      (ie ? XIC_IE_BIT : 0) |
                      ((ilvl & 0xF) << 2) |
                      (xglvl & 3) |
                      ((xglvl & 4) << 6));
}

static interrupt_request_t pending(void) {
    interrupt_request_t request = {0};
    CHECK(cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    return request;
}

static void test_registration_and_duplicate_rejection(void) {
    setup();
    int token = cemu_interrupt_subsystem_source_token(&S.interrupts, S0RIC);
    CHECK(token >= 0);
    CHECK(S.interrupts.sources[token].addr == S0RIC);
    CHECK(S.interrupts.sources[token].trap == 0x2B);
    CHECK(S.interrupts.sources[token].owner == S.memory.sfr_hook[memory_controller_sfr_index(S0RIC)]);
    CHECK(!cemu_interrupt_subsystem_register(
        &S.interrupts, S0RIC, 0x2B, 0, S.interrupts.sources[token].owner));
    cemu_soc_free(&S);
}

static void test_ilvl_extended_glvl_and_stable_ties(void) {
    setup();
    bus_write16(&S.bus, S0TIC, xic(5, 1, 1, 1));
    bus_write16(&S.bus, S0RIC, xic(6, 0, 1, 1));
    CHECK(pending().ic_addr == S0RIC);

    bus_write16(&S.bus, S0TIC, xic(6, 5, 1, 1));
    CHECK(pending().ic_addr == S0TIC);

    bus_write16(&S.bus, S0RIC, xic(6, 5, 1, 1));
    CHECK(pending().ic_addr == S0TIC); /* registration order */
    cemu_soc_free(&S);
}

static void test_negative_and_positive_cache_invalidation(void) {
    setup();
    cemu_interrupt_subsystem_collect_cache_stats(&S.interrupts, 1);
    interrupt_request_t request;
    CHECK(!cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    CHECK(!cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    interrupt_cache_stats_t stats =
        cemu_interrupt_subsystem_cache_stats(&S.interrupts);
    CHECK(stats.queries == 2 && stats.scans == 1 && stats.hits == 1);

    uint64_t generation = S.interrupts.generation;
    bus_write16(&S.bus, S0RIC, 0);
    CHECK(S.interrupts.generation == generation);
    CHECK(!cemu_interrupt_subsystem_pending(&S.interrupts, &request));

    bus_write16(&S.bus, S0RIC, xic(4, 2, 1, 1));
    CHECK(S.interrupts.generation == generation + 1);
    CHECK(cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    CHECK(request.ic_addr == S0RIC);
    CHECK(cemu_interrupt_subsystem_pending(&S.interrupts, &request));

    bus_write16(&S.bus, S0TIC, xic(7, 0, 1, 1));
    CHECK(cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    CHECK(request.ic_addr == S0TIC);
    stats = cemu_interrupt_subsystem_cache_stats(&S.interrupts);
    CHECK(stats.queries == 6 && stats.scans == 3 && stats.hits == 3);
    cemu_soc_free(&S);
}

static void test_ie_change_and_acknowledgement(void) {
    setup();
    interrupt_request_t request;
    bus_write16(&S.bus, S0RIC, xic(4, 2, 1, 0));
    CHECK(!cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    bus_write16(&S.bus, S0RIC, xic(4, 2, 1, 1));
    request = pending();
    cemu_interrupt_subsystem_acknowledge(&S.interrupts, &request);
    CHECK((memory_controller_sfr_get(&S.memory, S0RIC) & XIC_IR_BIT) == 0);
    CHECK(!cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    cemu_soc_free(&S);
}

static void test_debugger_request_precedes_registered_source(void) {
    setup();
    bus_write16(&S.bus, S0RIC, xic(15, 7, 1, 1));
    cemu_soc_debug_inject_irq(&S, 0xF130, 0x72, 1);
    interrupt_request_t request = pending();
    CHECK(request.source_token == INTERRUPT_NO_SOURCE);
    CHECK(request.ic_addr == 0xF130);
    CHECK(request.trap == 0x72 && request.ilvl == 1);
    cemu_interrupt_subsystem_acknowledge(&S.interrupts, &request);
    CHECK(pending().ic_addr == S0RIC);
    cemu_soc_free(&S);
}

static void test_future_wake_and_awaited_source_queries(void) {
    setup();
    peripheral_t owner = {
        .id = "future-source",
        .ic_will_fire = future_source_will_fire,
    };
    const uint32_t addr = 0xF130u;
    CHECK(cemu_interrupt_subsystem_register(
        &S.interrupts, addr, 0x72, 0, &owner));

    future_source_ready = 1;
    bus_write16(&S.bus, addr, xic(4, 0, 0, 0));
    CHECK(!cemu_interrupt_subsystem_idle_wake_possible(&S.interrupts));
    CHECK(cemu_interrupt_subsystem_awaited_source_will_fire(
        &S.interrupts, &addr, 1));

    bus_write16(&S.bus, addr, xic(4, 0, 0, 1));
    CHECK(cemu_interrupt_subsystem_idle_wake_possible(&S.interrupts));
    future_source_ready = 0;
    CHECK(!cemu_interrupt_subsystem_idle_wake_possible(&S.interrupts));
    CHECK(!cemu_interrupt_subsystem_awaited_source_will_fire(
        &S.interrupts, &addr, 1));
    cemu_soc_free(&S);
}

static void test_bulk_resynchronization_replaces_cached_result(void) {
    setup();
    interrupt_request_t request;
    CHECK(!cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    S.memory.sfr[memory_controller_sfr_index(S0EIC)] = xic(9, 3, 1, 1);
    CHECK(!cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    cemu_interrupt_subsystem_resync(&S.interrupts);
    CHECK(cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    CHECK(request.ic_addr == S0EIC && request.trap == 0x2C);
    cemu_soc_free(&S);
}

static void test_debugger_restore_resynchronizes_request_value(void) {
    setup();
    const uint32_t addr = 0xF130u;
    interrupt_request_t request;
    cemu_interrupt_subsystem_debug_inject(&S.interrupts, addr, 0x72, 5);
    S.memory.sfr[memory_controller_sfr_index(addr)] = xic(5, 0, 1, 1);
    cemu_interrupt_subsystem_debug_restore(&S.interrupts, 1, addr, 0x72, 5);
    CHECK(cemu_interrupt_subsystem_pending(&S.interrupts, &request));

    S.memory.sfr[memory_controller_sfr_index(addr)] = 0;
    cemu_interrupt_subsystem_debug_restore(&S.interrupts, 1, addr, 0x72, 5);
    CHECK(!cemu_interrupt_subsystem_pending(&S.interrupts, &request));
    cemu_soc_free(&S);
}

typedef struct { const char *name; void (*fn)(void); } test_entry_t;
static const test_entry_t TESTS[] = {
    {"registration_and_duplicate_rejection", test_registration_and_duplicate_rejection},
    {"ilvl_extended_glvl_and_stable_ties", test_ilvl_extended_glvl_and_stable_ties},
    {"negative_and_positive_cache_invalidation", test_negative_and_positive_cache_invalidation},
    {"ie_change_and_acknowledgement", test_ie_change_and_acknowledgement},
    {"debugger_request_precedes_registered_source", test_debugger_request_precedes_registered_source},
    {"future_wake_and_awaited_source_queries", test_future_wake_and_awaited_source_queries},
    {"bulk_resynchronization_replaces_cached_result", test_bulk_resynchronization_replaces_cached_result},
    {"debugger_restore_resynchronizes_request_value", test_debugger_restore_resynchronizes_request_value},
};

int main(void) {
    for (unsigned i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
        g_fail = 0; g_total_run++;
        TESTS[i].fn();
        if (g_fail) printf("FAIL %s (%d)\n", TESTS[i].name, g_fail);
        g_total_fail += g_fail;
    }
    printf("\n%d tests, %d failed\n", g_total_run, g_total_fail);
    free(flash);
    return g_total_fail ? 1 : 0;
}
