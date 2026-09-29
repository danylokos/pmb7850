/* Full-state snapshot writer — C mirror of pemu/src/harness/snapshot.py
 * write_snapshot_dir. One snapshot = one directory:
 *   <dir>/snapshot.json       scalars + SFR + serial + flash index + startup metadata
 *   <dir>/ram_<start>.bin     one sparse chunk per contiguous written-RAM region
 *   <dir>/flash_<start>.bin   one sparse chunk per contiguous committed flash-overlay region
 *   <dir>/full-bins/          optional CPU-visible fixed-boundary import bins
 * so a multi-MiB RAM/flash footprint is many small blobs, not one giant file —
 * the shape Ghidra/analysis tooling and tools/cmp_snapshot.py consume. RAM and
 * flash regions are coalesced gap=0 / keep-zero so restore is byte-exact. */
#ifndef CEMU_SNAPSHOT_H
#define CEMU_SNAPSHOT_H

#include "soc.h"

#define snapshot_read_dir cemu_snapshot_read_dir
#define snapshot_write_dir cemu_snapshot_write_dir
#define snapshot_write_dir_ex cemu_snapshot_write_dir_ex
#include "cpu.h"
#if CEMU_INSTRUMENTED
typedef struct {
    /* Write snapshot/full-bins/ Ghidra import blobs in addition to the canonical
     * sparse restore snapshot. Restore ignores these files. */
    int full_bins;
    const char *host_json;
} snapshot_write_options_t;

/* Write a snapshot of the live CPU+SoC into directory `dir` (created if needed;
 * an existing snapshot.json there is overwritten). `flash_path` (may be NULL) is
 * recorded in startup metadata; mutable flash state is recorded in the snapshot itself.
 * Returns 0 on success, -1 on I/O error. */
int snapshot_write_dir(const char *dir, cpu_t *cpu, soc_t *soc,
                       const char *flash_path);

int snapshot_write_dir_ex(const char *dir, cpu_t *cpu, soc_t *soc,
                          const char *flash_path,
                          snapshot_write_options_t opts);
#endif

/* Inject a snapshot's state into an already-built cpu+soc (the SoC must have been
 * built from the snapshot's recorded immutable flash).
 * Overwrites cpu scalars, SFR words, RAM regions, serial buffer, and mutable flash
 * state. Returns 0 on success, -1 on error. */
int snapshot_read_dir(const char *dir, cpu_t *cpu, soc_t *soc);

#endif /* CEMU_SNAPSHOT_H */
