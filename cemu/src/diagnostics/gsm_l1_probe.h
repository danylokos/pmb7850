/* Exact-firmware, read-only observability for the open GSM/L1 investigation.
 *
 * This is deliberately a harness facility, not a device/baseband profile: it
 * names guest consumers in order to trace them, but never writes guest state
 * and cannot supply radio behavior. */
#ifndef CEMU_GSM_L1_PROBE_H
#define CEMU_GSM_L1_PROBE_H

#include <stddef.h>
#include "cpu.h"
#include "devices.h"
#include "soc.h"

#define gsm_l1_probe_attach cemu_gsm_l1_probe_attach
#define gsm_l1_probe_detach cemu_gsm_l1_probe_detach

typedef struct {
    const char *model;
    int software_version;
} cemu_diagnostic_image_t;

typedef struct gsm_l1_probe {
    soc_t *soc;
    cpu_t *cpu;
    const void *profile;
    unsigned subscription;
    unsigned dequeue_pending : 1;
    unsigned receiver_message_active : 1;
} gsm_l1_probe_t;

/* Resolve model/SW metadata and instruction signatures, then subscribe to
 * instrumentation. Returns 0 when attached, 1 when no profile applies, and
 * -1 on subscription failure. A return of 1 may carry a signature-mismatch
 * diagnostic when model/SW metadata matched. */
int gsm_l1_probe_attach(gsm_l1_probe_t *probe, soc_t *soc, cpu_t *cpu,
                        const device_config_t *cfg,
                        const cemu_diagnostic_image_t *image,
                        char *error, size_t error_cap);
void gsm_l1_probe_detach(gsm_l1_probe_t *probe);

#endif
