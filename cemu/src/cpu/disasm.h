/* C166S contextual instruction formatter. Byte GPRs use rl0/rh0..rl7/rh7.
 *
 * It re-decodes one instruction from memory using event-free peeks and pre-exec
 * CPU state (segmented PC, the EXTR window for reg naming, GPRs for the DIV0
 * note). Kept entirely separate from exec() so the fast path is untouched: the
 * driver calls it only when a trace sink is attached, before stepping. */
#ifndef CEMU_DISASM_H
#define CEMU_DISASM_H

#include "cpu.h"
#include "soc.h"

#define cpu_disasm cemu_cpu_disasm
#define cpu_disasm_at cemu_cpu_disasm_at

/* Write the mnemonic for the instruction at the CPU's current PC into `buf`.
 * `extr` is the CPU's ESFR-window flag (affects reg-operand naming). Returns the
 * instruction length in bytes (matching cpu.fetch_len after the real step). */
int cpu_disasm(cpu_t *cpu, soc_t *soc, char *buf, int cap);

/* Same, but decode the instruction at an explicit 24-bit `addr` (csp=addr>>16,
 * ip=addr&0xFFFF) instead of the live PC — for the debugger's `disasm ADDR`.
 * Reg-operand naming still uses the CPU's current EXTR window (a static disasm
 * has no per-site window context), matching the standalone Perl disassembler. */
int cpu_disasm_at(cpu_t *cpu, soc_t *soc, uint32_t addr, char *buf, int cap);

#endif /* CEMU_DISASM_H */
