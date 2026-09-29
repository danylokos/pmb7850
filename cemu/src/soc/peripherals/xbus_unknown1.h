/* Identity-unknown device reached through XBUS windows 3 and 4.
 *
 * The observed surface answers an ID probe, completes E806 work after an EF3A
 * launch, and advances the EC12/EC16 mailbox handshake. These behaviors do not
 * identify the physical block, so the model deliberately uses a topology-only
 * name. */
#ifndef CEMU_PERIPH_XBUS_UNKNOWN1_H
#define CEMU_PERIPH_XBUS_UNKNOWN1_H

#include <stdint.h>
#include "peripheral.h"
#include "xbus_audio.h"

typedef enum {
    XBUS_MAILBOX_IDLE = 0,
    XBUS_MAILBOX_GRACE = 1,
    XBUS_MAILBOX_SYNC = 2,
} xbus_mailbox_phase_t;

typedef struct {
    uint16_t id;
    uint64_t id_reads, status_reads, doorbell_rings;
    uint64_t transaction_seq;
    uint64_t active_transaction_id;
    xbus_mailbox_phase_t phase;
    uint16_t control; /* EC10 sampled at acceptance, not the live register. */
    uint64_t deadline;
    addr_range_t ranges[5];
    xbus_audio_state_t audio;
} xbus_unknown1_state_t;

void cemu_xbus_unknown1_periph_init(peripheral_t *p, xbus_unknown1_state_t *st,
                                    uint16_t id, int audio_enabled);
void cemu_xbus_unknown1_audio_reset(peripheral_t *p, soc_t *soc);

#endif /* CEMU_PERIPH_XBUS_UNKNOWN1_H */
