#ifndef EMU_ENGINE_H
#define EMU_ENGINE_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#define EMU_SHA256_SIZE 32u
#define EMU_SHA256_HEX_SIZE 65u
#define EMU_MAX_CHIPS 4u
#define EMU_MAX_STORAGE_OPERATIONS 64u
#define EMU_MAX_ARTIFACTS 32u
#define EMU_MAX_EVENT_FIELDS 8u

typedef struct emu_trace_sink emu_trace_sink_t;
typedef struct emu_trace_event emu_trace_event_t;
typedef struct emu_drcov emu_drcov_t;

/* Read returns bytes, 0 for EOF, -1 for error, -2 for interrupted/would-block. */
typedef ptrdiff_t (*emu_text_read_fn)(void *, char *, size_t);
typedef int (*emu_text_write_fn)(void *, const char *, size_t);

typedef struct {
    void *opaque;
    emu_text_read_fn read;
    emu_text_write_fn write;
} emu_text_io_t;

typedef struct {
    const char *script;
    const char *command;
    int interactive;
    int default_interactive;
    int monitor;
    int batch_idle;
    emu_text_io_t input;
    emu_text_io_t output;
    /* Optional synchronous cooperation: phase 0 paused, 1 executing, 2 restored.
     * Cumulative execution counters survive restore. Return 0 to proceed,
     * 1 to interrupt the command, 2 to end the session, -1 on service failure. */
    int (*service)(void *, int phase, uint64_t ticks, uint64_t guest);
    void *service_opaque;
    /* Optional readiness poll, in milliseconds: 1 ready, 0 retry, -1 error.
     * With this hook input reads must return promptly when ready. */
    int (*input_wait)(void *, int timeout_ms);
} emu_debugger_request_t;

typedef struct {
    char status[32];
    char reason[704];
} emu_monitor_verdict_t;

typedef struct {
    const char *status;
    const char *reason;
    uint64_t steps;
    uint32_t pc;
    double elapsed_seconds;
    int serial_already_printed;
    int color;
    int show_writes;
} emu_summary_request_t;

typedef enum {
    EMU_OK = 0,
    EMU_ERR_ARGUMENT,
    EMU_ERR_NOMEM,
    EMU_ERR_IO,
    EMU_ERR_HASH_MISMATCH,
    EMU_ERR_INVALID_PREPARED_SESSION,
    EMU_ERR_UNKNOWN_ENGINE,
    EMU_ERR_DUPLICATE_ENGINE,
    EMU_ERR_UNSUPPORTED,
    EMU_ERR_LIFECYCLE,
    EMU_ERR_ENGINE,
} emu_error_code_t;

typedef struct {
    emu_error_code_t code;
    char message[256];
} emu_error_t;

typedef enum {
    EMU_CAP_KEYS          = UINT64_C(1) << 0,
    EMU_CAP_SERIAL        = UINT64_C(1) << 1,
    EMU_CAP_SIM           = UINT64_C(1) << 2,
    EMU_CAP_BATTERY       = UINT64_C(1) << 3,
    EMU_CAP_STORAGE_INIT  = UINT64_C(1) << 4,
    EMU_CAP_SNAPSHOTS     = UINT64_C(1) << 5,
    EMU_CAP_DEBUGGER      = UINT64_C(1) << 6,
    EMU_CAP_COVERAGE      = UINT64_C(1) << 7,
    EMU_CAP_FRAMES        = UINT64_C(1) << 8,
    EMU_CAP_EVENTS        = UINT64_C(1) << 9,
    EMU_CAP_KEY_SAMPLING  = UINT64_C(1) << 10,
    EMU_CAP_SERIAL_LINK   = UINT64_C(1) << 11,
    EMU_CAP_ARTIFACT_REQ  = UINT64_C(1) << 12,
    EMU_CAP_AUDIO         = UINT64_C(1) << 13,
} emu_capability_t;

typedef enum {
    EMU_DIAG_MONITOR = UINT64_C(1) << 0,
    EMU_DIAG_ARTIFACTS = UINT64_C(1) << 1,
    EMU_DIAG_DEBUGGER = UINT64_C(1) << 2,
    EMU_DIAG_SNAPSHOT = UINT64_C(1) << 3,
    EMU_DIAG_COVERAGE = UINT64_C(1) << 4,
    EMU_DIAG_RAW_LCD = UINT64_C(1) << 5,
    EMU_DIAG_SNAPSHOT_RESTORE = UINT64_C(1) << 6,
    EMU_DIAG_MANAGED_GDB = UINT64_C(1) << 7,
} emu_diagnostic_capability_t;

typedef int (*emu_synthetic_parse_fn)(unsigned *, const char *, const char **);
typedef void (*emu_synthetic_print_fn)(FILE *, unsigned);
typedef int (*emu_synthetic_names_fn)(unsigned, char *, size_t);

typedef struct {
    const char *engine_name;
    const char *profile_name;
    uint64_t capabilities;
    int trace_available;
    uint64_t trace_mask;
    unsigned synthetic_defaults;
    emu_synthetic_parse_fn synthetic_parse;
    emu_synthetic_print_fn synthetic_print;
    emu_synthetic_names_fn synthetic_names;
} emu_engine_diagnostics_t;

typedef enum {
    EMU_STORAGE_MAIN_ARRAY = 0,
    EMU_STORAGE_FACTORY_UID,
    EMU_STORAGE_AM29_FACTORY_SECSI,
    EMU_STORAGE_AM29_CUSTOMER_SECSI,
} emu_storage_space_t;

typedef enum {
    EMU_STORAGE_STAGE_PRE_RESET = 0,
    EMU_STORAGE_STAGE_POST_RESTORE,
} emu_storage_stage_t;

typedef enum {
    EMU_IDENTITY_NONE = 0,
    EMU_IDENTITY_FACTORY_UID,
    EMU_IDENTITY_AM29_SECSI,
} emu_identity_kind_t;

typedef struct {
    char *locator;
    uint8_t *bytes;
    size_t size;
    uint8_t sha256[EMU_SHA256_SIZE];
    char sha256_hex[EMU_SHA256_HEX_SIZE];
} emu_source_t;

typedef struct {
    char model[16];
    char langpack[16];
    uint8_t software_version_raw;
    int software_version;
    uint8_t bcore_software_version_raw;
    int bcore_software_version;
    uint16_t flash_manufacturer_id;
    uint16_t flash_device_id;
    char flash_vendor[16];
    char flash_engine[32];
    char flash_classification[16];
    size_t metadata_view_offset;
    size_t metadata_instance_count;
    char flash_file_order[24];
} emu_image_metadata_t;

typedef struct {
    char role[32];
    char model[32];
    size_t source_offset;
    size_t size;
    uint8_t sha256[EMU_SHA256_SIZE];
    char sha256_hex[EMU_SHA256_HEX_SIZE];
} emu_chip_view_t;

typedef struct {
    size_t order;
    emu_storage_stage_t stage;
    size_t group;
    size_t chip_index;
    emu_storage_space_t space;
    size_t offset;
    uint8_t *expected;
    size_t expected_size;
    uint8_t *replacement;
    size_t replacement_size;
    char provenance[128];
} emu_storage_operation_t;

/* Startup actions borrow argv strings for the lifetime of the invocation. */
#define EMU_GDB_MAX_ACTIONS 128
typedef struct {
    int script;
    const char *value;
} emu_gdb_action_t;

typedef struct {
    uint64_t requested_slice_ticks;
    uint64_t firmware_patches;
    unsigned synthetic_mask;
    int serial_autobaud_bypass;
    int defer_post_restore_storage;
    int sim_stub;
    uint64_t trace_mask;
    unsigned gdb_port;
    int gdb_enabled;
    int gdb_mode; /* 0: external, 1: managed batch, 2: managed interactive */
    char gdb_socket[108];
    char gdb_binary[512];
    size_t gdb_action_count;
    emu_gdb_action_t gdb_actions[EMU_GDB_MAX_ACTIONS];
    char qemu_binary[512];
    const char *snapshot_path; /* Borrowed startup path; NULL on fresh boots. */
    int snapshot_requested;
} emu_runtime_options_t;

typedef struct {
    emu_source_t source;
    emu_image_metadata_t metadata;
    char selected_device[32];
    emu_identity_kind_t identity_kind;
    size_t identity_chip_index;
    emu_chip_view_t chips[EMU_MAX_CHIPS];
    size_t chip_count;
    emu_storage_operation_t operations[EMU_MAX_STORAGE_OPERATIONS];
    size_t operation_count;
    emu_runtime_options_t options;
} emu_prepared_session_t;

typedef enum {
    EMU_RUN_LIMIT = 0,
    EMU_RUN_STOPPED,
    EMU_RUN_HALTED,
    EMU_RUN_UNIMPLEMENTED,
} emu_run_status_t;

typedef struct {
    emu_run_status_t status;
    uint64_t ticks;
    uint64_t guest_instructions;
    uint64_t end_icount;
    uint32_t pc;
    uint64_t digest;
    uint32_t unimplemented_opcode;
    uint32_t unimplemented_pc;
} emu_run_result_t;

typedef struct {
    uint64_t tick_budget;
    int allow_idle_batch;
    int compute_digest;
} emu_run_request_t;

typedef struct {
    /* CLOCK_MONOTONIC acquisition time; zero for directly queried engines. */
    uint64_t measured_ns;
    uint64_t ticks;
    uint64_t guest_instructions;
    uint64_t icount;
    uint32_t pc;
    uint64_t digest;
    int stop_requested;
    int halted;
    int idle;
    int idle_wake_possible;
    int audio_available;
    uint32_t unimplemented_opcode;
    uint32_t unimplemented_pc;
    uint64_t interrupts_delivered;
    uint64_t traps_taken;
    uint64_t xbus_unknown1_id_reads;
    uint64_t xbus_unknown1_status_reads;
    uint64_t xbus_unknown1_doorbell_rings;
    uint64_t interrupt_cache_queries;
    uint64_t interrupt_cache_hits;
    uint64_t interrupt_cache_scans;
    uint64_t interrupt_cache_invalidations;
} emu_engine_state_t;

typedef enum {
    EMU_EVENT_SLICE = 0,
    EMU_EVENT_RESET,
    EMU_EVENT_POLL,
    EMU_EVENT_KEY,
    EMU_EVENT_SERIAL,
    EMU_EVENT_LINK,
    EMU_EVENT_ARTIFACT,
    EMU_EVENT_KIND_COUNT,
} emu_event_kind_t;

#define EMU_EVENT_MASK(kind) (UINT64_C(1) << (unsigned)(kind))
#define EMU_EVENT_MASK_ALL ((UINT64_C(1) << EMU_EVENT_KIND_COUNT) - 1u)

typedef enum {
    EMU_EVENT_VALUE_U64 = 0,
    EMU_EVENT_VALUE_I64,
    EMU_EVENT_VALUE_BOOL,
    EMU_EVENT_VALUE_STRING,
} emu_event_value_kind_t;

typedef struct {
    const char *name;
    emu_event_value_kind_t kind;
    union {
        uint64_t u64;
        int64_t i64;
        int boolean;
        const char *string;
    } value;
} emu_event_field_t;

typedef struct {
    uint64_t sequence;
    uint64_t tick;
    uint64_t icount;
    uint32_t pc;
    emu_event_kind_t kind;
    emu_event_field_t fields[EMU_MAX_EVENT_FIELDS];
    size_t field_count;
} emu_event_t;

typedef struct {
    uint64_t kind_mask;
} emu_event_selection_t;

typedef enum {
    EMU_FRAME_DISPLAY = 0,
    EMU_FRAME_DDRAM,
} emu_frame_kind_t;

typedef struct {
    emu_frame_kind_t kind;
    uint64_t sequence;
    uint64_t icount;
    unsigned width;
    unsigned height;
    const uint8_t *rgb;
    size_t rgb_size;
} emu_frame_t;

/* D-Bus playback callbacks have no guest timestamp; never guess from stats. */
#define EMU_AUDIO_TIME_UNKNOWN UINT64_MAX

typedef struct {
    uint64_t completion_tick; /* Or EMU_AUDIO_TIME_UNKNOWN. */
    const int16_t *samples;
    size_t frame_count;
} emu_audio_output_t;

typedef enum {
    EMU_AUDIO_RESET_ENGINE = 0,
    EMU_AUDIO_RESET_SNAPSHOT,
    EMU_AUDIO_RESET_STOP,
    EMU_AUDIO_RESET_SOURCE_CANCELLED,
    EMU_AUDIO_RESET_SOURCE_OVERFLOW,
    EMU_AUDIO_RESET_BACKEND,
} emu_audio_reset_reason_t;

typedef struct {
    emu_audio_reset_reason_t reason;
    uint64_t icount; /* Or EMU_AUDIO_TIME_UNKNOWN for backend discontinuity. */
} emu_audio_reset_t;

typedef struct {
    int requested_pressed;
    int sampled_pressed;
    uint64_t icount;
} emu_key_sample_t;

typedef enum {
    EMU_SERIAL_LINK_DETACHED = 0,
    EMU_SERIAL_LINK_HOST,
    EMU_SERIAL_LINK_UI,
} emu_serial_link_attachment_t;

typedef struct {
    emu_serial_link_attachment_t attachment;
    int available;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
} emu_serial_link_state_t;

typedef enum {
    EMU_ARTIFACT_LCD_FRAME = 0,
    EMU_ARTIFACT_LCD_DDRAM,
    EMU_ARTIFACT_SNAPSHOT,
    EMU_ARTIFACT_COVERAGE,
    EMU_ARTIFACT_TRACE,
} emu_artifact_request_kind_t;

typedef enum {
    EMU_ARTIFACT_CAPTURE = 0,
    EMU_ARTIFACT_START,
    EMU_ARTIFACT_STOP,
} emu_artifact_action_t;

typedef struct {
    emu_artifact_request_kind_t kind;
    emu_artifact_action_t action;
    const char *path;
} emu_artifact_request_t;

typedef struct {
    emu_artifact_request_kind_t kind;
    uint64_t sequence;
    uint64_t icount;
} emu_artifact_result_t;

typedef void (*emu_frame_callback_t)(void *, const emu_frame_t *);
typedef void (*emu_serial_callback_t)(void *, const uint8_t *, size_t);
typedef void (*emu_event_callback_t)(void *, const emu_event_t *);
typedef void (*emu_audio_output_callback_t)(
    void *, const emu_audio_output_t *);
typedef void (*emu_audio_reset_callback_t)(void *, const emu_audio_reset_t *);

typedef enum {
    EMU_CONSOLE_OUTPUT = 0,
    EMU_CONSOLE_DELAY_WARNING,
} emu_console_kind_t;
/* Ordinary bytes may arrive in chunks; delay warnings are complete lines.
 * Called synchronously by the adapter, including startup and destruction. */
typedef void (*emu_console_callback_t)(void *, emu_console_kind_t,
                                       const char *, size_t);

typedef struct {
    void *opaque;
    emu_console_callback_t console;
    emu_frame_callback_t frame;
    emu_frame_callback_t capture_frame;
    emu_serial_callback_t serial;
    emu_event_callback_t event;
    emu_audio_output_callback_t audio_output;
    emu_audio_reset_callback_t audio_reset;
} emu_callbacks_t;

struct emu_engine_descriptor;

typedef enum {
    EMU_EXECUTION_SLICED = 0,
    EMU_EXECUTION_CONTINUOUS,
} emu_execution_model_t;

typedef struct {
    emu_error_code_t (*create)(const emu_prepared_session_t *,
                               const emu_callbacks_t *, void **, emu_error_t *);
    emu_error_code_t (*run)(void *, const emu_run_request_t *,
                            emu_run_result_t *, emu_error_t *);
    emu_error_code_t (*start)(void *, emu_error_t *);
    emu_error_code_t (*pump)(void *, unsigned, int *, emu_run_result_t *,
                             emu_error_t *);
    emu_error_code_t (*reset)(void *, emu_error_t *);
    emu_error_code_t (*request_stop)(void *, emu_error_t *);
    emu_error_code_t (*query)(void *, int, emu_engine_state_t *,
                              emu_error_t *);
    void (*destroy)(void *);
    emu_error_code_t (*key)(void *, const char *, int, emu_error_t *);
    emu_error_code_t (*serial_rx)(void *, const uint8_t *, size_t, emu_error_t *);
    emu_error_code_t (*sim)(void *, int, emu_error_t *);
    emu_error_code_t (*battery)(void *, unsigned, int, emu_error_t *);
    emu_error_code_t (*snapshot)(void *, const char *, int, emu_error_t *);
    emu_error_code_t (*debugger)(void *, const char *, emu_error_t *);
    emu_error_code_t (*coverage)(void *, int, emu_error_t *);
    emu_error_code_t (*poll)(void *, emu_error_t *);
    emu_error_code_t (*select_events)(void *, const emu_event_selection_t *,
                                      emu_error_t *);
    emu_error_code_t (*key_query)(void *, const char *, emu_key_sample_t *,
                                  emu_error_t *);
    emu_error_code_t (*serial_link)(void *, emu_serial_link_attachment_t,
                                    emu_serial_link_state_t *, emu_error_t *);
    emu_error_code_t (*artifact)(void *, const emu_artifact_request_t *,
                                 emu_artifact_result_t *, emu_error_t *);
} emu_engine_ops_t;

typedef struct {
    emu_error_code_t (*snapshot_restore)(void *, const char *,
                                         emu_error_t *);
    emu_error_code_t (*snapshot_write)(void *, const char *, const char *,
                                       int, emu_error_t *);
    emu_error_code_t (*trace_attach)(void *, emu_trace_sink_t *, int, int,
                                     emu_error_t *);
    emu_error_code_t (*trace_arm)(void *, emu_error_t *);
    emu_error_code_t (*trace_detach)(void *, emu_error_t *);
    /* Host lifecycle diagnostic, ordered by the engine's trace producer. */
    emu_error_code_t (*trace_host_event)(void *, const emu_trace_event_t *,
                                         emu_error_t *);
    emu_error_code_t (*drcov_attach)(void *, emu_drcov_t **,
                                     emu_error_t *);
    emu_error_code_t (*drcov_detach)(void *, emu_error_t *);
    emu_error_code_t (*flash_size)(void *, size_t *, emu_error_t *);
    emu_error_code_t (*flash_read)(void *, size_t, uint8_t *, size_t,
                                   emu_error_t *);
    emu_error_code_t (*debugger_run)(void *, const emu_debugger_request_t *,
                                     emu_error_t *);
    emu_error_code_t (*monitor_start)(void *, uint64_t, uint64_t,
                                      emu_error_t *);
    emu_error_code_t (*monitor_poll)(void *, emu_monitor_verdict_t *,
                                     emu_error_t *);
    emu_error_code_t (*summary_format)(void *, const emu_summary_request_t *,
                                       char *, size_t, emu_error_t *);
    emu_error_code_t (*monitor_stop)(void *, emu_error_t *);
} emu_engine_diagnostic_ops_t;

typedef struct emu_engine_descriptor {
    const char *name;
    const char *version;
    const char *const *supported_models;
    size_t supported_model_count;
    emu_execution_model_t execution_model;
    uint64_t capabilities;
    const emu_engine_diagnostics_t *diagnostics;
    const emu_engine_diagnostic_ops_t *diagnostic_ops;
    emu_engine_ops_t ops;
} emu_engine_descriptor_t;

typedef struct {
    const emu_engine_descriptor_t *items[8];
    size_t count;
} emu_engine_registry_t;

typedef struct emu_session emu_session_t;

const char *emu_error_code_name(emu_error_code_t code);
const char *emu_run_status_name(emu_run_status_t status);
const char *emu_storage_space_name(emu_storage_space_t space);
const char *emu_event_kind_name(emu_event_kind_t kind);
const char *emu_frame_kind_name(emu_frame_kind_t kind);
const char *emu_artifact_request_kind_name(emu_artifact_request_kind_t kind);
void emu_sha256(const void *data, size_t size, uint8_t digest[EMU_SHA256_SIZE]);
void emu_sha256_hex(const uint8_t digest[EMU_SHA256_SIZE],
                    char text[EMU_SHA256_HEX_SIZE]);

void emu_prepared_init(emu_prepared_session_t *prepared);
emu_error_code_t emu_prepared_load_source(emu_prepared_session_t *prepared,
                                           const char *locator,
                                           emu_error_t *error);
emu_error_code_t emu_prepared_rehash_source(const emu_prepared_session_t *prepared,
                                             emu_error_t *error);
emu_error_code_t emu_prepared_add_storage_operation(
    emu_prepared_session_t *prepared, size_t chip_index,
    emu_storage_space_t space, size_t offset,
    const void *expected, size_t expected_size,
    const void *replacement, size_t replacement_size,
    const char *provenance, emu_error_t *error);
emu_error_code_t emu_prepared_add_grouped_storage_operation(
    emu_prepared_session_t *prepared, emu_storage_stage_t stage,
    size_t group, size_t chip_index, emu_storage_space_t space,
    size_t offset, const void *expected, size_t expected_size,
    const void *replacement, size_t replacement_size,
    const char *provenance, emu_error_t *error);
emu_error_code_t emu_prepared_validate(const emu_prepared_session_t *prepared,
                                       uint64_t capabilities,
                                       emu_error_t *error);
void emu_prepared_free(emu_prepared_session_t *prepared);

emu_error_code_t emu_registry_init(emu_engine_registry_t *registry,
                                    emu_error_t *error);
emu_error_code_t emu_registry_register(emu_engine_registry_t *registry,
                                        const emu_engine_descriptor_t *descriptor,
                                        emu_error_t *error);
const emu_engine_descriptor_t *emu_registry_find(
    const emu_engine_registry_t *registry, const char *name);

emu_error_code_t emu_session_create(const emu_engine_registry_t *registry,
                                     const char *engine,
                                     const emu_prepared_session_t *prepared,
                                     const emu_callbacks_t *callbacks,
                                     emu_session_t **out, emu_error_t *error);
emu_error_code_t emu_session_run(emu_session_t *session, uint64_t budget,
                                  emu_run_result_t *result, emu_error_t *error);
emu_error_code_t emu_session_run_request(
    emu_session_t *session, const emu_run_request_t *request,
    emu_run_result_t *result, emu_error_t *error);
emu_error_code_t emu_session_start(emu_session_t *session,
                                    emu_error_t *error);
emu_error_code_t emu_session_pump(emu_session_t *session,
                                   unsigned timeout_ms, int *complete,
                                   emu_run_result_t *result,
                                   emu_error_t *error);
emu_error_code_t emu_session_reset(emu_session_t *session, emu_error_t *error);
emu_error_code_t emu_session_request_stop(emu_session_t *session,
                                           emu_error_t *error);
emu_error_code_t emu_session_query(emu_session_t *session,
                                    emu_engine_state_t *state,
                                    emu_error_t *error);
emu_error_code_t emu_session_query_request(
    emu_session_t *session, int compute_digest,
    emu_engine_state_t *state, emu_error_t *error);
emu_error_code_t emu_session_key(emu_session_t *, const char *, int, emu_error_t *);
emu_error_code_t emu_session_serial_rx(emu_session_t *, const uint8_t *, size_t,
                                       emu_error_t *);
emu_error_code_t emu_session_sim(emu_session_t *, int, emu_error_t *);
emu_error_code_t emu_session_battery(emu_session_t *, unsigned, int, emu_error_t *);
emu_error_code_t emu_session_snapshot(emu_session_t *, const char *, int,
                                       emu_error_t *);
emu_error_code_t emu_session_snapshot_restore(emu_session_t *, const char *,
                                               emu_error_t *);
emu_error_code_t emu_session_snapshot_write(
    emu_session_t *, const char *, const char *, int, emu_error_t *);
emu_error_code_t emu_session_trace_attach(
    emu_session_t *, emu_trace_sink_t *, int, int, emu_error_t *);
emu_error_code_t emu_session_trace_arm(emu_session_t *, emu_error_t *);
emu_error_code_t emu_session_trace_detach(emu_session_t *, emu_error_t *);
emu_error_code_t emu_session_trace_host_event(
    emu_session_t *, const emu_trace_event_t *, emu_error_t *);
emu_error_code_t emu_session_drcov_attach(
    emu_session_t *, emu_drcov_t **, emu_error_t *);
emu_error_code_t emu_session_drcov_detach(emu_session_t *, emu_error_t *);
emu_error_code_t emu_session_flash_size(
    emu_session_t *, size_t *, emu_error_t *);
emu_error_code_t emu_session_flash_read(
    emu_session_t *, size_t, uint8_t *, size_t, emu_error_t *);
emu_error_code_t emu_session_debugger_run(
    emu_session_t *, const emu_debugger_request_t *, emu_error_t *);
emu_error_code_t emu_session_monitor_start(
    emu_session_t *, uint64_t, uint64_t, emu_error_t *);
emu_error_code_t emu_session_monitor_poll(
    emu_session_t *, emu_monitor_verdict_t *, emu_error_t *);
emu_error_code_t emu_session_summary_format(
    emu_session_t *, const emu_summary_request_t *, char *, size_t,
    emu_error_t *);
emu_error_code_t emu_session_monitor_stop(emu_session_t *, emu_error_t *);
emu_error_code_t emu_session_debugger(emu_session_t *, const char *, emu_error_t *);
emu_error_code_t emu_session_coverage(emu_session_t *, int, emu_error_t *);
emu_error_code_t emu_session_poll(emu_session_t *, emu_error_t *);
emu_error_code_t emu_session_select_events(
    emu_session_t *, const emu_event_selection_t *, emu_error_t *);
emu_error_code_t emu_session_key_query(
    emu_session_t *, const char *, emu_key_sample_t *, emu_error_t *);
emu_error_code_t emu_session_serial_link(
    emu_session_t *, emu_serial_link_attachment_t,
    emu_serial_link_state_t *, emu_error_t *);
emu_error_code_t emu_session_artifact(
    emu_session_t *, const emu_artifact_request_t *,
    emu_artifact_result_t *, emu_error_t *);
const emu_engine_descriptor_t *emu_session_engine(const emu_session_t *session);
void emu_session_destroy(emu_session_t **session);

#endif
