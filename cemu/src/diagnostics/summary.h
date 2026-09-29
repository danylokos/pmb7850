/* Run summary renderer — C port of emu/src/boot.py format_run_summary (plain,
 * no ANSI color: the persisted summary.txt form). Reproduces the same sections
 * in the same order — progress, absorbed effects, polled I/O grids, XBUS
 * windows, interrupts, serial console, halt block — so a C run's summary matches
 * the Python-persisted summary.txt byte-for-byte for an equivalent run.
 *
 * Reads only the observe counters (soc) + monitor metrics; call after the run. */
#ifndef CEMU_SUMMARY_H
#define CEMU_SUMMARY_H

#include <stddef.h>
#include "soc.h"
#include "cpu.h"
#include "monitor.h"

#define format_run_summary cemu_format_run_summary

/* Everything the summary needs about how the run ended. */
typedef struct {
    const char *status;      /* limit/halted/idle/spin/waiting_io/... */
    const char *reason;      /* one-line halt reason (may be "") */
    uint64_t steps;
    uint32_t pc;
    double elapsed_s;
    int have_loop;           /* 1 if a loop_report_t verdict fired */
    int serial_already_printed;
    loop_report_t loop;
} run_result_t;

/* Render the full summary into `buf`. `color` gates ANSI SGR styling (mirror
 * boot.py format_run_summary's `color` arg): 1 colorizes with the same style
 * palette as Python, 0 emits plain text (the persisted summary.txt form).
 * `show_writes` mirrors boot.py's `--show-writes`: per-cell polled-I/O entries
 * show read/write counts and write-only cells surface too. Returns the length
 * (may exceed cap-1, in which case output is truncated). */
int format_run_summary(char *buf, int cap, const run_result_t *r,
                       soc_t *soc, cpu_t *cpu, const monitor_t *mon,
                       int color, int show_writes);

#endif /* CEMU_SUMMARY_H */
