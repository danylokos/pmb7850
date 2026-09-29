/* Register-name resolvers — see names.c.
 *
 * These expose ONLY the peripheral-independent name sources: the core CPU CSFRs
 * and the residual UNMODELED_* names. The full resolution order (core >
 * peripheral > unmodeled), which needs the SoC's peripheral name index, lives in
 * cemu_soc_sfr_name / cemu_soc_dpram_name (mirror pmb7850._sfr_name / _dpram_name). Use
 * those for trace `detail` / summary labels; use cemu_core_sfr_name for the
 * disassembler's _regname (core CSFRs only). */
#ifndef CEMU_NAMES_H
#define CEMU_NAMES_H

#include <stdint.h>

/* Core-CSFR name for a word address, or NULL (mirrors cpu/sfr.name_for). The
 * disassembler's _regname uses THIS, so it names only DPPx/CP/SP/etc. and falls
 * back to the bare address for peripheral/unmodeled SFRs. */
const char *cemu_core_sfr_name(uint32_t addr);

/* Residual UNMODELED SFR/ESFR name, or NULL (pmb7850.UNMODELED_SFR_NAMES) —
 * the last resolution rung, named-but-unmodeled. */
const char *cemu_unmodeled_sfr_name(uint32_t addr);

/* Residual UNMODELED DPRAM cell name, or NULL (pmb7850.UNMODELED_DPRAM_NAMES). */
const char *cemu_unmodeled_dpram_name(uint32_t addr);

#endif /* CEMU_NAMES_H */
