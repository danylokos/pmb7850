/* Peripheral model interface — C mirror of emu/src/soc/peripherals/base.py.
 *
 * A peripheral is a self-contained model that plugs into the SoC's memory/tick
 * machinery through this vtable instead of being an inline branch in the SoC. It
 * *claims* the SFR words / byte ranges it services and the interrupt nodes it
 * owns, and overrides the hooks it needs. Like the Python base class, a peripheral
 * NEVER owns raw SFR word storage — the memory controller's SFR array stays the
 * single source of truth; a peripheral computes derived read bits, reacts to
 * writes/polls, ticks, and owns only its private non-SFR state (prescaler
 * accumulators, flash mode) via `state`.
 *
 * Every hook takes (self, soc): the peripheral reaches memory through
 * `soc->memory`, keeping the fast path allocation-free. */
#ifndef CEMU_PERIPHERAL_H
#define CEMU_PERIPHERAL_H

#include <stdint.h>
#include <stddef.h>

#ifndef CEMU_INSTRUMENTED
#define CEMU_INSTRUMENTED 1
#endif

typedef struct soc soc_t;            /* defined in soc.h */
typedef struct peripheral peripheral_t;

#define MAX_PERIPHERALS 27

/* Closed linear-address byte range [start, end] (both inclusive). */
typedef struct { uint32_t start, end; } addr_range_t;

/* A deliverable classic-xIC register this peripheral owns. trap < 0 means the
 * node has no known vector-table trap (declared but not deliverable). */
typedef struct { uint32_t addr; int trap; int is_timer; } ic_node_t;

/* A register this peripheral NAMES (mirror Python REGISTER_NAMES). A named
 * SFR/ESFR word is what makes a cell "modeled" (green) in the run summary — a
 * word a peripheral merely hooks/claims for behavior but does not name (a shared
 * port, a flag aliasing a core reg) stays unmodeled. DPRAM names are declared
 * here too (they feed the DPRAM cell labels), same as _peripheral_names. */
typedef struct { uint32_t addr; const char *name; } reg_name_t;

struct peripheral {
    const char *id;      /* stable logical role used by selectors/traces */
    const char *model;   /* optional concrete part number */
    void *state;         /* private per-peripheral state */

    /* Declarative claims (static tables owned by the peripheral). */
    const uint32_t   *sfr_words;    int n_sfr_words;
    const addr_range_t *byte_ranges; int n_byte_ranges;
    const ic_node_t  *ic_nodes;     int n_ic_nodes;
    const reg_name_t *reg_names;    int n_reg_names;   /* REGISTER_NAMES */

    /* Byte-range memory hooks. read8/peek8 return the byte, or -1 to fall
     * through to normal decode. write8 returns 1 if it reacted (the store itself
     * always happens in the SoC first), else 0. Any may be NULL. */
    int (*read8)(peripheral_t *self, soc_t *s, uint32_t addr);
    int (*peek8)(peripheral_t *self, soc_t *s, uint32_t addr);
    int (*write8)(peripheral_t *self, soc_t *s, uint32_t addr, uint8_t value);

    /* SFR-word hooks over claimed words. Any may be NULL. */
    uint16_t (*read_sfr_word)(peripheral_t *self, soc_t *s, uint32_t word_addr, uint16_t stored);
    void (*on_sfr_poll)(peripheral_t *self, soc_t *s, uint32_t word_addr);
    void (*on_sfr_write)(peripheral_t *self, soc_t *s, uint32_t word_addr, uint16_t stored);

    /* Advance autonomous state by n instruction-ticks. May be NULL. */
    void (*tick)(peripheral_t *self, soc_t *s, int n);

    /* IDLE batching contract. next_event_ticks returns the number of ticks
     * until the next tick that must use the canonical tick path:
     *   0          cannot prove a quiet interval
     *   UINT64_MAX no autonomous event is currently possible
     *   N >= 1     the event occurs on the Nth following tick
     * advance_quiet updates only event-free private/guest-visible state for
     * the supplied span and must not emit traces, callbacks, or interrupts. */
    uint64_t (*next_event_ticks)(peripheral_t *self, soc_t *s);
    void (*advance_quiet)(peripheral_t *self, soc_t *s, uint64_t ticks);

    /* For idle-wake: 1 if the timer feeding `ic_addr` is running (will set its
     * request flag on overflow). NULL for non-timer peripherals. Mirrors the
     * Python ic_pending_map() running predicate. */
    int (*timer_running)(peripheral_t *self, soc_t *s, uint32_t ic_addr);

    /* For idle/monitor future-wake prediction: 1 if `ic_addr` can become
     * pending autonomously from the current modeled state (for example a timer
     * overflow or CAPCOM compare on a running timer). NULL if unknown. */
    int (*ic_will_fire)(peripheral_t *self, soc_t *s, uint32_t ic_addr);
};

#if CEMU_INSTRUMENTED
#define PERIPHERAL_REG_NAMES(p, table, count) do { \
    (p)->reg_names = (table); (p)->n_reg_names = (count); \
} while (0)
#else
#define PERIPHERAL_REG_NAMES(p, table, count) do { \
    (void)sizeof(table); (void)(count); \
    (p)->reg_names = NULL; (p)->n_reg_names = 0; \
} while (0)
#endif

#endif /* CEMU_PERIPHERAL_H */
