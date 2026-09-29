#ifndef CEMU_PERIPH_SIM_H
#define CEMU_PERIPH_SIM_H

#include <stdint.h>
#include "peripheral.h"

#define SIM_REG_FIRST 0xEF50u
#define SIM_REG_LAST  0xEF6Cu
#define SIM_CTRL      0xEF50u
#define SIM_RATE      0xEF52u
#define SIM_STATUS    0xEF54u
#define SIM_CONFIG    0xEF56u
#define SIM_TX        0xEF58u
#define SIM_RX        0xEF5Au
#define SIM_FRAME     0xEF5Cu
#define SIM_GUARD     0xEF5Eu

#define SIM_BYTE_IC   0xF184u
#define SIM_STATUS_IC 0xF18Cu
#define SIM_EVENT_IC  0xF194u

typedef enum {
    SIM_MODE_NONE = 0,
    SIM_MODE_STUB = 1,
} sim_mode_t;

typedef enum {
    SIM_PHASE_OFF = 0,
    SIM_PHASE_INITIAL,
    SIM_PHASE_ATR,
    SIM_PHASE_READY,
} sim_phase_t;

typedef enum {
    SIM_FILE_DIRECTORY = -1,
    SIM_FILE_TRANSPARENT = 0,
    SIM_FILE_LINEAR_FIXED = 1,
    SIM_FILE_CYCLIC = 3,
} sim_file_structure_t;

typedef struct {
    uint16_t parent_df;
    uint16_t id;
    sim_file_structure_t structure;
    uint8_t *data;
    uint16_t len;
    uint8_t record_len;
} sim_file_t;

#define SIM_QUEUE_CAP 512
#define SIM_APDU_CAP  260
#define SIM_PROFILE_FILE_CAP 20

typedef struct {
    sim_mode_t mode;
    int restored;
    sim_phase_t phase;
    uint64_t due_tick;
    uint64_t byte_seq;
    uint8_t output[SIM_QUEUE_CAP];
    uint8_t output_requires_rx[SIM_QUEUE_CAP];
    uint8_t output_internal[SIM_QUEUE_CAP];
    uint16_t output_head, output_len;
    uint8_t input[SIM_APDU_CAP];
    uint16_t input_len, input_need;
    uint8_t input_kind;
    uint16_t selected_df, selected_ef;
    uint8_t response[32], response_len;
    sim_file_t files[SIM_PROFILE_FILE_CAP];
    uint8_t file_count;
    uint8_t imsi[9], iccid[10];
    uint8_t lp[1], phase_ef[1], sst[2], ad[4], kc[9], hpplmn[1];
    uint8_t plmn[24], loci[11], bcch[16], acc[2], fplmn[12];
    uint8_t cphs_info[3], operator_name[16];
    uint8_t tx_waiting, completion_pending, rx_loaded, initial_raised;
    uint16_t completion_sw, last_ctrl;
} sim_state_t;

void cemu_sim_periph_init(peripheral_t *p, sim_state_t *st);
void cemu_sim_set_mode(peripheral_t *p, soc_t *s, sim_mode_t mode);
sim_mode_t cemu_sim_get_mode(const peripheral_t *p);
const char *cemu_sim_mode_name(sim_mode_t mode);

#endif /* CEMU_PERIPH_SIM_H */
