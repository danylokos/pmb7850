#ifndef CEMU_STATE_DIGEST_H
#define CEMU_STATE_DIGEST_H

#include <stdint.h>
#include "cpu.h"
#include "soc.h"

#define state_digest cemu_state_digest

/* Deterministic machine-state fingerprint used for cross-build parity checks. */
uint64_t state_digest(const cpu_t *cpu, const soc_t *soc);

#endif
