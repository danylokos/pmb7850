/* CEMU-native observability events.  This header is part of the emulation
 * core and deliberately has no dependency on diagnostics or EMU types. */
#ifndef CEMU_EVENT_H
#define CEMU_EVENT_H

#include <stddef.h>
#include <stdint.h>

#include "bus.h"

#ifndef CEMU_INSTRUMENTED
#define CEMU_INSTRUMENTED 1
#endif

typedef enum {
    CEMU_EVENT_INSTRUCTION = 1u << 0,
    CEMU_EVENT_BUS         = 1u << 1,
    CEMU_EVENT_CONTROL     = 1u << 2,
    CEMU_EVENT_PROTECTED   = 1u << 3,
    CEMU_EVENT_TRAP        = 1u << 4,
    CEMU_EVENT_DEBUG_IRQ   = 1u << 5,
    CEMU_EVENT_PERIPHERAL  = 1u << 6,
    CEMU_EVENT_FLASH_MUTATION = 1u << 7,
} cemu_event_mask_t;

typedef enum {
    CEMU_FLASH_MUTATION_PROGRAM,
    CEMU_FLASH_MUTATION_ERASE,
} cemu_flash_mutation_kind_t;

/* One guest-committed main-array mutation over the half-open chip-relative
 * range [offset, offset + size). */
typedef struct {
    cemu_flash_mutation_kind_t kind;
    int chip_index;
    const char *chip_name;
    const char *model;
    uint32_t offset;
    uint32_t size;
    uint64_t tick;
    uint64_t icount;
    uint32_t pc;
} cemu_flash_mutation_event_t;

typedef enum {
    CEMU_EVENT_VALUE_I64,
    CEMU_EVENT_VALUE_BOOL,
    CEMU_EVENT_VALUE_NULL,
    CEMU_EVENT_VALUE_STRING,
} cemu_event_value_kind_t;

typedef struct {
    const char *key;
    cemu_event_value_kind_t kind;
    long ival;
    const char *sval;
} cemu_event_field_t;

#define CEMU_EVENT_MAX_FIELDS 32
typedef struct {
    int n;
    cemu_event_field_t kv[CEMU_EVENT_MAX_FIELDS];
} cemu_event_fields_t;

typedef struct cemu_native_trace_event {
    const char *kind;
    uint64_t icount;
    uint32_t pc;
    int has_addr;
    uint32_t addr;
    int has_size;
    int size;
    int has_value;
    uint32_t value;
    const char *detail;
    cemu_event_fields_t info;
} cemu_native_trace_event_t;

typedef struct {
    uint32_t pc_before;
    uint32_t pc_after;
    uint64_t icount;
    int size;
    const char *detail;
} cemu_instruction_event_t;

/* A completed CPU bus transaction with its observation-time stamp and, when
 * the memory controller resolved the transaction to external NOR, the exact
 * physical chip target. `flash_array_data` distinguishes ordinary main-array
 * reads from ID/status/CFI/SecSi responses without exposing protocol state. */
typedef struct {
    const bus_transaction_t *transaction;
    const cemu_native_trace_event_t *trace;
    uint64_t tick;
    uint64_t icount;
    uint32_t pc;
    int has_flash_target;
    int flash_array_data;
    int chip_index;
    const char *chip_name;
    const char *model;
    uint32_t chip_offset;
} cemu_bus_event_t;

typedef struct {
    uint32_t caller_pc;
    uint32_t target_pc;
    const char *kind;
    int is_return;
} cemu_control_event_t;

typedef struct {
    uint32_t addr;
    int trap;
    int ilvl;
} cemu_debug_irq_event_t;

typedef struct cemu_event {
    cemu_event_mask_t type;
    union {
        cemu_instruction_event_t instruction;
        cemu_bus_event_t bus;
        cemu_control_event_t control;
        struct { int slot; } protected_op;
        struct { int trap; } trap;
        cemu_debug_irq_event_t debug_irq;
        struct { const cemu_native_trace_event_t *trace; } peripheral;
        cemu_flash_mutation_event_t flash_mutation;
    } as;
} cemu_event_t;

typedef void (*cemu_event_consumer_fn)(void *, const cemu_event_t *);
typedef int (*cemu_event_filter_fn)(void *, const char *);

#define CEMU_EVENT_MAX_CONSUMERS 12
typedef struct {
    cemu_event_mask_t mask;
    cemu_event_consumer_fn consumer;
    cemu_event_filter_fn filter;
    void *opaque;
    unsigned id;
} cemu_event_subscription_t;

typedef struct cemu_event_hub {
#if CEMU_INSTRUMENTED
    cemu_event_subscription_t consumers[CEMU_EVENT_MAX_CONSUMERS];
    int consumer_count;
    cemu_event_mask_t active_mask;
    unsigned next_id;
    int statistics_enabled;
    /* One generic compatibility attachment is reserved for the legacy CEMU
     * trace bridge.  Its type and policy remain outside the core. */
    void *compatibility_attachment;
    unsigned compatibility_subscription;
#else
    unsigned char disabled;
#endif
} cemu_event_hub_t;

void cemu_event_hub_init(cemu_event_hub_t *hub);
unsigned cemu_event_subscribe(
    cemu_event_hub_t *hub, cemu_event_mask_t mask,
    cemu_event_consumer_fn consumer, void *opaque);
unsigned cemu_event_subscribe_filtered(
    cemu_event_hub_t *hub, cemu_event_mask_t mask,
    cemu_event_consumer_fn consumer, cemu_event_filter_fn filter,
    void *opaque);
void cemu_event_unsubscribe(cemu_event_hub_t *hub, unsigned id);
void cemu_event_emit(cemu_event_hub_t *hub, const cemu_event_t *event);
#if CEMU_INSTRUMENTED
int cemu_event_native_trace_active(
    const cemu_event_hub_t *hub, const char *kind);
void cemu_event_emit_native_trace(
    cemu_event_hub_t *hub, const cemu_native_trace_event_t *trace,
    cemu_event_mask_t type);
void cemu_event_set_statistics(cemu_event_hub_t *hub, int enabled);
int cemu_event_statistics_enabled(const cemu_event_hub_t *hub);
#else
static inline int cemu_event_native_trace_active(
        const cemu_event_hub_t *hub, const char *kind) {
    (void)hub;
    (void)kind;
    return 0;
}

static inline void cemu_event_emit_native_trace(
        cemu_event_hub_t *hub, const cemu_native_trace_event_t *trace,
        cemu_event_mask_t type) {
    (void)hub;
    (void)trace;
    (void)type;
}

static inline void cemu_event_set_statistics(
        cemu_event_hub_t *hub, int enabled) {
    (void)hub;
    (void)enabled;
}

static inline int cemu_event_statistics_enabled(
        const cemu_event_hub_t *hub) {
    (void)hub;
    return 0;
}
#endif

static inline int cemu_event_active(
        const cemu_event_hub_t *hub, cemu_event_mask_t mask) {
#if CEMU_INSTRUMENTED
    return hub && (hub->active_mask & mask) != 0;
#else
    (void)hub;
    (void)mask;
    return 0;
#endif
}

static inline void cemu_event_field_i64(
        cemu_event_fields_t *info, const char *key, long value) {
    info->kv[info->n++] = (cemu_event_field_t){
        key, CEMU_EVENT_VALUE_I64, value, NULL,
    };
}

static inline void cemu_event_field_bool(
        cemu_event_fields_t *info, const char *key, int value) {
    info->kv[info->n++] = (cemu_event_field_t){
        key, CEMU_EVENT_VALUE_BOOL, value != 0, NULL,
    };
}

static inline void cemu_event_field_null(
        cemu_event_fields_t *info, const char *key) {
    info->kv[info->n++] = (cemu_event_field_t){
        key, CEMU_EVENT_VALUE_NULL, 0, NULL,
    };
}

static inline void cemu_event_field_string(
        cemu_event_fields_t *info, const char *key, const char *value) {
    info->kv[info->n++] = (cemu_event_field_t){
        key, CEMU_EVENT_VALUE_STRING, 0, value,
    };
}

#endif
