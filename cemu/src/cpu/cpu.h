/* C166S CPU core — C port of emu/src/cpu/c166.py.
 *
 * A correctness-first interpreter for the Infineon C166S instruction set.
 * GPRs/PSW/SP/CP/DPP are memory-mapped (in SFR/register-bank space reached
 * through the bus), so the CPU object itself holds only the segmented PC, the
 * EXT/ATOMIC override window, run flags, and always-on progress counters — the
 * same split as the Python class. Encoding/semantics follow M166 and the C166S
 * V1 manual; see c166.py for the per-opcode manual citations. */
#ifndef CEMU_CPU_H
#define CEMU_CPU_H

#include <stdint.h>
#include "bus.h"
#include "interrupt.h"
#include "cemu_event.h"

/* PSW ALU condition-flag masks (C166S User's Manual §3.7.6). */
enum {
    F_N = 1u << 0,
    F_C = 1u << 1,
    F_V = 1u << 2,
    F_Z = 1u << 3,
    F_E = 1u << 4,
};

#define TFR_ADDR   0xFFACu
#define TFR_UNDOPC (1u << 7)
#define BTRAP_VEC  0x0Au

/* Result of a single step (mirrors how boot.py classifies a run). OK means the
 * CPU is still runnable; the others are terminal for the run loop. */
typedef enum {
    STEP_OK = 0,
    STEP_UNIMPL,   /* valid-but-unimplemented opcode hit (would raise in Python) */
} step_result_t;

/* EXT override window kind. */
typedef enum { EXT_NONE = 0, EXT_SEG, EXT_PAGE } ext_kind_t;

typedef struct cpu {
    bus_t *bus;
    interrupt_port_t *interrupts;

    /* CSP:IP form the 24-bit PC. */
    uint8_t  csp;
    uint16_t ip;

    /* EXT override window: kind + value + remaining instruction count. extr is
     * the ESFR-addressing window (EXTR / EXT?R). An ATOMIC window is ext_kind
     * NONE + extr 0 + ext_count>0. */
    ext_kind_t ext_kind;
    uint16_t   ext_val;      /* seg (8b) or page (10b) */
    int        ext_count;
    int        extr;

    int halted;
    int idle;

    uint64_t icount;

    /* Always-on progress counters (tracing-independent), for snapshot parity. */
    uint64_t interrupts_delivered;
    uint64_t traps_taken;

    /* Instrumentation-owned summary counters. */
    uint64_t trap_counts[256];
    uint64_t protected_ops[6];

    /* Bytes fetched for the current instruction (opcode + operands). */
    int fetch_len;

    /* 1 if the last cemu_cpu_step fetched+executed an instruction, 0 for an idle/
     * halted spin. Lets a tracing driver emit an `exec` event only for real
     * instructions (mirrors Python, where idle steps log nothing). */
    int last_ran;

    /* Set when _exec hits an unimplemented opcode; carries the fault detail. */
    int      unimpl;
    uint32_t unimpl_op;
    uint32_t unimpl_pc;

    cemu_event_hub_t *instrumentation;
} cpu_t;

/* protected_ops[] slot indices (order matches PROTECTED_OP_NAMES in cpu.c). */
enum { POP_DISWDT, POP_EINIT, POP_IDLE, POP_PWRDN, POP_SRST, POP_SRVWDT };

void cemu_cpu_init(cpu_t *cpu, bus_t *bus);
void cemu_cpu_attach_interrupt_port(cpu_t *cpu, interrupt_port_t *interrupts);
void cemu_cpu_reset(cpu_t *cpu);

/* Mnemonic for a protected_ops[] slot (diswdt/einit/...). */
const char *cemu_cpu_protected_op_name(int slot);

/* Execute one instruction (or one idle spin). Returns STEP_UNIMPL if an
 * unimplemented opcode was encountered (cpu->unimpl_* describe it). */
step_result_t cemu_cpu_step(cpu_t *cpu);

/* 24-bit program counter. */
static inline uint32_t cpu_pc(const cpu_t *cpu) {
    return ((uint32_t)(cpu->csp & 0xFF) << 16) | (cpu->ip & 0xFFFF);
}

/* GPR access (memory-mapped at CP + 2n). Exposed for tests/driver. */
uint16_t cemu_cpu_gpr(cpu_t *cpu, int n);
void     cemu_cpu_set_gpr(cpu_t *cpu, int n, uint16_t val);
uint16_t cemu_cpu_psw(cpu_t *cpu);
void     cemu_cpu_set_psw(cpu_t *cpu, uint16_t v);

#endif /* CEMU_CPU_H */
