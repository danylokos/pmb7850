/* Synthetic-behavior registry.
 *
 * A small table of named, individually-toggleable *synthetic* behaviors — model
 * additions that are NOT strictly faithful to the hardware but let the boot make
 * forward progress past an unmodeled gap. Each behavior carries its own default
 * (on OR off). The set of active behaviors is a bitmask (`synth_mask`, one bit per
 * id) carried on the SoC; peripherals/decode consult it to decide whether to apply
 * the synthetic action.
 *
 * The DEFAULT mask (`synth_defaults()`) reproduces today's faithful baseline.
 * A run whose mask differs from it is a
 * deliberately synthetic run and the driver flags it with a SYNTHETIC banner. */
#ifndef CEMU_SYNTH_H
#define CEMU_SYNTH_H

#include <stddef.h>
#include <stdint.h>

#define synth_registry cemu_synth_registry
#define synth_id_by_name cemu_synth_id_by_name
#define synth_parse cemu_synth_parse
#define synth_defaults cemu_synth_defaults
#define synth_active_names cemu_synth_active_names

/* Stable behavior ids (bit positions in synth_mask). Keep in sync with
 * synth_registry[] in synth.c. New synthetic behaviors get a new id here + a
 * registry row; nothing else in the framework changes. */
enum {
    SYN_SUPPRESS_EF_MAILBOX_RESPONSE = 0, /* diagnostic: retain pre-mailbox zero response */
    SYN_GSM, /* measured DSP responses plus registration/signal publication */
    SYN_XBUS_MAILBOX_COMPLETE_BIT1, /* diagnostic: add EC12.1 completion */
    SYN_XBUS_MAILBOX_NO_COMPLETE_BIT2, /* diagnostic: suppress EC12.2 completion */
    SYN_XBUS_MAILBOX_NO_IRQ80, /* diagnostic: suppress mailbox IRQ80 */
    SYN_XBUS_MAILBOX_IMMEDIATE, /* diagnostic: old immediate responder */
    SYN_COUNT
};

typedef struct {
    const char *name;   /* CLI token (kebab-case) */
    const char *desc;   /* one-line description for `--synth list` */
    int         dflt;   /* default state: 1 = on, 0 = off */
} synth_behavior_t;

/* The registry (SYN_COUNT entries, indexed by id). */
extern const synth_behavior_t synth_registry[SYN_COUNT];

/* Mask of every behavior's default state — today's faithful baseline. */
unsigned synth_defaults(void);

/* Look up a behavior id by CLI name, or -1 if unknown. */
int synth_id_by_name(const char *name);

/* Apply a comma-separated toggle list over *mask: each token is either `name`
 * (enable) or `no-name` (disable). Returns 0 on success, -1 on an unknown token
 * (writing the offending token into `bad` if non-NULL). `list`/`help` are handled
 * by the caller, not here. */
int synth_parse(unsigned *mask, const char *csv, const char **bad);

/* Write a comma-separated list of the active behaviors' names into buf (for the
 * run banner). Returns the number of active behaviors. */
int synth_active_names(unsigned mask, char *buf, size_t cap);

#endif /* CEMU_SYNTH_H */
