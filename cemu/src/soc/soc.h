/* PMB7850 (E-GOLD+ V3) SoC composition and peripheral lifecycle.
 * emu/src/soc/pmb7850.py (peripherals live under src/peripherals/, mirroring the
 * Python soc/peripherals/ split; they register via a table rather than inline
 * branches here).
 *
 * Memory storage and routing live in memory_controller_t; interrupt arbitration
 * and PEC live in interrupt_subsystem_t. The SoC composes those subsystems with
 * peripherals, lifecycle state, and instrumentation. */
#ifndef CEMU_SOC_H
#define CEMU_SOC_H

#include <stdint.h>
#include <stddef.h>
#include "bus.h"
#include "devices.h"
#include "peripheral.h"
#include "cemu_event.h"
#include "cpu.h"
#include "interrupts.h"
#include "memory_controller.h"
#include "cemu_status.h"
#include "audio.h"

#define SOC_CAPCOM_INPUT_T0IN (-1)
#define SOC_CAPCOM_INPUT_T7IN (-2)
#define SOC_PORT_P3 3
#define SOC_PORT_P6 6
#define SOC_PORT_P7 7
#define SOC_PORT_P8 8
#define SOC_PORT_EDGE_INVALID (-2)
#define SOC_PORT_EDGE_FALLING (-1)
#define SOC_PORT_EDGE_NONE 0
#define SOC_PORT_EDGE_RISING 1

#define MAX_IC INTERRUPT_MAX_SOURCES
#define MAX_PERIPHERAL_NAMES 320   /* total reg_names across all peripherals */

typedef struct soc {
    memory_controller_t memory;
    struct soc_peripheral_storage *peripheral_storage;

    unsigned synth_mask;   /* active synthetic behaviors (bit per SYN_* id, see synth.h) */

    /* Boot straps to re-latch on reset. */
    struct { uint32_t addr; uint16_t val; } straps[MAX_STRAPS];
    int nstraps;

    /* Peripheral lifecycle registry. Memory endpoint indexes live in `memory`. */
    peripheral_t *peripherals[MAX_PERIPHERALS];
    int           n_peripherals;
    /* Register-name index (mirror pmb7850._peripheral_names): every addr a
     * registered peripheral NAMES via its reg_names. Distinct from sfr_hook — a
     * word can be hooked-for-behavior without being named. Drives the "modeled"
     * (green) run-summary color and the DPRAM/SFR cell labels. */
    const reg_name_t *periph_names[MAX_PERIPHERAL_NAMES];
    int               n_periph_names;
    /* Typed handles used by cross-peripheral wiring and host controls. */
    peripheral_t *xbus_unknown1_periph;
    peripheral_t *keypad_periph;
    peripheral_t *capcom1_periph;
    peripheral_t *capcom2_periph;
    peripheral_t *ports_periph;
    peripheral_t *twi_gpio_periph;
    peripheral_t *sim_periph;
    peripheral_t *gsm_stub_periph;
    peripheral_t *gsm_legacy_adapter_periph;
    peripheral_t *battery_periph;
    peripheral_t *serial_periph;
    peripheral_t *ssc0_periph;
    peripheral_t *lcd_periph;
    peripheral_t *speaker_periph;

    /* Transient per-instruction GPT2 T6 pulse for CAPCOM counter mode. */
    int capcom_t6_pulse;

    interrupt_subsystem_t interrupts;

    /* ASC0 serial buffers live on the SoC (Python: soc.serial_tx_bytes /
     * serial_rx_bytes); the ASC0 peripheral appends into them. */
    uint8_t *serial_tx;
    size_t   serial_tx_len, serial_tx_cap;
    uint8_t *serial_rx;
    size_t   serial_rx_head, serial_rx_len, serial_rx_cap;
    int      serial_autobaud_bypass;
    device_serial_link_config_t serial_link;
    int      serial_link_attached;

    cemu_status_t runtime_error;

    uint64_t ticks;
    int init_locked;
    int rstout;

    /* One typed event hub shared by CPU, SoC, peripherals, and harnesses. */
    cemu_event_hub_t instrumentation;
    cpu_t *cpu;

    cemu_audio_t audio;

    bus_t bus;   /* vtable handed to the CPU */
} soc_t;

/* Append a byte to the ASC0 transmit buffer (used by the serial peripheral). */
cemu_status_code_t cemu_soc_serial_tx_push(soc_t *s, uint8_t byte);

/* Build a SoC over a loaded flash image + device config. The memory controller
 * allocates backing storage and the SoC composes the default peripheral set.
 * `flash` is borrowed (not copied); it must outlive the SoC. */
/* `synth_mask` selects active diagnostic behaviors (see synth.h). */
cemu_status_t cemu_soc_init(soc_t *s, const uint8_t *flash, size_t flash_len,
                       const device_config_t *cfg, unsigned synth_mask,
                       int serial_autobaud_bypass);
void cemu_soc_free(soc_t *s);

/* Re-seed core CSFR reset values + boot straps (reset / SRST). */
void cemu_soc_reset_core(soc_t *s);

/* Point the SoC at its CPU so trace events can read the live icount/pc. */
void cemu_soc_attach_cpu(soc_t *s, cpu_t *cpu);

/* Emit an `exec` trace event for the instruction just stepped (detail = disasm,
 * addr = pre-exec pc0, pc = post-exec pc, size = fetch_len). No-op if no sink.
 * Called by the driver after each cemu_cpu_step when tracing. */
void cemu_soc_instrument_instruction(soc_t *s, uint32_t pc0, const char *detail, int size);
void cemu_soc_instrument_idle_span(soc_t *s, uint32_t pc, const char *detail,
                              uint64_t first_icount, uint64_t ticks);

/* Emit a serial_tx / serial_rx event (kind, S0TBUF/S0RBUF addr + name, byte,
 * info={"char": <printable|null>}). Called by the ASC0 peripheral; no-op if no
 * sink. is_tx selects the addr/name/kind. */
void cemu_soc_trace_serial(soc_t *s, int is_tx, uint8_t byte);
void cemu_soc_emit_native_trace(soc_t *s, const char *kind, int has_addr, uint32_t addr,
                    int has_size, int size, int has_value, uint32_t value,
                    const char *detail, const cemu_event_fields_t *info);

#if !CEMU_INSTRUMENTED
#define cemu_soc_instrument_instruction(s, pc, detail, size) ((void)0)
#define cemu_soc_instrument_idle_span(s, pc, detail, first, ticks) ((void)0)
#define cemu_soc_trace_serial(s, is_tx, byte) ((void)0)
#endif

/* Queue host->phone serial input (ASC0 RX). */
cemu_status_t cemu_soc_feed_serial(soc_t *s, const uint8_t *data, size_t n);

/* Reflect attachment of the PTY bridge on the device-specific ASC0 RX input.
 * The bridge drives only the idle level, not individual electrical bits. */
void cemu_soc_serial_link_attached(soc_t *s, int attached);

/* Drive an external P3/P6/P7/P8 pin level for tests/debugger/future GPIO glue.
 * Returns SOC_PORT_EDGE_* and forwards P6.0..P6.7 edges to CAPCOM1 capture
 * inputs CC0..CC7. */
int cemu_soc_port_input_level(soc_t *s, int port, int bit, int level);
void cemu_soc_port_input_restore_level(soc_t *s, int port, int bit, int level);
void cemu_soc_port_input_release(soc_t *s, int port, int bit);
int cemu_soc_port_input_edge(soc_t *s, int port, int bit, int rising);

/* Inject one CAPCOM external edge for tests and future GPIO glue. `input_id` is
 * SOC_CAPCOM_INPUT_T0IN / SOC_CAPCOM_INPUT_T7IN, or a channel number 0..31 for
 * CCxIO capture inputs. `rising` selects the edge polarity. */
void cemu_soc_capcom_input_edge(soc_t *s, int input_id, int rising);

/* Read the CAPCOM-owned internal output latch for channel 0..31. */
int cemu_soc_capcom_output_level(soc_t *s, int channel);
int cemu_soc_capcom_output_driven(soc_t *s, int channel);
void cemu_soc_audio_capcom_edge(soc_t *s, int channel, int level);
int cemu_soc_audio_available(const soc_t *s);
void cemu_soc_audio_reset(soc_t *s);

/* Advance peripherals by n instruction-ticks (also reachable via bus.tick). */
void cemu_soc_tick(void *ctx, int n);
/* Return the earliest canonical peripheral event while IDLE. Zero means at
 * least one active timed peripheral cannot prove a quiet interval. */
uint64_t cemu_soc_next_event_ticks(soc_t *s);
/* Advance a previously proven event-free span. Returns 0 on success. */
int cemu_soc_advance_quiet(soc_t *s, uint64_t ticks);
/* Batch a proven event-free IDLE span, stopping before the next event. */
uint64_t cemu_soc_batch_idle(soc_t *s, uint64_t max_ticks);

/* True if an IDLE CPU could still be woken by some interrupt source. */
int cemu_soc_idle_wake_possible(soc_t *s);

/* True if a finite PEC transfer can progress from a pending or autonomously
 * scheduled interrupt source under the attached CPU's current IEN/priority. */
int cemu_soc_finite_pec_will_progress(soc_t *s);

/* Debugger-only forced pending interrupt source. This deliberately does not add
 * the IC word to the normal deliverable table; it is for discriminating probes. */
void cemu_soc_debug_inject_irq(soc_t *s, uint32_t addr, int trap, int ilvl);

/* Identity-unknown XBUS device counters, for reporting only. */
void cemu_soc_xbus_unknown1_counts(soc_t *s, uint64_t *id_reads,
                              uint64_t *status_reads,
                              uint64_t *doorbell_rings);

/* ---- observability (monitor.c / summary.c) ----------------------------- */
/* Turn on the always-on-in-Python access counters. Allocates the count arrays
 * lazily; a bare benchmark never calls this and pays nothing. Must be called
 * before the run loop. Idempotent. */
cemu_status_t cemu_soc_enable_stats(soc_t *s);
void cemu_soc_disable_stats(soc_t *s);

/* True if `addr` is an SFR/ESFR word backed by a real model (a peripheral or a
 * core CSFR), not merely named (mirror _sfr_modeled). */
int cemu_soc_sfr_modeled(soc_t *s, uint32_t addr);

/* Register-name resolution with the peripheral index. */
const char *cemu_soc_sfr_name(soc_t *s, uint32_t addr);
const char *cemu_soc_internal_io_name(soc_t *s, uint32_t addr);
const char *cemu_soc_dpram_name(soc_t *s, uint32_t addr);

/* One armed interrupt-control register (mirror an armed_interrupts() dict). */
typedef struct {
    uint32_t addr;
    const char *name;
    uint16_t value;
    int ir, ie, ilvl, glvl;
    int trap;       /* resolved trap number, or -1 if none */
    int has_trap;
} armed_ic_t;

/* Enumerate armed xIC registers (IE=1, or IR=1 if pending_only), sorted by
 * (ilvl, glvl) descending — arbitration order. Fills up to `cap`; returns the
 * count. Mirrors pmb7850.armed_interrupts(). */
int cemu_soc_armed_interrupts(soc_t *s, int pending_only, armed_ic_t *out, int cap);

/* True if any of the given polled SFR word addrs can still become pending
 * autonomously from the current modeled state (timer overflow, CAPCOM compare,
 * etc.). Mirrors the monitor's awaited-hardware progression check. */
int cemu_soc_awaited_timer_will_fire(soc_t *s, const uint32_t *polled, int n);

/* Describe the handler installed at a trap's vector slot (mirror
 * describe_vector); writes into buf. trap<0 => "n/a". */
void cemu_soc_describe_vector(soc_t *s, int trap, char *buf, int cap);

/* Short trap label (mirror irqmap.trap_name); writes into buf. */
void cemu_soc_trap_name(int trap, char *buf, int cap);

#endif /* CEMU_SOC_H */
