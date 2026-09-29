/* C55/M55 firmware-side GSM/L1 receiver observer.
 *
 * The addresses below are evidence for two exact firmware images and are kept
 * out of the shared SoC and baseband transport by design. The probe observes
 * control-flow and queue objects; it has no mutation API. */
#include <stdio.h>
#include <string.h>
#include "gsm_l1_probe.h"
#include "instrumentation.h"
#include "memory_controller.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

typedef struct {
    uint32_t pc;
    uint8_t length;
    uint8_t bytes[8];
} firmware_signature_t;

typedef struct {
    const char *device;
    const char *model;
    int software_version;
    const char *name;
    firmware_signature_t signatures[25];
    size_t signature_count;
    uint16_t receiver_owner;
    uint16_t receiver_queue_offset, receiver_queue_segment;
    uint16_t task_table_offset, task_table_segment;
    uint16_t current_signal_offset, current_signal_segment;
    uint32_t state_addr;
    uint32_t state_setter_pc;
    uint32_t search_status_pc;
    uint32_t timer_target_pc;
    uint32_t timer_insert_target_pc, timer_insert_caller_pc;
    uint32_t periodic_callback_pc, periodic_worker_pc;
    uint32_t periodic_accumulator_addr;
    uint32_t timer_2019_caller_pc;
    uint32_t timer_201b_caller_pc;
    uint32_t lower_dispatch_pc;
    uint32_t service_adapter_pc;
    uint32_t receiver_dispatcher_pc;
    uint32_t enqueue_pc;
    uint32_t dequeue_pc;
    uint32_t dequeue_post_pc;
    uint32_t acknowledge_pc;
    uint32_t event_002d_handler_pc;
    uint32_t event_0039_handler_pc;
    uint32_t event_0040_handler_pc;
    uint32_t event_0006_handler_pc;
    uint16_t preexisting_startup_event;
    uint32_t strength_setter_pc;
    uint32_t candidate_ptr_addr;
    uint32_t candidate_seg_addr;
    uint32_t xp0_isr_pc, xp1_isr_pc;
    uint32_t xp2_isr_pc, irq45_isr_pc, irq80_isr_pc;
    uint32_t elapsed_feed_pc, elapsed_worker_pc;
    uint32_t elapsed_deadline_caller_pc;
    uint32_t elapsed_pending_addr, elapsed_active_count_addr;
    uint32_t elapsed_head_offset_addr, elapsed_head_segment_addr;
    uint32_t constructor_pcs[4];
    uint32_t current_source_offset_addr, current_source_segment_addr;
    uint32_t buffered_envelope_root_addr;
} gsm_l1_probe_profile_t;

#define SIG4(address, a, b, c, d) \
    {.pc = (address), .length = 4, .bytes = {(a), (b), (c), (d)}}
#define SIG8(address, a, b, c, d, e, f, g, h) \
    {.pc = (address), .length = 8, \
     .bytes = {(a), (b), (c), (d), (e), (f), (g), (h)}}

static const gsm_l1_probe_profile_t PROFILES[] = {
    {
        .device = "c55", .model = "C55", .software_version = 24,
        .name = "C55-SW24",
        .signatures = {
            SIG4(0xBB1D56, 0x46, 0xFC, 0x03, 0x03),
            SIG4(0xBBE20E, 0x39, 0x00, 0x2E, 0xE6),
            SIG8(0xBBE2C2, 0xE6, 0xFC, 0x6C, 0x3F,
                            0xE6, 0xFD, 0x55, 0x00),
            SIG8(0xBBE884, 0xDA, 0xBC, 0x8E, 0x97,
                            0xDA, 0xBB, 0xB2, 0xEF),
            SIG4(0xBBE62E, 0x88, 0x90, 0x88, 0x80),
            SIG8(0xBBE78A, 0xD7, 0x50, 0x55, 0x00,
                            0xF2, 0xFC, 0x76, 0x3F),
            SIG8(0x920E58, 0x88, 0x90, 0x88, 0x80,
                            0x88, 0x70, 0x88, 0x60),
            SIG8(0x9218D2, 0x88, 0x90, 0x88, 0x80,
                            0xF0, 0x9D, 0xF0, 0x8C),
            SIG8(0x9217FC, 0xE6, 0x00, 0x0A, 0x00,
                            0xCC, 0x00, 0xF2, 0xFC),
            SIG8(0xBD9C14, 0x98, 0xE0, 0x98, 0xD0,
                            0x98, 0xC0, 0x48, 0x40),
            SIG8(0x973EE0, 0x26, 0x09, 0x1E, 0x00,
                            0xEC, 0xF0, 0xD1, 0x30),
            SIG8(0x977A06, 0x26, 0x09, 0x1E, 0x00,
                            0xEC, 0xF0, 0xD1, 0x30),
            SIG8(0x92323E, 0x88, 0x90, 0x88, 0x80,
                            0x88, 0x70, 0x88, 0x60),
            SIG8(0x96FFAA, 0x88, 0x80, 0xDA, 0xB4,
                            0x34, 0xFF, 0xF2, 0xFC),
            SIG8(0x92119A, 0x88, 0xE0, 0x88, 0xD0,
                            0x88, 0xC0, 0x88, 0x90),
            SIG8(0x9212F0, 0x88, 0x90, 0x88, 0x80,
                            0x88, 0xC0, 0x88, 0xD0),
            SIG8(0x921B2E, 0xE6, 0x00, 0x0A, 0x00,
                            0xCC, 0x00, 0xF2, 0xFE),
            SIG8(0x922ED6, 0x88, 0x90, 0x88, 0x80,
                            0x88, 0x70, 0x88, 0x60),
            SIG8(0x923056, 0xE6, 0xFC, 0xDC, 0x1B,
                            0xE6, 0xFD, 0x0A, 0x00),
            SIG8(0x92915E, 0x26, 0x09, 0x1E, 0x00,
                            0xEC, 0xF0, 0xD1, 0x30),
            SIG8(0x9291A2, 0x26, 0x09, 0x1E, 0x00,
                            0xEC, 0xF0, 0xD1, 0x30),
            SIG8(0x90B144, 0xAF, 0x06, 0xBF, 0x0D,
                            0xD1, 0x20, 0xFB, 0x88),
            SIG8(0xBBE5DC, 0xE6, 0xFC, 0x84, 0x1E,
                            0xE6, 0xFD, 0x0A, 0x00),
            SIG8(0xBC4184, 0x88, 0x90, 0x88, 0x80,
                            0xF0, 0x9D, 0xF0, 0x8C),
            SIG8(0xBC4234, 0x88, 0x90, 0xE6, 0x00,
                            0x56, 0x00, 0x88, 0x80),
        },
        .signature_count = 25,
        .receiver_owner = 0x67,
        .receiver_queue_offset = 0x1FBC, .receiver_queue_segment = 0x000A,
        .task_table_offset = 0x1C84, .task_table_segment = 0x000A,
        .current_signal_offset = 0x3F6C, .current_signal_segment = 0x0055,
        .state_addr = 0x157972, .state_setter_pc = 0xBB0B8C,
        .search_status_pc = 0xBB1D56,
        .timer_target_pc = 0x922ED6,
        .timer_insert_target_pc = 0x91EADC,
        .timer_insert_caller_pc = 0x923062,
        .periodic_callback_pc = 0xBC4184,
        .periodic_worker_pc = 0xBC4234,
        .periodic_accumulator_addr = 0x158130,
        .timer_2019_caller_pc = 0xBB205A,
        .timer_201b_caller_pc = 0xBB1D9C,
        .lower_dispatch_pc = 0xBB2EC0,
        .service_adapter_pc = 0xBB4382,
        .receiver_dispatcher_pc = 0xBBE2C2,
        .enqueue_pc = 0x920E58,
        .dequeue_pc = 0x9218D2, .dequeue_post_pc = 0xBD9C14,
        .acknowledge_pc = 0x9217FC,
        .event_002d_handler_pc = 0xBBE884,
        .event_0039_handler_pc = 0xBBE62E,
        .event_0040_handler_pc = 0xBBE78A,
        .event_0006_handler_pc = 0xBBE5DC,
        .preexisting_startup_event = 0x00FB,
        .strength_setter_pc = 0xBC6CAA,
        .candidate_ptr_addr = 0x1580B2, .candidate_seg_addr = 0x1580B4,
        .xp0_isr_pc = 0x973EE0, .xp1_isr_pc = 0x977A06,
        .xp2_isr_pc = 0x92915E, .irq45_isr_pc = 0x9291A2,
        .irq80_isr_pc = 0x90B144,
        .elapsed_feed_pc = 0x922A7E, .elapsed_worker_pc = 0x92323E,
        .elapsed_deadline_caller_pc = 0x923376,
        .elapsed_pending_addr = 0x02A484,
        .elapsed_active_count_addr = 0x029BE6,
        .elapsed_head_offset_addr = 0x029BDC,
        .elapsed_head_segment_addr = 0x029BDE,
        .constructor_pcs = {0x9210F0, 0x92119A, 0x9212F0, 0x921B2E},
        .current_source_offset_addr = 0x029C80,
        .current_source_segment_addr = 0x029C82,
        .buffered_envelope_root_addr = 0x029BFC,
    },
    {
        .device = "m55", .model = "M55", .software_version = 91,
        .name = "M55-SW91",
        .signatures = {
            SIG4(0xB7EDA6, 0x46, 0xFC, 0x03, 0x03),
            SIG4(0x64FA92, 0x39, 0x00, 0x0A, 0x9D),
            SIG8(0xB899B0, 0xE6, 0xFC, 0xCC, 0x08,
                            0xE6, 0xFD, 0x61, 0x00),
            SIG8(0xB89F60, 0xDA, 0xB9, 0x70, 0x43,
                            0xDA, 0xB8, 0x4C, 0xA6),
            SIG4(0xB89D0A, 0x88, 0x90, 0x88, 0x80),
            SIG8(0xB89E66, 0xD7, 0x50, 0x61, 0x00,
                            0xF2, 0xFC, 0xD6, 0x08),
            SIG8(0xA05E3E, 0x88, 0x90, 0x88, 0x80,
                            0x88, 0x70, 0x88, 0x60),
            SIG8(0xA068B8, 0x88, 0x90, 0x88, 0x80,
                            0xF0, 0x9D, 0xF0, 0x8C),
            SIG8(0xA067E2, 0xE6, 0x00, 0x11, 0x00,
                            0xCC, 0x00, 0xF2, 0xFC),
            SIG8(0xBA657A, 0x98, 0xE0, 0x98, 0xD0,
                            0x98, 0xC0, 0x48, 0x40),
            SIG8(0xA3E00A, 0x26, 0x09, 0x1E, 0x00,
                            0xEC, 0xF0, 0xD1, 0x30),
            SIG8(0xA3E0DE, 0x26, 0x09, 0x1E, 0x00,
                            0xEC, 0xF0, 0xD1, 0x30),
            SIG8(0xA08304, 0x88, 0x90, 0x88, 0x80,
                            0x88, 0x70, 0x88, 0x60),
            SIG8(0xA3DFC2, 0x88, 0x80, 0xDA, 0xB4,
                            0x94, 0xFD, 0xF2, 0xFC),
            SIG8(0xA06180, 0x88, 0xE0, 0x88, 0xD0,
                            0x88, 0xC0, 0x88, 0x90),
            SIG8(0xA062D6, 0x88, 0x90, 0x88, 0x80,
                            0x88, 0xC0, 0x88, 0xD0),
            SIG8(0xA06B14, 0xE6, 0x00, 0x11, 0x00,
                            0xCC, 0x00, 0xF2, 0xFE),
            SIG8(0xA07F9C, 0x88, 0x90, 0x88, 0x80,
                            0x88, 0x70, 0x88, 0x60),
            SIG8(0xA0811C, 0xE6, 0xFC, 0x6C, 0x14,
                            0xE6, 0xFD, 0x11, 0x00),
            SIG8(0x9AFFB6, 0x26, 0x09, 0x1E, 0x00,
                            0xEC, 0xF0, 0xD1, 0x30),
            SIG8(0x9DFFA2, 0x26, 0x09, 0x1E, 0x00,
                            0xEC, 0xF0, 0xD1, 0x30),
            SIG8(0x34FFF8, 0xFF, 0x0B, 0xBF, 0x13,
                            0xD1, 0x20, 0xFB, 0x88),
            SIG8(0xB89CB8, 0xE6, 0xFC, 0xC0, 0x19,
                            0xE6, 0xFD, 0x11, 0x00),
            SIG8(0xB8F4AC, 0x88, 0x90, 0x88, 0x80,
                            0xF0, 0x9D, 0xF0, 0x8C),
            SIG8(0xB8F55C, 0x88, 0x90, 0xE6, 0x00,
                            0x61, 0x00, 0x88, 0x80),
        },
        .signature_count = 25,
        .receiver_owner = 0x67,
        .receiver_queue_offset = 0x1AF8, .receiver_queue_segment = 0x0011,
        .task_table_offset = 0x17C0, .task_table_segment = 0x0011,
        .current_signal_offset = 0x08CC, .current_signal_segment = 0x0061,
        .state_addr = 0x00B49A, .state_setter_pc = 0xB7E08E,
        .search_status_pc = 0xB7EDA6,
        .timer_target_pc = 0xA07F9C,
        .timer_insert_target_pc = 0xA03FA6,
        .timer_insert_caller_pc = 0xA08128,
        .periodic_callback_pc = 0xB8F4AC,
        .periodic_worker_pc = 0xB8F55C,
        .periodic_accumulator_addr = 0x184A3E,
        .timer_2019_caller_pc = 0xB7F0A4,
        .timer_201b_caller_pc = 0xB7EDEC,
        .lower_dispatch_pc = 0xB80024,
        .service_adapter_pc = 0xB811E4,
        .receiver_dispatcher_pc = 0xB899B0,
        .enqueue_pc = 0xA05E3E,
        .dequeue_pc = 0xA068B8, .dequeue_post_pc = 0xBA657A,
        .acknowledge_pc = 0xA067E2,
        .event_002d_handler_pc = 0xB89F60,
        .event_0039_handler_pc = 0xB89D0A,
        .event_0040_handler_pc = 0xB89E66,
        .event_0006_handler_pc = 0xB89CB8,
        .preexisting_startup_event = 0x019A,
        .strength_setter_pc = 0xB91E1A,
        .candidate_ptr_addr = 0x1849CA, .candidate_seg_addr = 0x1849CC,
        .xp0_isr_pc = 0xA3E00A, .xp1_isr_pc = 0xA3E0DE,
        .xp2_isr_pc = 0x9AFFB6, .irq45_isr_pc = 0x9DFFA2,
        .irq80_isr_pc = 0x34FFF8,
        .elapsed_feed_pc = 0xA07ACE, .elapsed_worker_pc = 0xA08304,
        .elapsed_deadline_caller_pc = 0xA0843A,
        .elapsed_pending_addr = 0x046054,
        .elapsed_active_count_addr = 0x045476,
        .elapsed_head_offset_addr = 0x04546C,
        .elapsed_head_segment_addr = 0x04546E,
        .constructor_pcs = {0xA060D6, 0xA06180, 0xA062D6, 0xA06B14},
        .current_source_offset_addr = 0x0457BC,
        .current_source_segment_addr = 0x0457BE,
        .buffered_envelope_root_addr = 0x04548C,
    },
};

typedef struct {
    int has_owner; uint16_t owner;
    int has_caller; uint32_t caller;
    int has_event; uint16_t event_id;
    int has_object; uint16_t object_offset, object_segment;
    int has_status; uint16_t status;
    int has_selector; uint16_t selector;
    int has_state; uint16_t state_before, state_requested;
    int has_timer; uint16_t handle_offset, handle_segment; uint32_t delay;
    int has_value; uint32_t value;
    int has_queue; uint32_t queue;
    int has_source; uint16_t source;
    int has_raw_event; uint16_t raw_event;
    int has_normalized_event; uint16_t normalized_event;
    int has_payload_size; uint32_t payload_size;
    int has_hardware_address; uint32_t hardware_address;
    int has_hardware_value; uint16_t hardware_value;
} probe_fields_t;

static uint32_t far_address(uint16_t offset, uint16_t segment) {
    return ((uint32_t)segment << 14) + offset;
}

static uint16_t peek_far16(const gsm_l1_probe_t *probe, uint16_t offset,
                           uint16_t segment, uint16_t displacement) {
    return cemu_memory_controller_peek16(&probe->soc->memory,
                                    far_address(offset, segment) + displacement);
}

static uint16_t peek_stack16(const gsm_l1_probe_t *probe,
                             uint16_t displacement) {
    /* The firmware ABI uses R0 as its software argument stack. CALLS' system
     * SP contains only the architectural return frame. Normal indirect data
     * addressing selects DPP0..3 with the top two bits of R0. */
    uint16_t sp = (uint16_t)(cemu_cpu_gpr(probe->cpu, 0) + displacement);
    uint32_t dpp_addr = 0x00FE00 + 2u * (sp >> 14);
    uint16_t dpp = cemu_memory_controller_peek16(
        &probe->soc->memory, dpp_addr) & 0x03FF;
    return cemu_memory_controller_peek16(
        &probe->soc->memory,
        ((uint32_t)dpp << 14) | (sp & 0x3FFF));
}

static void add_nullable_int(cemu_event_fields_t *info, const char *key,
                             int present, long value) {
    if (present) cemu_event_field_i64(info, key, value);
    else cemu_event_field_null(info, key);
}

static void emit_probe(gsm_l1_probe_t *probe, const char *role,
                       uint32_t event_addr, probe_fields_t f) {
    const gsm_l1_probe_profile_t *p = probe->profile;
    cemu_event_fields_t info = {0};
    add_nullable_int(&info, "caller_pc", f.has_caller, f.caller);
    add_nullable_int(&info, "event_id", f.has_event, f.event_id);
    add_nullable_int(&info, "handle_offset", f.has_timer, f.handle_offset);
    add_nullable_int(&info, "handle_segment", f.has_timer, f.handle_segment);
    cemu_event_field_null(&info, "hardware_surface");
    add_nullable_int(&info, "hardware_address", f.has_hardware_address,
                     f.hardware_address);
    add_nullable_int(&info, "hardware_value", f.has_hardware_value,
                     f.hardware_value);
    add_nullable_int(&info, "normalized_event", f.has_normalized_event,
                     f.normalized_event);
    add_nullable_int(&info, "object_offset", f.has_object, f.object_offset);
    add_nullable_int(&info, "object_segment", f.has_object, f.object_segment);
    add_nullable_int(&info, "owner", f.has_owner, f.owner);
    add_nullable_int(&info, "payload_size", f.has_payload_size,
                     f.payload_size);
    cemu_event_field_string(&info, "profile", p->name);
    add_nullable_int(&info, "queue", f.has_queue, f.queue);
    add_nullable_int(&info, "raw_event", f.has_raw_event, f.raw_event);
    add_nullable_int(&info, "receiver_state", 1,
                     cemu_memory_controller_peek16(&probe->soc->memory,
                                              p->state_addr));
    cemu_event_field_string(&info, "role", role);
    add_nullable_int(&info, "search_status", f.has_status, f.status);
    add_nullable_int(&info, "selector", f.has_selector, f.selector);
    add_nullable_int(&info, "source", f.has_source, f.source);
    add_nullable_int(&info, "state_before", f.has_state, f.state_before);
    add_nullable_int(&info, "state_requested", f.has_state,
                     f.state_requested);
    add_nullable_int(&info, "target_pc", 1, event_addr);
    add_nullable_int(&info, "timer_delay", f.has_timer, f.delay);
    add_nullable_int(&info, "value", f.has_value, f.value);
    cemu_soc_emit_native_trace(probe->soc, "gsm_l1_firmware", 1, event_addr,
                   0, 0, f.has_value, f.value, role, &info);
}

static void emit_receiver_probe(gsm_l1_probe_t *probe, const char *role,
                                uint32_t event_addr, probe_fields_t f) {
    const gsm_l1_probe_profile_t *p = probe->profile;
    f.has_owner = 1;
    f.owner = p->receiver_owner;
    emit_probe(probe, role, event_addr, f);
}

static int task_owner(const gsm_l1_probe_profile_t *p, uint16_t offset,
                      uint16_t segment, uint16_t *owner) {
    if (segment != p->task_table_segment || offset < p->task_table_offset)
        return 0;
    uint16_t displacement = (uint16_t)(offset - p->task_table_offset);
    if (displacement % 8) return 0;
    uint16_t value = displacement / 8;
    if (value > 0xFF) return 0;
    *owner = value;
    return 1;
}

static int current_source_owner(const gsm_l1_probe_t *probe,
                                uint16_t *owner) {
    const gsm_l1_probe_profile_t *p = probe->profile;
    uint16_t offset = cemu_memory_controller_peek16(
        &probe->soc->memory, p->current_source_offset_addr);
    uint16_t segment = cemu_memory_controller_peek16(
        &probe->soc->memory, p->current_source_segment_addr);
    return task_owner(p, offset, segment, owner);
}

static probe_fields_t timer_registration_fields(gsm_l1_probe_t *probe) {
    const gsm_l1_probe_profile_t *p = probe->profile;
    uint16_t handle_offset = cemu_cpu_gpr(probe->cpu, 12);
    uint16_t handle_segment = cemu_cpu_gpr(probe->cpu, 13);
    uint16_t event = peek_stack16(probe, 0);
    uint16_t callback_offset = peek_stack16(probe, 2);
    uint16_t callback_segment = peek_stack16(probe, 4);
    uint16_t source = 0;
    uint32_t delay = ((uint32_t)cemu_cpu_gpr(probe->cpu, 15) << 16) |
                     cemu_cpu_gpr(probe->cpu, 14);

    return (probe_fields_t){
        /* The three software-stack words are event plus callback/repeat
         * state. Ownership is not available until the allocated timer node
         * reaches the confirmed ordered-list insertion site. */
        .has_event = 1, .event_id = event,
        .has_object = callback_offset || callback_segment,
        .object_offset = callback_offset, .object_segment = callback_segment,
        .has_timer = 1,
        .handle_offset = handle_offset, .handle_segment = handle_segment,
        .delay = delay,
        .has_value = 1,
        .value = cemu_memory_controller_peek16(
            &probe->soc->memory, p->elapsed_active_count_addr),
        .has_source = current_source_owner(probe, &source), .source = source,
        .has_raw_event = 1, .raw_event = event,
        .has_normalized_event = 1, .normalized_event = event & 0x03FF,
        .has_hardware_address = 1,
        .hardware_address = p->elapsed_pending_addr,
        .has_hardware_value = 1,
        .hardware_value = cemu_memory_controller_peek16(
            &probe->soc->memory, p->elapsed_pending_addr),
    };
}

static void observe_timer_insert(gsm_l1_probe_t *probe, uint32_t caller,
                                 uint32_t target) {
    const gsm_l1_probe_profile_t *p = probe->profile;
    uint16_t handle_offset = cemu_cpu_gpr(probe->cpu, 14);
    uint16_t handle_segment = cemu_cpu_gpr(probe->cpu, 15);
    uint16_t node_offset = peek_far16(probe, handle_offset, handle_segment, 0);
    uint16_t node_segment = peek_far16(probe, handle_offset, handle_segment, 2);
    uint32_t deadline = (uint32_t)peek_far16(
        probe, node_offset, node_segment, 0) |
        ((uint32_t)peek_far16(probe, node_offset, node_segment, 2) << 16);
    uint32_t callback_state = (uint32_t)peek_far16(
        probe, node_offset, node_segment, 4) |
        ((uint32_t)peek_far16(probe, node_offset, node_segment, 6) << 16);
    uint16_t target_offset = peek_far16(
        probe, node_offset, node_segment, 8);
    uint16_t target_segment = peek_far16(
        probe, node_offset, node_segment, 10);
    uint16_t event = peek_far16(probe, node_offset, node_segment, 12);
    uint16_t owner = 0, source = 0;
    probe_fields_t f = {
        .has_caller = 1, .caller = caller,
        .has_owner = task_owner(p, target_offset, target_segment, &owner),
        .owner = owner,
        .has_event = 1, .event_id = event,
        .has_object = node_offset || node_segment,
        .object_offset = node_offset, .object_segment = node_segment,
        .has_timer = 1,
        .handle_offset = handle_offset, .handle_segment = handle_segment,
        /* At this confirmed insertion point timer_delay is the absolute
         * deadline; timer_registration carries the requested relative delay. */
        .delay = deadline,
        .has_value = 1, .value = callback_state,
        .has_queue = target_offset || target_segment,
        .queue = far_address(target_offset, target_segment),
        .has_source = current_source_owner(probe, &source), .source = source,
        .has_raw_event = 1, .raw_event = event,
        .has_normalized_event = 1, .normalized_event = event & 0x03FF,
        .has_hardware_address = 1,
        .hardware_address = p->elapsed_active_count_addr,
        .has_hardware_value = 1,
        .hardware_value = cemu_memory_controller_peek16(
            &probe->soc->memory, p->elapsed_active_count_addr),
    };
    emit_probe(probe, "timer_insert", target, f);
}

static probe_fields_t current_signal_fields(gsm_l1_probe_t *probe,
                                             int include_payload) {
    const gsm_l1_probe_profile_t *p = probe->profile;
    uint16_t source_offset = peek_far16(probe, p->current_signal_offset,
                                        p->current_signal_segment, 0);
    uint16_t source_segment = peek_far16(probe, p->current_signal_offset,
                                         p->current_signal_segment, 2);
    uint16_t normalized = peek_far16(probe, p->current_signal_offset,
                                     p->current_signal_segment, 4) & 0x03FF;
    uint16_t source = 0;
    uint16_t object_offset = peek_far16(probe, p->current_signal_offset,
                                        p->current_signal_segment, 10);
    uint16_t object_segment = peek_far16(probe, p->current_signal_offset,
                                         p->current_signal_segment, 12);
    probe_fields_t f = {
        .has_event = 1, .event_id = normalized,
        .has_queue = 1,
        .queue = far_address(p->receiver_queue_offset,
                             p->receiver_queue_segment),
        .has_normalized_event = 1, .normalized_event = normalized,
        .has_source = task_owner(p, source_offset, source_segment, &source),
        .source = source,
    };
    if (include_payload && (object_offset || object_segment)) {
        f.has_object = 1;
        f.object_offset = object_offset;
        f.object_segment = object_segment;
        if (normalized == 0x0039) {
            f.has_payload_size = 1;
            f.payload_size = 0x01EC;
        }
    }
    return f;
}

static void observe_enqueue(gsm_l1_probe_t *probe, uint32_t caller,
                            uint32_t target) {
    const gsm_l1_probe_profile_t *p = probe->profile;
    uint16_t envelope_offset = cemu_cpu_gpr(probe->cpu, 12);
    uint16_t envelope_segment = cemu_cpu_gpr(probe->cpu, 13);
    uint16_t target_offset = peek_far16(probe, envelope_offset,
                                        envelope_segment, 4);
    uint16_t target_segment = peek_far16(probe, envelope_offset,
                                         envelope_segment, 6);
    if (target_offset != p->receiver_queue_offset ||
        target_segment != p->receiver_queue_segment)
        return;
    uint16_t source_offset = peek_far16(probe, envelope_offset,
                                        envelope_segment, 8);
    uint16_t source_segment = peek_far16(probe, envelope_offset,
                                         envelope_segment, 10);
    uint16_t raw_event = peek_far16(probe, envelope_offset,
                                    envelope_segment, 12);
    uint16_t source = 0;
    probe_fields_t f = {
        .has_caller = 1, .caller = caller,
        .has_event = 1, .event_id = raw_event & 0x03FF,
        .has_object = envelope_offset || envelope_segment,
        .object_offset = envelope_offset, .object_segment = envelope_segment,
        .has_queue = 1, .queue = far_address(target_offset, target_segment),
        .has_source = task_owner(p, source_offset, source_segment, &source),
        .source = source,
        .has_raw_event = 1, .raw_event = raw_event,
        .has_normalized_event = 1,
        .normalized_event = raw_event & 0x03FF,
    };
    if ((raw_event & 0x03FF) == 0x0039) {
        f.has_payload_size = 1;
        f.payload_size = 0x01EC;
    }
    emit_receiver_probe(probe, "queue_enqueue", target, f);
}

static void observe_handler(gsm_l1_probe_t *probe, const char *role,
                            uint32_t caller, uint32_t target) {
    probe_fields_t f = current_signal_fields(probe, 1);
    f.has_caller = 1;
    f.caller = caller;
    emit_receiver_probe(probe, role, target, f);
}

static probe_fields_t constructor_fields(gsm_l1_probe_t *probe,
                                         unsigned constructor) {
    const gsm_l1_probe_profile_t *p = probe->profile;
    uint16_t target_offset = cemu_cpu_gpr(probe->cpu, 12);
    uint16_t target_segment = cemu_cpu_gpr(probe->cpu, 13);
    uint16_t source_offset = 0, source_segment = 0, raw_event = 0;
    int has_event = 1;

    if (constructor < 2) {
        raw_event = cemu_cpu_gpr(probe->cpu, 14);
        source_offset = cemu_memory_controller_peek16(
            &probe->soc->memory, p->current_source_offset_addr);
        source_segment = cemu_memory_controller_peek16(
            &probe->soc->memory, p->current_source_segment_addr);
    } else if (constructor == 2) {
        uint16_t object_offset = cemu_cpu_gpr(probe->cpu, 14);
        uint16_t object_segment = cemu_cpu_gpr(probe->cpu, 15);
        source_offset = peek_far16(probe, object_offset, object_segment, 0);
        source_segment = peek_far16(probe, object_offset, object_segment, 2);
        raw_event = peek_far16(probe, object_offset, object_segment, 4);
    } else {
        uint16_t root_offset = cemu_memory_controller_peek16(
            &probe->soc->memory, p->buffered_envelope_root_addr);
        uint16_t root_segment = cemu_memory_controller_peek16(
            &probe->soc->memory, p->buffered_envelope_root_addr + 2);
        uint16_t envelope_offset = peek_far16(
            probe, root_offset, root_segment, 8);
        uint16_t envelope_segment = peek_far16(
            probe, root_offset, root_segment, 10);
        if (envelope_offset || envelope_segment) {
            source_offset = peek_far16(
                probe, envelope_offset, envelope_segment, 8);
            source_segment = peek_far16(
                probe, envelope_offset, envelope_segment, 10);
            raw_event = peek_far16(
                probe, envelope_offset, envelope_segment, 12);
        } else {
            has_event = 0;
        }
    }

    uint16_t owner = 0, source = 0;
    int has_owner = task_owner(p, target_offset, target_segment, &owner);
    int has_source = task_owner(p, source_offset, source_segment, &source);
    probe_fields_t f = {
        .has_owner = has_owner,
        .owner = owner,
        .has_queue = target_offset || target_segment,
        .queue = far_address(target_offset, target_segment),
        .has_source = has_source,
        .source = source,
        .has_event = has_event,
        .event_id = raw_event & 0x03FF,
        .has_raw_event = has_event,
        .raw_event = raw_event,
        .has_normalized_event = has_event,
        .normalized_event = raw_event & 0x03FF,
    };
    if (has_event && (raw_event & 0x03FF) == 0x0039) {
        f.has_payload_size = 1;
        f.payload_size = 0x01EC;
    }
    return f;
}

static int constructor_index(const gsm_l1_probe_profile_t *p,
                             uint32_t target) {
    for (unsigned i = 0; i < ARRAY_LEN(p->constructor_pcs); i++)
        if (target == p->constructor_pcs[i]) return (int)i;
    return -1;
}

static void probe_event(void *ctx, const cemu_event_t *event) {
    gsm_l1_probe_t *probe = ctx;
    const gsm_l1_probe_profile_t *p = probe->profile;
    if (event->type == CEMU_EVENT_INSTRUCTION) {
        const instrumentation_instruction_t *in = &event->as.instruction;
        if (in->pc_before == p->xp0_isr_pc ||
            in->pc_before == p->xp1_isr_pc) {
            emit_probe(probe,
                       in->pc_before == p->xp0_isr_pc
                           ? "xp0_interrupt" : "xp1_interrupt",
                       in->pc_before, (probe_fields_t){
                .has_hardware_address = 1, .hardware_address = 0x00E034,
                .has_hardware_value = 1,
                .hardware_value = cemu_memory_controller_peek16(
                    &probe->soc->memory, 0x00E034),
            });
        } else if (in->pc_before == p->xp2_isr_pc ||
                   in->pc_before == p->irq45_isr_pc ||
                   in->pc_before == p->irq80_isr_pc) {
            const char *role = in->pc_before == p->xp2_isr_pc
                             ? "xp2_interrupt"
                             : in->pc_before == p->irq45_isr_pc
                             ? "irq45_interrupt" : "irq80_interrupt";
            uint32_t ic_addr = in->pc_before == p->xp2_isr_pc ? 0x00F196
                             : in->pc_before == p->irq45_isr_pc ? 0x00F190
                             : 0x00F140;
            emit_probe(probe, role, in->pc_before, (probe_fields_t){
                .has_hardware_address = 1, .hardware_address = ic_addr,
                .has_hardware_value = 1,
                .hardware_value = cemu_memory_controller_peek16(
                    &probe->soc->memory, ic_addr),
            });
        } else if (in->pc_before == p->periodic_callback_pc) {
            uint16_t object_offset = cemu_cpu_gpr(probe->cpu, 12);
            uint16_t object_segment = cemu_cpu_gpr(probe->cpu, 13);
            emit_receiver_probe(probe, "receiver_periodic_callback",
                                in->pc_before, (probe_fields_t){
                .has_object = object_offset || object_segment,
                .object_offset = object_offset,
                .object_segment = object_segment,
                .has_value = 1,
                .value = cemu_memory_controller_peek16(
                    &probe->soc->memory, p->periodic_accumulator_addr),
                .has_hardware_address = 1,
                .hardware_address = p->periodic_accumulator_addr,
                .has_hardware_value = 1,
                .hardware_value = cemu_memory_controller_peek16(
                    &probe->soc->memory, p->periodic_accumulator_addr + 2),
            });
        } else if (in->pc_before == p->periodic_worker_pc) {
            uint16_t object_offset = cemu_memory_controller_peek16(
                &probe->soc->memory, p->candidate_ptr_addr);
            uint16_t object_segment = cemu_memory_controller_peek16(
                &probe->soc->memory, p->candidate_seg_addr);
            emit_receiver_probe(probe, "receiver_periodic_worker",
                                in->pc_before, (probe_fields_t){
                .has_object = object_offset || object_segment,
                .object_offset = object_offset,
                .object_segment = object_segment,
                .has_hardware_address = 1,
                .hardware_address = p->candidate_ptr_addr,
                .has_hardware_value = 1,
                .hardware_value = object_offset,
            });
        } else if (in->pc_before == p->elapsed_worker_pc) {
            uint16_t head_offset = cemu_memory_controller_peek16(
                &probe->soc->memory, p->elapsed_head_offset_addr);
            uint16_t head_segment = cemu_memory_controller_peek16(
                &probe->soc->memory, p->elapsed_head_segment_addr);
            emit_probe(probe, "elapsed_worker", in->pc_before,
                       (probe_fields_t){
                .has_queue = head_offset || head_segment,
                .queue = far_address(head_offset, head_segment),
                .has_value = 1,
                .value = cemu_memory_controller_peek16(
                    &probe->soc->memory, p->elapsed_active_count_addr),
                .has_hardware_address = 1,
                .hardware_address = p->elapsed_pending_addr,
                .has_hardware_value = 1,
                .hardware_value = cemu_memory_controller_peek16(
                    &probe->soc->memory, p->elapsed_pending_addr),
            });
        } else if (in->pc_before == p->search_status_pc) {
            uint16_t status = cemu_cpu_gpr(probe->cpu, 12);
            emit_receiver_probe(
                probe, "search_status", in->pc_before,
                (probe_fields_t){.has_status = 1, .status = status});
        } else if (in->pc_before == p->dequeue_post_pc &&
                   probe->dequeue_pending) {
            probe->dequeue_pending = 0;
            if (cemu_cpu_gpr(probe->cpu, 4)) {
                emit_receiver_probe(probe, "queue_dequeue", in->pc_before,
                                    current_signal_fields(probe, 1));
                probe->receiver_message_active = 1;
            }
        }
        return;
    }
    if (event->type != CEMU_EVENT_CONTROL || event->as.control.is_return)
        return;
    const instrumentation_control_t *control = &event->as.control;
    uint32_t target = control->target_pc & 0xFFFFFFu;
    uint32_t caller = control->caller_pc & 0xFFFFFFu;
    int constructor = constructor_index(p, target);
    if (target == p->elapsed_feed_pc) {
        emit_probe(probe, "elapsed_feed", target, (probe_fields_t){
            .has_caller = 1, .caller = caller,
            .has_value = 1, .value = cemu_cpu_gpr(probe->cpu, 12),
            .has_hardware_address = 1, .hardware_address = 0x00E034,
            .has_hardware_value = 1,
            .hardware_value = cemu_memory_controller_peek16(
                &probe->soc->memory, 0x00E034),
        });
    } else if (constructor >= 0) {
        probe_fields_t f = constructor_fields(probe, (unsigned)constructor);
        f.has_caller = 1;
        f.caller = caller;
        emit_probe(probe, "constructor_attempt", target, f);
        if (caller == p->elapsed_deadline_caller_pc)
            emit_probe(probe, "elapsed_deadline_dispatch", target, f);
        if (f.has_normalized_event &&
            (f.normalized_event == 0x002D ||
             f.normalized_event == 0x0039 ||
             f.normalized_event == 0x0040))
            emit_probe(probe, "constructor_candidate", target, f);
    } else if (target == p->timer_insert_target_pc &&
               caller == p->timer_insert_caller_pc) {
        observe_timer_insert(probe, caller, target);
    } else if (target == p->state_setter_pc) {
        emit_receiver_probe(probe, "state_set_request", target, (probe_fields_t){
            .has_caller = 1, .caller = caller, .has_state = 1,
            .state_before = cemu_memory_controller_peek16(
                &probe->soc->memory, p->state_addr),
            .state_requested = cemu_cpu_gpr(probe->cpu, 12),
        });
    } else if (target == p->timer_target_pc) {
        probe_fields_t f = timer_registration_fields(probe);
        f.has_caller = 1;
        f.caller = caller;
        emit_probe(probe, "timer_registration", target, f);
        if (caller == p->timer_2019_caller_pc ||
            caller == p->timer_201b_caller_pc) {
            f.has_event = 1;
            f.event_id = caller == p->timer_2019_caller_pc
                       ? 0x2019 : 0x201B;
            emit_receiver_probe(probe, "timer_schedule", target, f);
        }
    } else if (target == p->lower_dispatch_pc) {
        emit_receiver_probe(
            probe, "lower_dispatch", target,
            (probe_fields_t){.has_caller = 1, .caller = caller});
    } else if (target == p->service_adapter_pc) {
        uint16_t selector = cemu_cpu_gpr(probe->cpu, 12);
        emit_receiver_probe(probe, "service_adapter", target, (probe_fields_t){
            .has_caller = 1, .caller = caller,
            .has_selector = 1, .selector = selector,
            .has_value = 1, .value = selector,
        });
    } else if (target == p->enqueue_pc) {
        observe_enqueue(probe, caller, target);
    } else if (target == p->dequeue_pc) {
        uint16_t offset = cemu_cpu_gpr(probe->cpu, 12);
        uint16_t segment = cemu_cpu_gpr(probe->cpu, 13);
        probe->dequeue_pending = offset == p->current_signal_offset &&
                                 segment == p->current_signal_segment;
    } else if (target == p->acknowledge_pc &&
               probe->receiver_message_active) {
        probe_fields_t f = current_signal_fields(probe, 1);
        f.has_caller = 1;
        f.caller = caller;
        emit_receiver_probe(probe, "queue_ack", target, f);
        probe->receiver_message_active = 0;
    } else if (target == p->receiver_dispatcher_pc) {
        probe_fields_t f = current_signal_fields(probe, 0);
        f.has_caller = 1;
        f.caller = caller;
        emit_receiver_probe(probe, "receiver_table_wrapper", target, f);
        if (f.normalized_event == 0 ||
            f.normalized_event == p->preexisting_startup_event)
            emit_receiver_probe(probe, "receiver_startup_unhandled",
                                target, f);
    } else if (target == p->event_0006_handler_pc) {
        observe_handler(probe, "receiver_event_0006", caller, target);
    } else if (target == p->event_002d_handler_pc) {
        observe_handler(probe, "receiver_event_002d", caller, target);
    } else if (target == p->event_0039_handler_pc) {
        observe_handler(probe, "candidate_object", caller, target);
    } else if (target == p->event_0040_handler_pc) {
        observe_handler(probe, "receiver_event_0040", caller, target);
    } else if (target == p->strength_setter_pc) {
        uint16_t value = cemu_cpu_gpr(probe->cpu, 12);
        uint16_t object_offset = cemu_memory_controller_peek16(
            &probe->soc->memory, p->candidate_ptr_addr);
        uint16_t object_segment = cemu_memory_controller_peek16(
            &probe->soc->memory, p->candidate_seg_addr);
        emit_receiver_probe(probe, "strength_publish", target, (probe_fields_t){
            .has_caller = 1, .caller = caller,
            .has_object = object_offset || object_segment,
            .object_offset = object_offset, .object_segment = object_segment,
            .has_value = 1, .value = value,
        });
    }
}

static const gsm_l1_probe_profile_t *resolve_profile(
    soc_t *soc, const device_config_t *cfg, const cemu_diagnostic_image_t *image,
    char *error, size_t error_cap) {
    for (size_t i = 0; i < ARRAY_LEN(PROFILES); i++) {
        const gsm_l1_probe_profile_t *p = &PROFILES[i];
        if (strcmp(cfg->name, p->device) || strcmp(image->model, p->model) ||
            image->software_version != p->software_version)
            continue;
        for (size_t signature = 0; signature < p->signature_count;
             signature++) {
            const firmware_signature_t *s = &p->signatures[signature];
            for (uint8_t byte = 0; byte < s->length; byte++) {
                uint8_t actual = cemu_memory_controller_peek8(
                    &soc->memory, s->pc + byte);
                if (actual == s->bytes[byte]) continue;
                snprintf(error, error_cap,
                         "GSM/L1 trace profile signature mismatch for %s at %#x",
                         p->name, s->pc);
                return NULL;
            }
        }
        return p;
    }
    if (error && error_cap) error[0] = 0;
    return NULL;
}

int gsm_l1_probe_attach(gsm_l1_probe_t *probe, soc_t *soc, cpu_t *cpu,
                        const device_config_t *cfg,
                        const cemu_diagnostic_image_t *image,
                        char *error, size_t error_cap) {
    if (!probe || !soc || !cpu || !cfg || !image) return -1;
    memset(probe, 0, sizeof *probe);
    const gsm_l1_probe_profile_t *profile =
        resolve_profile(soc, cfg, image, error, error_cap);
    if (!profile) return 1;
    probe->soc = soc;
    probe->cpu = cpu;
    probe->profile = profile;
    probe->subscription = cemu_event_subscribe(
        &soc->instrumentation,
        (instrumentation_event_mask_t)(CEMU_EVENT_INSTRUCTION |
                                       CEMU_EVENT_CONTROL),
        probe_event, probe);
    if (!probe->subscription) {
        snprintf(error, error_cap, "cannot attach GSM/L1 firmware trace observer");
        memset(probe, 0, sizeof *probe);
        return -1;
    }
    if (error && error_cap) error[0] = 0;
    return 0;
}

void gsm_l1_probe_detach(gsm_l1_probe_t *probe) {
    if (!probe || !probe->subscription) return;
    cemu_event_unsubscribe(&probe->soc->instrumentation,
                                probe->subscription);
    memset(probe, 0, sizeof *probe);
}
