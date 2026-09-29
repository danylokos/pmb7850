/* Interactive debugger engine — C port of pemu/src/harness/debugger.py.
 *
 * A thin control layer over a live cpu_t/soc_t that turns an opaque boot into
 * something you can stop and inspect: break on a PC or a memory/SFR access, run
 * to it, single-step, read (and now poke) registers/memory, walk a shadow call
 * stack, and checkpoint/rewind. All primitives already exist in the model — this
 * is assembly, not new hardware:
 *   - single-step is cemu_cpu_step(); the debugger owns its own step loop so it can
 *     stop on a condition.
 *   - PC breakpoints: check cpu_pc() before each step.
 *   - access watchpoints: tap the SoC's watch_hook (fired from the canonical
 *     transaction path, so it needs no recorded trace and sees every resolved
 *     CPU access class the bus routes).
 *   - backtrace: the hub publishes typed control-flow events at CALL/RET/trap/IRQ sites; we
 *     keep a shadow call stack from them.
 *   - inspection is event-free (cemu_memory_controller_peek8/16); mutation is event-free poke
 *     (cemu_memory_controller_poke8/16), so neither perturbs the run nor trips our own watchpoint.
 *   - checkpoint/rewind snapshots full machine state in memory.
 *
 * With no debugger attached the CPU/SoC hooks are NULL, so a detached run is
 * byte-for-byte identical (see test_debugger.c). */
#ifndef CEMU_DEBUGGER_H
#define CEMU_DEBUGGER_H

#include <stdint.h>
#include <stdio.h>
#include "cpu.h"
#include "soc.h"
#include "instrumentation.h"
#include "monitor.h"
#include "serial.h"
#include "ssc0.h"
#include "lcd.h"
#include "battery.h"
#include "keypad.h"
#include "xbus_unknown1.h"
#include "twi_gpio.h"

#define debugger_init cemu_debugger_init
#define debugger_detach cemu_debugger_detach
#define debugger_free cemu_debugger_free
#define debugger_set_after_step cemu_debugger_set_after_step
#define debugger_set_batch_idle cemu_debugger_set_batch_idle
#define debugger_set_monitor cemu_debugger_set_monitor
#define debugger_monitor_enabled cemu_debugger_monitor_enabled
#define debugger_set_extension cemu_debugger_set_extension
#define debugger_request_external_stop cemu_debugger_request_external_stop
#define debugger_add_break cemu_debugger_add_break
#define debugger_remove_break cemu_debugger_remove_break
#define debugger_clear_breaks cemu_debugger_clear_breaks
#define debugger_add_watch cemu_debugger_add_watch
#define debugger_add_watch_ex cemu_debugger_add_watch_ex
#define debugger_clear_watches cemu_debugger_clear_watches
#define debugger_step cemu_debugger_step
#define debugger_cont cemu_debugger_cont
#define debugger_regs cemu_debugger_regs
#define debugger_read_mem cemu_debugger_read_mem
#define debugger_read_word cemu_debugger_read_word
#define debugger_read_sfr cemu_debugger_read_sfr
#define debugger_disasm_last cemu_debugger_disasm_last
#define debugger_disasm_at cemu_debugger_disasm_at
#define debugger_write_mem cemu_debugger_write_mem
#define debugger_write_word cemu_debugger_write_word
#define debugger_write_sfr cemu_debugger_write_sfr
#define debugger_set_gpr cemu_debugger_set_gpr
#define debugger_set_reg cemu_debugger_set_reg
#define debugger_backtrace cemu_debugger_backtrace
#define debugger_checkpoint cemu_debugger_checkpoint
#define debugger_restore cemu_debugger_restore
#define stop_reason_str cemu_stop_reason_str
#define debugger_run_script cemu_debugger_run_script
#define debugger_repl cemu_debugger_repl

/* Access classes reported by hub bus events; a watch's kinds mask is OR of these.
 * Device-owned subtype hints are matched inside the debugger from the resolved
 * transaction metadata rather than from SoC-hardcoded access names. */
enum {
    WK_MEM_READ    = 1u << 0,
    WK_MEM_WRITE   = 1u << 1,
    WK_SFR_READ    = 1u << 2,
    WK_SFR_WRITE   = 1u << 3,
    WK_MEM_OVERRIDE= 1u << 4,
    WK_UNMAPPED    = 1u << 5,
    WK_FLASH_WRITE = 1u << 6,
    WK_FLASH_ID    = 1u << 7,
    WK_FETCH       = 1u << 8,
    WK_ALL_DEFAULT = WK_MEM_READ | WK_MEM_WRITE | WK_SFR_READ | WK_SFR_WRITE,
};

/* Why the run loop stopped (mirror StopReason). kind is the discriminator:
 * break | watch | step | halted | idle | limit | unimplemented | memory. */
typedef struct {
    const char *kind;
    uint32_t    pc;           /* stopped/resume PC */
    uint32_t    trigger_pc;   /* watch: instruction that caused the access */
    int         value_change;
    uint8_t     old_value, new_value; /* first changed byte of a value watch */
    uint64_t    icount;
    char        reason[640];
    int         has_addr;
    uint32_t    addr;         /* watch: accessed address */
    const char *access_kind;    /* watch: main access kind string */
    const char *access_device;  /* watch: responder device id, if any */
    const char *access_subtype; /* watch: responder-owned subtype, if any */
    int         has_value;
    uint32_t    value;        /* watch: value read/written */
    int         has_size;
    int         size;         /* watch: access size */
} stop_reason_t;

/* One shadow-call-stack frame: a taken CALL/trap/IRQ not yet returned from. */
typedef struct {
    uint32_t    callee_pc;    /* where control landed (entry / vector) */
    uint32_t    caller_pc;    /* the CALL/trap instruction's PC */
    const char *kind;         /* call | trap | btrap | irq */
    uint16_t    sp;           /* SP right after the frame was pushed */
} frame_t;

#define DBG_MAX_BREAKS   64
#define DBG_MAX_WATCHES  32
#define DBG_MAX_STACK    512

/* The legacy API remains an access watch. Value watches compare the complete
 * range after a matching write instruction, using event-free byte baselines. */
typedef struct {
    uint32_t start, end;
    unsigned kinds;
    int log, dirty;
    uint8_t *baseline; /* non-NULL selects value-change semantics */
} dbg_watch_t;

typedef int (*debugger_extension_command_fn)(
    void *opaque, int argc, char *const *argv, FILE *out);
typedef void (*debugger_extension_help_fn)(void *opaque, FILE *out);
typedef void (*debugger_extension_restore_fn)(void *opaque);

/* An in-memory checkpoint: full machine state + the shadow stack. */
typedef struct dbg_checkpoint {
    uint8_t  *ram;            /* copy of soc->memory.ram (ADDR_SPACE bytes) */
    uint8_t  *present;        /* copy of soc->memory.present */
    uint8_t  *lm;             /* copy of physical local memory */
    uint8_t  *external_ram[MAX_EXTERNAL_RAM_DEVICES];
    uint32_t external_ram_size[MAX_EXTERNAL_RAM_DEVICES];
    int n_external_ram;
    uint16_t  sfr[SFR_WORDS];
    uint8_t  *serial_tx; size_t serial_tx_len;
    uint8_t  *serial_rx; size_t serial_rx_len;
    serial_state_t serial;
    flash_state_t flash[MAX_FLASH_CHIPS];
    int n_flash_chips;
    ssc0_state_t ssc0;
    keypad_mutable_state_t keypad;
    battery_state_t battery;
    xbus_unknown1_state_t xbus_unknown1;
    twi_gpio_state_t twi_gpio;
    lcd_state_storage_t lcd;
    size_t lcd_state_size;
    int has_lcd;
    int has_battery;
    int has_twi_gpio;
    /* cpu scalars */
    uint8_t  csp; uint16_t ip;
    ext_kind_t ext_kind; uint16_t ext_val; int ext_count; int extr;
    int halted, idle;
    uint64_t icount, interrupts_delivered, traps_taken;
    uint64_t ticks;
    int init_locked, rstout;
    int debug_irq_valid;
    uint32_t debug_irq_addr;
    int debug_irq_trap, debug_irq_ilvl;
    /* shadow stack */
    frame_t  stack[DBG_MAX_STACK]; int stack_depth; int stack_underflows;
    int valid;
} dbg_checkpoint_t;

typedef struct debugger {
    cpu_t *cpu;
    soc_t *soc;

    uint32_t breaks[DBG_MAX_BREAKS]; int n_breaks;
    dbg_watch_t watches[DBG_MAX_WATCHES]; int n_watches;

    frame_t stack[DBG_MAX_STACK]; int stack_depth;
    int stack_underflows;

    int           have_watch_hit;   /* set by the access hook during a step */
    stop_reason_t watch_hit;
    uint32_t instruction_pc;
    long          parked_pc;        /* PC we last stopped on (-1 = none) */

    /* last executed instruction, for `dis` */
    int      have_last_exec;
    uint32_t last_exec_pc;
    char     last_exec_text[128];
    int      last_exec_size;

    int capture_disasm;             /* record disasm each step (drives `dis`) */
    unsigned bus_subscription;
    unsigned control_subscription;
    FILE *log_out;                  /* where log-watches print (NULL => stderr) */
    uint64_t watch_log_count;       /* running total of log-watch hits printed */
    void (*after_step)(void *ctx);
    void *after_step_ctx;
    debugger_extension_command_fn extension_command;
    debugger_extension_help_fn extension_help;
    debugger_extension_restore_fn extension_restore;
    void *extension_opaque;
    size_t serial_output_offset;
    /* Optional host cooperation. Same result/phase contract as the adapter. */
    int (*service)(void *, int phase, uint64_t ticks, uint64_t guest);
    int (*input_wait)(void *, int timeout_ms);
    void *service_opaque, *input_wait_opaque;
    int service_result;
    uint64_t executed_ticks, executed_guest;
    unsigned service_countdown;

    dbg_checkpoint_t checkpoint;    /* single live checkpoint slot */

    /* Loop/stall detection during cont() — mirrors the driver so a debugged run
     * ends at the natural spin/waiting_io/waiting_dpram stall instead of grinding
     * until an explicit step limit. Lazily set up on first use; detect_loops
     * toggles it. */
    int      batch_idle;
    int      detect_loops;          /* default 1 (set in debugger_init) */
    monitor_t *mon;                 /* NULL until first cont with detect_loops */
    int      mon_ready;             /* observe counters enabled + monitor built */
} debugger_t;

/* Attach a debugger to a live CPU/SoC (installs the hooks). capture_disasm=1
 * records the last-executed instruction text for `dis`. */
void debugger_init(debugger_t *d, cpu_t *cpu, soc_t *soc, int capture_disasm);
/* Remove all hooks; the CPU/SoC return to un-debugged behavior. */
void debugger_detach(debugger_t *d);
/* Free debugger-owned resources. */
int debugger_free(debugger_t *d);
void debugger_set_after_step(debugger_t *d, void (*fn)(void *ctx), void *ctx);
void debugger_set_batch_idle(debugger_t *d, int enabled);
void debugger_set_extension(
    debugger_t *d, debugger_extension_command_fn command,
    debugger_extension_help_fn help, debugger_extension_restore_fn restore,
    void *opaque);
/* Request a stop from a synchronous event consumer during the current step. */
void debugger_request_external_stop(
    debugger_t *d, const char *kind, uint32_t pc, uint64_t icount,
    const char *reason);


/* Monitoring is off by default. Enabling it starts a fresh baseline at the
 * current machine state; disabling it stops collection and verdict checks. */
void debugger_set_monitor(debugger_t *d, int enabled);
int debugger_monitor_enabled(const debugger_t *d);

/* ---- breakpoints / watchpoints ---------------------------------------- */
void debugger_add_break(debugger_t *d, uint32_t pc);
void debugger_remove_break(debugger_t *d, uint32_t pc);
void debugger_clear_breaks(debugger_t *d);
/* Watch [addr, end] (end==addr for one byte); kinds is an OR of WK_*.
 * log=0 stops the run on the first match; log=1 prints every match to
 * d->log_out and keeps running (set d->log_out before continuing). */
/* Legacy access-watch wrapper. The extended API returns zero on allocation or
 * capacity failure. Value watches compare all bytes, reporting the first change. */
void debugger_add_watch(debugger_t *d, uint32_t addr, uint32_t end, unsigned kinds, int log);
int debugger_add_watch_ex(debugger_t *d, uint32_t addr, uint32_t end,
                          unsigned kinds, int log, int value_change);
void debugger_clear_watches(debugger_t *d);

/* ---- run control ------------------------------------------------------- */
stop_reason_t debugger_step(debugger_t *d, uint64_t n);
stop_reason_t debugger_cont(debugger_t *d, uint64_t max_steps);   /* 0 => no step cap */

/* ---- inspection (event-free) ------------------------------------------ */
typedef struct {
    uint32_t pc; uint64_t icount;
    uint8_t csp; uint16_t ip;
    uint16_t psw, sp, cp, dpp[4], gpr[16];
    int halted, idle;
} dbg_regs_t;
void     debugger_regs(debugger_t *d, dbg_regs_t *out);
void     debugger_read_mem(debugger_t *d, uint32_t addr, uint8_t *out, int n);
uint16_t debugger_read_word(debugger_t *d, uint32_t addr);
uint16_t debugger_read_sfr(debugger_t *d, uint32_t addr);
/* Text of the last executed instruction (or raw bytes if disasm off). */
void     debugger_disasm_last(debugger_t *d, char *buf, int cap);
/* Disassemble the instruction at an explicit addr into `buf`; returns its
 * length in bytes (so a caller can advance to the next instruction). */
int      debugger_disasm_at(debugger_t *d, uint32_t addr, char *buf, int cap);

/* ---- mutation (event-free poke — new, beyond Python parity) ----------- */
void debugger_write_mem(debugger_t *d, uint32_t addr, const uint8_t *bytes, int n);
void debugger_write_word(debugger_t *d, uint32_t addr, uint16_t val);
void debugger_write_sfr(debugger_t *d, uint32_t addr, uint16_t val);
void debugger_set_gpr(debugger_t *d, int n, uint16_t val);
/* Set a named CPU register (pc/sp/cp/psw/ip/csp/dpp0..3/r0..r15). Returns 1 on a
 * recognized name, 0 otherwise. */
int  debugger_set_reg(debugger_t *d, const char *name, uint32_t val);

/* ---- backtrace --------------------------------------------------------- */
/* Copy frames innermost-first (frame 0 = current function) into out; returns
 * the count (<= cap). */
int debugger_backtrace(debugger_t *d, frame_t *out, int cap);

/* ---- checkpoint / rewind ---------------------------------------------- */
void debugger_checkpoint(debugger_t *d);
int  debugger_restore(debugger_t *d);   /* 1 if a checkpoint existed, else 0 */

/* Format a stop reason like StopReason.__str__ into buf. */
void stop_reason_str(const stop_reason_t *s, char *buf, int cap);

/* ---- session (script + REPL), mirror tools/debug.py ------------------- */
/* Run one ';'-separated (or newline-separated) command script, printing to out.
 * Returns 0 normally, 1 if a `quit` ended it. */
int  debugger_run_script(debugger_t *d, const char *script, FILE *out);
/* Interactive REPL reading from `in`, writing to `out`. */
void debugger_repl(debugger_t *d, FILE *in, FILE *out);

#endif /* CEMU_DEBUGGER_H */
