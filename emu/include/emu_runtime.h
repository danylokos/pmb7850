#ifndef EMU_RUNTIME_H
#define EMU_RUNTIME_H

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "emu_patch.h"
#include "emu_runtime_state.h"
#include "emu_trace.h"

typedef struct {
    const char *flash_path;
    const char *device_name;
    const char *from_snapshot;
    const char *eeprom_overlay_path;
    const char *ui_socket_path;
    const char *serial_pty_path;
    const char *qemu_binary;
    unsigned gdb_port;
    int gdb_enabled;
    const char *gdb_binary;
    size_t gdb_action_count;
    emu_gdb_action_t gdb_actions[EMU_GDB_MAX_ACTIONS];
    const char *synth_spec;
    int patch_list;
    const char *fsn_hex;
    const char *imei;
    uint64_t limit;
    int limit_explicit;
    int benchmark_json;
    int batch_idle;
    int help;

    const char *label;
    const char *dbg_command;
    const char *dbg_script;
    int dbg_interactive;
    int want_trace;
    int trace_list;
    emu_trace_mask_t trace_mask;
    int gsm_l1_trace_explicit;
    int want_drcov;
    int want_lcd_frames;
    int want_lcd_ddram_frames;
    int want_snapshot;
    int want_snapshot_full_bins;
    int want_dump_flash;
    uint64_t snapshot_at;
    int snapshot_at_set;
    uint64_t trace_from_icount;
    int trace_from_icount_set;
    long trace_from_pc;
    int summary;
    int show_writes;
    int monitor;
    uint64_t monitor_loop_threshold;
    uint64_t monitor_stall_window;
    int monitor_loop_threshold_set;
    int monitor_stall_window_set;

    int sim_stub;
    unsigned battery_level;
    int battery_level_set;
    int battery_charging;
    int battery_charging_set;
    unsigned synth_mask;
    emu_patch_set_t firmware_patches;
    uint32_t fsn;
    int fsn_set;
} emu_cli_options_t;

void emu_cli_options_init(emu_cli_options_t *,
                          const emu_engine_diagnostics_t *);
int emu_cli_parse(emu_cli_options_t *, int, char **, FILE *,
                  const emu_engine_diagnostics_t *);
void emu_cli_usage(FILE *, const char *, const emu_engine_diagnostics_t *);
int emu_cli_required_diagnostics(int, char **, uint64_t *, FILE *);
int emu_cli_argument_span(const char *, const char *, uint64_t *, FILE *);
int emu_cemu_runtime_main(int, char **, const emu_engine_descriptor_t *);
int emu_qemu_runtime_main(int, char **, const emu_engine_descriptor_t *,
                          const char *);

typedef struct {
    const char *status;
    const char *reason;
    const char *device;
    const char *device_source;
    const emu_image_metadata_t *image;
    size_t source_size;
    size_t flash_primary_offset;
    size_t flash_secondary_offset;
    emu_patch_set_t firmware_patches;
    uint64_t start_icount;
    uint64_t end_icount;
    uint64_t ticks;
    uint64_t guest_instructions;
    double elapsed_seconds;
    uint32_t pc;
    uint64_t state_digest;
    uint64_t interrupt_cache_queries;
    uint64_t interrupt_cache_hits;
    uint64_t interrupt_cache_scans;
    uint64_t interrupt_cache_invalidations;
} emu_benchmark_record_t;

void emu_benchmark_print(FILE *, const emu_benchmark_record_t *);
uint64_t emu_runtime_elapsed_ns(const struct timespec *, const struct timespec *);
int emu_runtime_ticker_enabled(int benchmark_json, int stdout_is_tty);
uint64_t emu_runtime_next_allowance(uint64_t current, uint64_t remaining,
                                    const uint64_t *deadlines, size_t count);
/* Estimate the next host observation from the latest tick/time interval. */
uint64_t emu_runtime_sample_allowance(uint64_t ticks, uint64_t interval_ns,
                                      uint64_t since_publication_ns);
typedef struct {
    uint64_t interval_ns, displayed_ns;
    int has_displayed;
    unsigned visible_rows;
    int console_partial;
    char warning[256];
    emu_runtime_snapshot_t snapshot;
    emu_runtime_lifecycle_t lifecycle;
} emu_runtime_ticker_t;
void emu_runtime_draw_ticker(FILE *, const emu_runtime_snapshot_t *,
                             const emu_runtime_lifecycle_t *);
/* Refresh reads only published state; force is for bootstrap/final delivery. */
int emu_runtime_ticker_refresh(FILE *, emu_runtime_ticker_t *,
                               const emu_runtime_state_t *, uint64_t now_ns, int force);
/* Live text occupies the terminal below the parked cursor. Clear before any
 * other output; restore preserves the cached sample and refresh deadline. */
void emu_runtime_ticker_restore(FILE *, emu_runtime_ticker_t *);
void emu_runtime_ticker_clear(FILE *, emu_runtime_ticker_t *);
void emu_runtime_ticker_finish(FILE *, emu_runtime_ticker_t *);
void emu_runtime_ticker_console(FILE *, emu_runtime_ticker_t *,
                                emu_console_kind_t, const char *, size_t);
int emu_runtime_install_stop_handlers(void);
void emu_runtime_reset_stop_signal(void);
int emu_runtime_stop_signal(void);
const char *emu_runtime_classify_status(const char *, int, int, int);
int emu_runtime_exit_code(int artifact_failed, int signal_number);

#endif
