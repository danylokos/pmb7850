/* Embeddable PMB7850/C166S core lifecycle. */
#ifndef CEMU_CORE_H
#define CEMU_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "cemu_status.h"
#include "cemu_core_storage.h"
#include "cemu_event.h"
#include "devices.h"
#include "audio.h"

typedef struct cemu_core cemu_core_t;
typedef void (*cemu_core_after_step_fn)(void *);
typedef void (*cemu_core_instruction_detail_fn)(
    void *, char *, size_t);
typedef void (*cemu_core_audio_output_fn)(void *, uint64_t,
                                          const int16_t *, size_t);
typedef void (*cemu_core_audio_reset_fn)(void *, uint64_t,
                                         cemu_audio_discontinuity_t);

typedef struct {
    const uint8_t *source;
    size_t source_size;
    device_config_t device;
    unsigned synthetic_mask;
    int gsm_stub_enabled;
    int serial_autobaud_bypass;
    int defer_post_reset_storage;
    cemu_storage_initializer_t storage_initializer;
    cemu_storage_result_callback_t storage_result;
    void *storage_opaque;
    cemu_core_audio_output_fn audio_output;
    cemu_core_audio_reset_fn audio_reset;
    void *audio_opaque;
} cemu_core_options_t;

typedef enum {
    CEMU_RUN_SLICE_LIMIT = 0,
    CEMU_RUN_HALTED,
    CEMU_RUN_UNIMPLEMENTED,
    CEMU_RUN_STOPPED,
    CEMU_RUN_ERROR,
} cemu_run_reason_t;

typedef struct {
    cemu_run_reason_t reason;
    uint64_t ticks;
    uint64_t guest_instructions;
    uint64_t total_guest_instructions;
    uint64_t instruction_count;
    uint32_t pc;
} cemu_run_result_t;

typedef struct {
    uint64_t tick_budget;
    int allow_idle_batch;
} cemu_run_request_t;

typedef struct {
    uint64_t ticks;
    uint64_t total_guest_instructions;
    uint64_t instruction_count;
    uint32_t pc;
    int halted;
    int unimplemented;
    uint32_t unimplemented_opcode;
    uint32_t unimplemented_pc;
    int stop_requested;
    int statistics_enabled;
    uint64_t interrupts_delivered;
    uint64_t traps_taken;
    int idle;
    int idle_wake_possible;
    uint64_t xbus_unknown1_id_reads;
    uint64_t xbus_unknown1_status_reads;
    uint64_t xbus_unknown1_doorbell_rings;
    uint64_t interrupt_cache_queries;
    uint64_t interrupt_cache_hits;
    uint64_t interrupt_cache_scans;
    uint64_t interrupt_cache_invalidations;
    cemu_status_t error;
} cemu_core_state_t;

typedef struct {
    unsigned width;
    unsigned height;
    size_t size;
    uint64_t sequence;
    uint64_t instruction_count;
} cemu_frame_info_t;

#define CEMU_FRAME_MAX_RGB_SIZE ((size_t)132u * 162u * 3u)

typedef struct {
    int pressed;
    int sampled;
    uint64_t instruction_count;
} cemu_key_state_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t next_cursor;
} cemu_serial_tx_view_t;

typedef struct {
    int available;
    int attached;
    uint64_t received_bytes;
    size_t transmitted_bytes;
} cemu_serial_link_state_t;

cemu_status_t cemu_core_create(cemu_core_t **out,
                               const cemu_core_options_t *options);
cemu_status_t cemu_core_run_slice(cemu_core_t *core, uint64_t budget,
                                  cemu_run_result_t *result);
cemu_status_t cemu_core_run_request(cemu_core_t *core,
                                    const cemu_run_request_t *request,
                                    cemu_run_result_t *result);
cemu_status_t cemu_core_reset(cemu_core_t *core);
cemu_status_t cemu_core_apply_post_restore_storage(cemu_core_t *core);
cemu_status_t cemu_core_request_stop(cemu_core_t *core);
cemu_status_t cemu_core_query(const cemu_core_t *core,
                              cemu_core_state_t *state);
void cemu_core_destroy(cemu_core_t *core);
cemu_status_t cemu_core_set_after_step(cemu_core_t *,
                                      cemu_core_after_step_fn, void *);
cemu_status_t cemu_core_set_instruction_detail(
    cemu_core_t *, cemu_core_instruction_detail_fn, void *);
cemu_status_t cemu_core_flush_lcd_frame(cemu_core_t *);
cemu_status_t cemu_core_lcd_frame_state(cemu_core_t *, uint64_t *, uint64_t *);

cemu_status_t cemu_core_render_frame(cemu_core_t *core, int raw_ddram,
                                     uint8_t *rgb, size_t capacity,
                                     cemu_frame_info_t *info);
cemu_status_t cemu_core_render_capture_frame(
    cemu_core_t *, int raw_ddram, unsigned scale,
    uint8_t *, size_t, cemu_frame_info_t *);
cemu_status_t cemu_core_set_key(cemu_core_t *core, const char *name,
                                int pressed);
cemu_status_t cemu_core_key_count(cemu_core_t *core, size_t *count);
cemu_status_t cemu_core_key_name(cemu_core_t *core, size_t index,
                                 const char **name);
cemu_status_t cemu_core_query_key(cemu_core_t *core, const char *name,
                                  cemu_key_state_t *state);
cemu_status_t cemu_core_feed_serial(cemu_core_t *core, const uint8_t *data,
                                    size_t size);
cemu_status_t cemu_core_serial_tx_view(cemu_core_t *core, size_t cursor,
                                       cemu_serial_tx_view_t *view);
cemu_status_t cemu_core_set_serial_link(cemu_core_t *core, int attached,
                                        cemu_serial_link_state_t *state);
cemu_status_t cemu_core_set_sim_attached(cemu_core_t *core, int attached);
cemu_status_t cemu_core_set_battery(cemu_core_t *core, unsigned level,
                                    int charging);
cemu_status_t cemu_core_enable_statistics(cemu_core_t *core);
cemu_status_t cemu_core_subscribe_events(
    cemu_core_t *core, cemu_event_mask_t mask,
    cemu_event_consumer_fn consumer, cemu_event_filter_fn filter,
    void *opaque, unsigned *subscription);
cemu_status_t cemu_core_unsubscribe_events(cemu_core_t *core,
                                           unsigned subscription);

#endif /* CEMU_CORE_H */
