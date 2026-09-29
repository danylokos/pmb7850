/* Memory-bus interface between the CPU core and whatever backs the address
 * space (a flat test memory, or the real PMB7850 SoC decode).
 *
 * The CPU issues explicit bus transactions instead of selecting dedicated
 * read8/read16/write8/write16 entrypoints itself. The backing model resolves
 * each transaction to a topology-level route, and may optionally annotate it
 * with a responder device id + responder-owned subtype for observability. */
#ifndef CEMU_BUS_H
#define CEMU_BUS_H

#include <stdint.h>

#ifndef CEMU_INSTRUMENTED
#define CEMU_INSTRUMENTED 1
#endif

typedef enum {
    BUS_ACCESS_FETCH = 0,
    BUS_ACCESS_READ,
    BUS_ACCESS_WRITE,
} bus_access_kind_t;

typedef enum {
    BUS_ROUTE_NONE = 0,
    BUS_ROUTE_INTERNAL_RAM,
    BUS_ROUTE_INTERNAL_IO,
    BUS_ROUTE_SFR,
    BUS_ROUTE_LM,
    BUS_ROUTE_EXTERNAL,
    BUS_ROUTE_XBUS,
    BUS_ROUTE_UNMAPPED,
} bus_route_kind_t;

typedef struct bus_transaction {
    bus_access_kind_t kind;
    uint32_t addr;
    uint32_t value;
    uint8_t size;
#if CEMU_INSTRUMENTED
    bus_route_kind_t route;
    const char *device;
    const char *subtype;
#endif
} bus_transaction_t;

typedef struct bus {
    void *ctx;

    /* 24-bit access transaction. The callee resolves the address, performs the
     * access, and returns the read value/route/annotations in `txn`. */
    void (*access)(void *ctx, bus_transaction_t *txn);

    /* Advance time-based peripherals once per instruction. May be NULL. */
    void (*tick)(void *ctx, int n);

    /* SRST core re-seed (DPPx/CP/SP/... back to reset). May be NULL. */
    void (*reset_core)(void *ctx);
    /* EINIT lifecycle transition (locks CPU SYSCON and raises RSTOUT). */
    void (*end_init)(void *ctx);
} bus_t;

static inline void bus_run(bus_t *bus, bus_transaction_t *txn) {
    bus->access(bus->ctx, txn);
}

static inline uint8_t bus_fetch8(bus_t *bus, uint32_t addr) {
    bus_transaction_t txn = { .kind = BUS_ACCESS_FETCH, .addr = addr & 0xFFFFFF, .size = 1 };
    bus_run(bus, &txn);
    return (uint8_t)(txn.value & 0xFF);
}

static inline uint8_t bus_read8(bus_t *bus, uint32_t addr) {
    bus_transaction_t txn = { .kind = BUS_ACCESS_READ, .addr = addr & 0xFFFFFF, .size = 1 };
    bus_run(bus, &txn);
    return (uint8_t)(txn.value & 0xFF);
}

static inline uint16_t bus_read16(bus_t *bus, uint32_t addr) {
    bus_transaction_t txn = { .kind = BUS_ACCESS_READ, .addr = addr & 0xFFFFFF, .size = 2 };
    bus_run(bus, &txn);
    return (uint16_t)(txn.value & 0xFFFF);
}

static inline void bus_write8(bus_t *bus, uint32_t addr, uint8_t value) {
    bus_transaction_t txn = {
        .kind = BUS_ACCESS_WRITE,
        .addr = addr & 0xFFFFFF,
        .value = value,
        .size = 1,
    };
    bus_run(bus, &txn);
}

static inline void bus_write16(bus_t *bus, uint32_t addr, uint16_t value) {
    bus_transaction_t txn = {
        .kind = BUS_ACCESS_WRITE,
        .addr = addr & 0xFFFFFF,
        .value = value,
        .size = 2,
    };
    bus_run(bus, &txn);
}

#endif /* CEMU_BUS_H */
