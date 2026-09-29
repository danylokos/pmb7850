/* Register-name tables + resolvers — see names.h.
 *
 * These tables hold ONLY the two name sources that don't belong to a peripheral:
 * the core CPU CSFRs (cpu/sfr.py) and the residual UNMODELED_* names (the
 * registers no modeled peripheral owns yet — pmb7850.UNMODELED_SFR_NAMES /
 * UNMODELED_DPRAM_NAMES). A register a modeled peripheral owns is NAMED by that
 * peripheral (its reg_names), and the SoC resolves the full order core >
 * peripheral > unmodeled in cemu_soc_sfr_name / cemu_soc_dpram_name — mirroring Python's
 * _sfr_name / _dpram_name. So as a peripheral gets modeled, its names move out
 * of the residual tables here and into the peripheral, exactly as in Python. */
#include <stddef.h>
#include "names.h"

typedef struct { uint32_t addr; const char *name; } name_entry_t;

/* Core CPU CSFR names (cpu/sfr.py SFR + ESFR). The first resolution rung and
 * the disassembler's _regname source. */
static const name_entry_t CORE_SFR_NAMES[] = {
    {0xF00C, "CPUID"},
    {0xFE00, "DPP0"},
    {0xFE02, "DPP1"},
    {0xFE04, "DPP2"},
    {0xFE06, "DPP3"},
    {0xFE08, "CSP"},
    {0xFE0C, "MDH"},
    {0xFE0E, "MDL"},
    {0xFE10, "CP"},
    {0xFE12, "SP"},
    {0xFE14, "STKOV"},
    {0xFE16, "STKUN"},
    {0xFF0E, "MDC"},
    {0xFF10, "PSW"},
    {0xFF12, "SYSCON"},
    {0xFF1C, "ZEROS"},
    {0xFF1E, "ONES"},
    {0xFFAC, "TFR"},
};

/* Residual UNMODELED SFR/ESFR names (pmb7850.UNMODELED_SFR_NAMES): registers
 * that are only named, with no modeled behavior — they color cyan in the run
 * summary. The last rung of cemu_soc_sfr_name. */
static const name_entry_t UNMODELED_SFR_NAMES[] = {
    {0xF014, "XADRS1"}, {0xF016, "XADRS2"}, {0xF018, "XADRS3"}, {0xF01A, "XADRS4"},
    {0xF01C, "XADRS5"}, {0xF01E, "XADRS6"}, {0xF024, "XPERCON"},
    {0xF032, "PT1"}, {0xF034, "PT2"}, {0xF038, "PP0"}, {0xF03A, "PP1"},
    {0xF0C0, "IRQ100IC"}, {0xF0C2, "IRQ101IC"}, {0xF0C6, "IRQ103IC"}, {0xF0C8, "IRQ104IC"},
    {0xF114, "XBCON1"}, {0xF116, "XBCON2"}, {0xF118, "XBCON3"}, {0xF11A, "XBCON4"},
    {0xF11C, "XBCON5"}, {0xF11E, "XBCON6"},
    {0xF120, "IRQ64IC"}, {0xF122, "IRQ65IC"}, {0xF124, "IRQ66IC"}, {0xF126, "IRQ67IC"},
    {0xF128, "IRQ68IC"}, {0xF12A, "IRQ69IC"}, {0xF12C, "IRQ70IC"}, {0xF12E, "IRQ71IC"},
    {0xF130, "IRQ72IC"}, {0xF132, "IRQ73IC"}, {0xF134, "IRQ74IC"}, {0xF136, "IRQ75IC"},
    {0xF13C, "IRQ78IC"}, {0xF13E, "IRQ79IC"}, {0xF140, "IRQ80IC"},
    {0xF190, "IRQ45IC"}, {0xF196, "XP2IC"},
    {0xF1C0, "EXICON"}, {0xF1D4, "SYSCON3"}, {0xF1DC, "SYSCON1"},
    {0xFE18, "ADDRSEL1"}, {0xFE1A, "ADDRSEL2"}, {0xFE1C, "ADDRSEL3"}, {0xFE1E, "ADDRSEL4"},
    {0xFEB6, "FDV"},
    {0xFF0C, "BUSCON0"}, {0xFF14, "BUSCON1"}, {0xFF16, "BUSCON2"}, {0xFF18, "BUSCON3"},
    {0xFF1A, "BUSCON4"},
    {0xFF98, "ADCIC"}, {0xFF9A, "ADEIC"},
    {0xFFAE, "WDTCON"},
    {0xFFC0, "P2"}, {0xFFC2, "DP2"}, {0xFFC4, "P3"}, {0xFFC6, "DP3"},
    {0xFFC8, "P4"}, {0xFFCA, "DP4"}, {0xFFCC, "P6"}, {0xFFCE, "DP6"},
    {0xFFD0, "P7"}, {0xFFD2, "DP7"}, {0xFFD4, "P8"}, {0xFFD6, "DP8"},
    {0xFFEA, "(RSVD)"}, {0xFFEC, "(RSVD)"}, {0xFFEE, "(RSVD)"}, {0xFFF0, "(RSVD)"},
    {0xFFF2, "(RSVD)"}, {0xFFF4, "(RSVD)"}, {0xFFF6, "(RSVD)"}, {0xFFF8, "(RSVD)"},
    {0xFFFA, "(RSVD)"}, {0xFFFC, "(RSVD)"}, {0xFFFE, "(RSVD)"},
};

/* Residual UNMODELED DPRAM cell names. PEC pointers moved into the PEC
 * peripheral when the controller model was added; keep a sentinel so the generic
 * lookup helper still has a concrete table. */
static const name_entry_t UNMODELED_DPRAM_NAMES[] = {
    {0xFFFFFFFFu, NULL},
};

static const char *lookup(const name_entry_t *t, int n, uint32_t addr) {
    /* tables are small and addr-sorted; linear scan is fine (off the hot path). */
    for (int i = 0; i < n; i++) if (t[i].addr == addr) return t[i].name;
    return NULL;
}

#define NELEM(t) ((int)(sizeof(t) / sizeof((t)[0])))

const char *cemu_core_sfr_name(uint32_t addr) {
    return lookup(CORE_SFR_NAMES, NELEM(CORE_SFR_NAMES), addr);
}
const char *cemu_unmodeled_sfr_name(uint32_t addr) {
    return lookup(UNMODELED_SFR_NAMES, NELEM(UNMODELED_SFR_NAMES), addr);
}
const char *cemu_unmodeled_dpram_name(uint32_t addr) {
    return lookup(UNMODELED_DPRAM_NAMES, NELEM(UNMODELED_DPRAM_NAMES), addr & 0xFFFFFF);
}
