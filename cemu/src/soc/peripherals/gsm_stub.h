#ifndef CEMU_PERIPH_GSM_STUB_H
#define CEMU_PERIPH_GSM_STUB_H

#include <stdint.h>
#include "peripheral.h"
#include "devices.h"
#include "virtual_cell.h"

#define GSM_DSP_COMMAND_MIRROR 0xE0DCu
#define GSM_DSP_COMMAND        0xE17Cu
#define GSM_IRQ45_IC           0xF190u
#define GSM_XP2_IC             0xF196u
#define BASEBAND_RESULT_WORD0  0xEF2Cu
#define BASEBAND_RESULT_WORD1  0xEF2Eu

typedef enum {
    BASEBAND_RESULT_NONE = 0,
    BASEBAND_RESULT_GSM,
    BASEBAND_RESULT_ADC,
} baseband_result_source_t;

typedef struct {
    int enabled;
    int restored;
    int pending;
    uint16_t command;
    uint16_t pending_command;
    uint64_t due_tick;
    uint64_t request_sequence;
    uint64_t pending_sequence;
    uint32_t response_carrier_addr;
    uint32_t response_flags_addr;
    uint64_t synchronized_responses;
    uint64_t result_sequence;
    baseband_result_source_t last_result_source;
    uint16_t last_result_command;
    uint16_t last_result_word0;
    uint16_t last_result_word1;
    virtual_cell_state_t cell;
} gsm_stub_state_t;

void cemu_gsm_stub_periph_init(peripheral_t *p, gsm_stub_state_t *st,
                          const device_baseband_profile_t *profile);
void cemu_gsm_stub_set_enabled(peripheral_t *p, soc_t *s, int enabled);
int cemu_gsm_stub_available(const peripheral_t *p);
uint64_t cemu_baseband_result_publish(peripheral_t *p, soc_t *s,
                                 baseband_result_source_t source,
                                 uint16_t command,
                                 uint16_t word0, uint16_t word1);

#endif /* CEMU_PERIPH_GSM_STUB_H */
