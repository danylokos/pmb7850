/* Minimal test harness for the C166S CPU port — mirrors emu/tests/cpu/conftest.py.
 *
 * Provides a flat 16 MiB little-endian FakeSoC bus (RAM/SFR/GPR-bank all in one
 * array, the only contract the CPU relies on), a make_cpu() that seeds identity
 * DPP + CP/SP like the Python fixture, and run()/assert_flags() equivalents. A
 * tiny CHECK/RUN_TEST assertion+registry keeps the vectors readable one-per-func
 * exactly like the pytest files they're ported from. */
#ifndef CEMU_TEST_HARNESS_H
#define CEMU_TEST_HARNESS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cpu.h"
#include "bus.h"

/* ---- flat-memory FakeSoC ---------------------------------------------- */
#define FAKE_SIZE (1u << 24)

typedef struct fake_soc {
    uint8_t *mem;
} fake_soc_t;

static bus_transaction_t g_last_txn;

static uint8_t fs_mem_read8(fake_soc_t *s, uint32_t a) {
    return s->mem[a & 0xFFFFFF];
}
static uint16_t fs_mem_read16(fake_soc_t *s, uint32_t a) {
    uint8_t *m = s->mem; a &= 0xFFFFFF;
    return m[a] | ((uint16_t)m[a + 1] << 8);
}
static void fs_mem_write8(fake_soc_t *s, uint32_t a, uint8_t v) {
    s->mem[a & 0xFFFFFF] = v;
}
static void fs_mem_write16(fake_soc_t *s, uint32_t a, uint16_t v) {
    uint8_t *m = s->mem; a &= 0xFFFFFF;
    m[a] = v & 0xFF; m[a + 1] = (v >> 8) & 0xFF;
}
static void fs_access(void *ctx, bus_transaction_t *txn) {
    fake_soc_t *s = (fake_soc_t *)ctx;
    g_last_txn = *txn;
    txn->route = BUS_ROUTE_INTERNAL_RAM;
    if (txn->kind == BUS_ACCESS_WRITE) {
        if (txn->size == 2) fs_mem_write16(s, txn->addr, (uint16_t)txn->value);
        else fs_mem_write8(s, txn->addr, (uint8_t)txn->value);
        return;
    }
    if (txn->size == 2) txn->value = fs_mem_read16(s, txn->addr);
    else txn->value = fs_mem_read8(s, txn->addr);
}
static void     fs_tick(void *ctx, int n) { (void)ctx; (void)n; }
/* Settable pending interrupt (mirrors the Python FakeSoC's `_pending`). */
static int      g_pend_valid, g_pend_trap, g_pend_ilvl;
static uint32_t g_pend_ic;
static int fs_pending(void *ctx, interrupt_request_t *request) {
    (void)ctx;
    if (!g_pend_valid) return 0;
    request->source_token = 0;
    request->trap = g_pend_trap;
    request->ic_addr = g_pend_ic;
    request->ilvl = g_pend_ilvl;
    return 1;
}
static interrupt_service_t fs_begin_service(
    void *ctx, const interrupt_request_t *request) {
    (void)ctx;
    (void)request;
    return INTERRUPT_SERVICE_CPU;
}
static void fs_acknowledge(void *ctx, const interrupt_request_t *request) {
    fake_soc_t *s = ctx;
    uint16_t value = fs_mem_read16(s, request->ic_addr);
    fs_mem_write16(s, request->ic_addr, value & (uint16_t)~(1u << 7));
}

/* Global bus/soc/cpu used by the per-test macros (one active test at a time). */
static fake_soc_t g_soc;
static bus_t      g_bus;
static interrupt_port_t g_interrupt_port;
static cpu_t      g_cpu;

static void make_cpu(void) {
    if (!g_soc.mem) g_soc.mem = calloc(FAKE_SIZE, 1);
    memset(g_soc.mem, 0, FAKE_SIZE);
    g_bus.ctx = &g_soc;
    g_bus.access = fs_access;
    g_bus.tick = fs_tick;
    g_bus.reset_core = NULL;
    g_bus.end_init = NULL;
    g_pend_valid = 0;
    g_interrupt_port.ctx = &g_soc;
    g_interrupt_port.pending = fs_pending;
    g_interrupt_port.begin_service = fs_begin_service;
    g_interrupt_port.acknowledge = fs_acknowledge;

    /* Identity DPP mapping (DPPx = x); CP/SP in low DPRAM; PSW cleared. */
    for (int i = 0; i < 4; i++) fs_mem_write16(&g_soc, 0xFE00 + 2 * i, i);
    fs_mem_write16(&g_soc, 0xFE10, 0xFC00);   /* CP */
    fs_mem_write16(&g_soc, 0xFE12, 0xFB00);   /* SP */
    fs_mem_write16(&g_soc, 0xFF10, 0x0000);   /* PSW */
    cemu_cpu_init(&g_cpu, &g_bus);
    cemu_cpu_attach_interrupt_port(&g_cpu, &g_interrupt_port);
    g_cpu.bus = &g_bus;
    /* Match Python make_cpu: trace off (no-op here) then reset(). */
    cemu_cpu_reset(&g_cpu);
}

/* Load `code` at `at` (seg 0), point PC there, single-step. */
static void run_code(const uint8_t *code, int n, uint32_t at) {
    memcpy(g_soc.mem + at, code, n);
    g_cpu.csp = (at >> 16) & 0xFF;
    g_cpu.ip = at & 0xFFFF;
    cemu_cpu_step(&g_cpu);
}
#define RUN(...) do { uint8_t _c[] = {__VA_ARGS__}; run_code(_c, (int)sizeof(_c), 0x0000); } while (0)
#define RUN_AT(at, ...) do { uint8_t _c[] = {__VA_ARGS__}; run_code(_c, (int)sizeof(_c), (at)); } while (0)

/* Load `code` at `at`, point PC there, but DON'T step (for multi-step tests). */
static void load_at(const uint8_t *code, int n, uint32_t at) {
    memcpy(g_soc.mem + at, code, n);
    g_cpu.csp = (at >> 16) & 0xFF;
    g_cpu.ip = at & 0xFFFF;
}
#define LOAD_AT(at, ...) do { uint8_t _c[] = {__VA_ARGS__}; load_at(_c, (int)sizeof(_c), (at)); } while (0)

#define GPR(n)      cemu_cpu_gpr(&g_cpu, (n))
#define SET_GPR(n,v) cemu_cpu_set_gpr(&g_cpu, (n), (uint16_t)(v))
#define PSW()       cemu_cpu_psw(&g_cpu)
#define SET_PSW(v)  cemu_cpu_set_psw(&g_cpu, (uint16_t)(v))
#define MEM16(a)    fs_mem_read16(&g_soc, (a))
#define MEM8(a)     fs_mem_read8(&g_soc, (a))
#define SET_MEM16(a,v) fs_mem_write16(&g_soc, (a), (uint16_t)(v))
#define SET_MEM8(a,v)  fs_mem_write8(&g_soc, (a), (uint8_t)(v))
#define STEP()      cemu_cpu_step(&g_cpu)
#define IP()        (g_cpu.ip)
#define CSP()       (g_cpu.csp)
#define PC()        cpu_pc(&g_cpu)

/* ---- assertion + test registry ---------------------------------------- */
static int g_fail;        /* failures in the current test */
static int g_total_fail;  /* failures across the whole run */
static int g_total_run;
static const char *g_cur;

#define CHECK(cond) do { \
    if (!(cond)) { \
        g_fail++; \
        printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* Flag checks: each takes the expected bool. */
#define CHECK_FLAG(mask, want) do { \
    int _have = (PSW() & (mask)) != 0; \
    if (_have != (want)) { g_fail++; \
        printf("  FAIL %s:%d  flag %s: want %d got %d (PSW=%#06x)\n", \
               __FILE__, __LINE__, #mask, (want), _have, PSW()); } \
} while (0)

typedef void (*test_fn)(void);

typedef struct { const char *name; test_fn fn; } test_entry_t;

/* Each test file defines TESTS[] and calls run_all(). */
static int run_all(const test_entry_t *tests, int count) {
    for (int i = 0; i < count; i++) {
        g_fail = 0;
        g_cur = tests[i].name;
        make_cpu();
        tests[i].fn();
        g_total_run++;
        if (g_fail) { g_total_fail++; printf("[FAIL] %s (%d)\n", g_cur, g_fail); }
    }
    printf("\n%d tests, %d failed\n", g_total_run, g_total_fail);
    return g_total_fail ? 1 : 0;
}

#endif /* CEMU_TEST_HARNESS_H */
