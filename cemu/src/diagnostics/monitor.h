/* Loop / progress detector — C port of pemu/src/harness/monitor.py (RunMonitor).
 *
 * Driven one call per executed instruction (monitor_observe), it accumulates the
 * progress metrics the run summary reports (distinct basic blocks / branch
 * targets, hot blocks, per-segment residency, stack extent) and answers whether
 * a hot loop is productive or genuinely stuck — and if stuck, whether it's a
 * wedge (`spin`), a wait on an SFR the model never updates (`waiting_io`), or a
 * wait on DPRAM the model never updates (`waiting_dpram`).
 *
 * The cost model matches Python: the per-step fast path is a handful of int ops;
 * the expensive fingerprint (peek16 over CP+16 GPRs+DPP0-3+PSW+SP) is
 * taken only at exponentially-backed-off checkpoints per back-edge target.
 *
 * Requires cemu_soc_enable_stats() to have been called (the counters it reads). */
#ifndef CEMU_MONITOR_H
#define CEMU_MONITOR_H

#include <stdint.h>
#include <stddef.h>
#include "soc.h"
#include "cpu.h"

#define monitor_init cemu_monitor_init
#define monitor_free cemu_monitor_free
#define monitor_observe cemu_monitor_observe
#define monitor_take_verdict cemu_monitor_take_verdict
#define monitor_branch_targets cemu_monitor_branch_targets
#define monitor_basic_blocks cemu_monitor_basic_blocks
#define monitor_hot_blocks cemu_monitor_hot_blocks

/* A detected-loop verdict (mirror LoopReport). Only populated when observe()
 * returns non-NULL (a stop-worthy loop). */
typedef struct {
    const char *verdict;     /* spin | waiting_io | waiting_dpram */
    uint32_t back_edge_pc;
    uint64_t iterations;
    int cpu_ilvl, cpu_ien;
} loop_report_t;

/* Open-addressing map of 24-bit block addr -> hit count. Used as a plain set
 * (exec_blocks, hits ignored) and as the branch-target histogram (block_hits). */
typedef struct { uint32_t *keys; uint64_t *hits; size_t cap, n; } addrset_t;

/* Per-back-edge checkpoint bookkeeping — a small open-addressing table keyed by
 * the 24-bit back-edge target. */
typedef struct {
    uint32_t target;         /* 0xFFFFFFFF = empty slot */
    uint64_t hits;
    uint64_t next_checkpoint;
    uint64_t fp;             /* state fingerprint hash */
    int      has_fp;
    uint64_t writes, write_blocks, external_ram_mutations, irq, reads;
    uint64_t sfr_digest, dpram_reads, dpram_digest;
    /* Per-target read-count baselines (mirror checkpoint_read_counts /
     * checkpoint_dpram_counts): the SFR/DPRAM read counts as of the last
     * checkpoint, so the next window's digest is scoped to cells whose count
     * actually ROSE this window. Lazily malloc'd on first checkpoint. */
    uint32_t *sfr_base;      /* [SFR_WORDS + INTERNAL_IO_WORDS] or NULL */
    uint32_t *dpram_base;    /* [DPRAM_WORDS] or NULL */
} backedge_t;

typedef struct monitor {
    soc_t *soc;
    cpu_t *cpu;
    uint64_t loop_threshold, max_interval, stall_window;
    int block_cap;

    addrset_t branch_targets;   /* landing addrs -> block_hits histogram */
    addrset_t exec_blocks;      /* distinct executed instruction addrs (set) */

    /* per-segment exec residency (Python RunMonitor.seg_exec). */
    uint64_t seg_exec[256];

    int sp_min, sp_max;   /* -1 = unset */

    backedge_t *be; size_t be_cap, be_n;

    uint32_t ip0; uint8_t csp0;
    uint64_t prev_irq, prev_traps;

    /* global stall watchdog */
    uint64_t stall_steps, stall_blocks, stall_footprint;
    uint64_t stall_external_ram_mutations;
    uint32_t *stall_read_counts;   /* SFR + internal-I/O baseline */

    unsigned instrumentation_subscription;
    int verdict_pending;
    loop_report_t pending_verdict;
} monitor_t;

/* Construct/destroy. thresholds default to Python's 256/65536/64/2_000_000 when
 * passed 0 (except block_cap which uses 64). */
void monitor_init(monitor_t *m, soc_t *soc, cpu_t *cpu,
                  uint64_t loop_threshold, uint64_t stall_window);
void monitor_free(monitor_t *m);

/* Call once per executed instruction (after cemu_cpu_step). Returns 1 and fills
 * `out` if a stop-worthy loop verdict fired this step, else 0. */
int monitor_observe(monitor_t *m, loop_report_t *out);
/* Return and clear a verdict produced by the hub instruction consumer. */
int monitor_take_verdict(monitor_t *m, loop_report_t *out);

/* Metric accessors for the summary. */
size_t monitor_branch_targets(const monitor_t *m);
size_t monitor_basic_blocks(const monitor_t *m);
/* Top-N hot blocks by hits into out_addr/out_hits (descending); returns count. */
int monitor_hot_blocks(const monitor_t *m, int n, uint32_t *out_addr, uint64_t *out_hits);

#endif /* CEMU_MONITOR_H */
