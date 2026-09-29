#include <string.h>

#include "fake_engine.h"
#include "test_support.h"

typedef struct {
    char order[64];
    size_t count;
    emu_frame_kind_t last_frame_kind;
    uint64_t last_frame_sequence;
    uint64_t last_frame_icount;
    emu_event_kind_t last_event_kind;
    uint64_t last_event_sequence;
    uint64_t last_event_icount;
} callbacks_t;

static void frame_callback(void *opaque, const emu_frame_t *frame) {
    callbacks_t *callbacks = opaque;
    EMU_CHECK(frame->rgb_size == 3);
    EMU_CHECK(frame->width == 1 && frame->height == 1);
    EMU_CHECK(frame->sequence > callbacks->last_frame_sequence);
    callbacks->last_frame_kind = frame->kind;
    callbacks->last_frame_sequence = frame->sequence;
    callbacks->last_frame_icount = frame->icount;
    callbacks->order[callbacks->count++] = 'F';
}

static void serial_callback(void *opaque, const uint8_t *data, size_t size) {
    callbacks_t *callbacks = opaque;
    EMU_CHECK(size > 0 && data);
    callbacks->order[callbacks->count++] = 'S';
}

static void event_callback(void *opaque, const emu_event_t *event) {
    callbacks_t *callbacks = opaque;
    EMU_CHECK(event->sequence > callbacks->last_event_sequence);
    EMU_CHECK(event->field_count == 2);
    EMU_CHECK(!strcmp(event->fields[0].name, "engine"));
    EMU_CHECK(event->fields[0].kind == EMU_EVENT_VALUE_STRING);
    EMU_CHECK(!strcmp(event->fields[0].value.string, "fake"));
    callbacks->last_event_kind = event->kind;
    callbacks->last_event_sequence = event->sequence;
    callbacks->last_event_icount = event->icount;
    callbacks->order[callbacks->count++] = 'E';
}

int main(void) {
    emu_prepared_session_t prepared;
    emu_test_prepared(&prepared);
    emu_error_t error = {0};
    uint8_t expect = 2, replacement = 9;
    uint8_t expect_second = 9, replacement_second = 10;
    EMU_CHECK(emu_prepared_add_storage_operation(
                  &prepared, 0, EMU_STORAGE_MAIN_ARRAY, 2,
                  &expect, 1, &replacement, 1, "contract-first",
                  &error) == EMU_OK);
    EMU_CHECK(emu_prepared_add_storage_operation(
                  &prepared, 0, EMU_STORAGE_MAIN_ARRAY, 2,
                  &expect_second, 1, &replacement_second, 1,
                  "contract-second", &error) == EMU_OK);

    emu_engine_registry_t registry = {0};
    EMU_CHECK(emu_registry_register(
                  &registry, &emu_fake_engine_descriptor, &error) == EMU_OK);
    EMU_CHECK(emu_registry_register(
                  &registry, &emu_fake_engine_descriptor, &error) ==
              EMU_ERR_DUPLICATE_ENGINE);
    EMU_CHECK(!emu_registry_find(&registry, "missing"));

    emu_session_t *session = NULL;
    EMU_CHECK(emu_session_create(
                  &registry, "missing", &prepared, NULL, &session, &error) ==
              EMU_ERR_UNKNOWN_ENGINE);
    callbacks_t callbacks = {0};
    emu_callbacks_t callback_set = {
        .opaque = &callbacks,
        .frame = frame_callback,
        .serial = serial_callback,
        .event = event_callback,
    };
    EMU_CHECK(emu_session_create(
                  &registry, "fake", &prepared, &callback_set,
                  &session, &error) == EMU_OK);
    EMU_CHECK(emu_fake_live_sessions() == 1);
    EMU_CHECK(callbacks.count == 0);

    emu_event_selection_t selection = {
        EMU_EVENT_MASK(EMU_EVENT_POLL) |
        EMU_EVENT_MASK(EMU_EVENT_ARTIFACT),
    };
    EMU_CHECK(emu_session_select_events(
                  session, &selection, &error) == EMU_OK);
    EMU_CHECK(callbacks.count == 0);

    EMU_CHECK(emu_session_serial_rx(
                  session, (const uint8_t *)"abc", 3, &error) == EMU_OK);
    EMU_CHECK(callbacks.count == 0);
    emu_run_result_t first, second;
    EMU_CHECK(emu_session_run(session, 3, &first, &error) == EMU_OK);
    EMU_CHECK(first.ticks == 3 && first.guest_instructions == 3 &&
              first.pc == 3);
    EMU_CHECK(callbacks.count == 2);
    EMU_CHECK(!strncmp(callbacks.order, "FS", 2));
    EMU_CHECK(callbacks.last_frame_kind == EMU_FRAME_DISPLAY);
    EMU_CHECK(callbacks.last_frame_icount == 3);
    EMU_CHECK(emu_session_run(session, 4, &second, &error) == EMU_OK);
    EMU_CHECK(second.ticks == 4 && second.end_icount == 7);
    EMU_CHECK(callbacks.count == 3 && callbacks.order[2] == 'F');

    EMU_CHECK(emu_session_key(session, "power", 1, &error) == EMU_OK);
    emu_key_sample_t key;
    EMU_CHECK(emu_session_key_query(
                  session, "power", &key, &error) == EMU_OK);
    EMU_CHECK(key.requested_pressed && !key.sampled_pressed);
    EMU_CHECK(callbacks.count == 3);
    EMU_CHECK(emu_session_poll(session, &error) == EMU_OK);
    EMU_CHECK(callbacks.count == 5);
    EMU_CHECK(callbacks.order[3] == 'F' && callbacks.order[4] == 'E');
    EMU_CHECK(callbacks.last_event_kind == EMU_EVENT_POLL);
    EMU_CHECK(callbacks.last_event_icount == 7);
    EMU_CHECK(emu_session_key_query(
                  session, "power", &key, &error) == EMU_OK);
    EMU_CHECK(key.requested_pressed && key.sampled_pressed);
    EMU_CHECK(emu_session_key(session, "power", 0, &error) == EMU_OK);
    EMU_CHECK(emu_session_key_query(
                  session, "power", &key, &error) == EMU_OK);
    EMU_CHECK(!key.requested_pressed && key.sampled_pressed);
    EMU_CHECK(emu_session_poll(session, &error) == EMU_OK);
    EMU_CHECK(emu_session_key_query(
                  session, "power", &key, &error) == EMU_OK);
    EMU_CHECK(!key.requested_pressed && !key.sampled_pressed);

    emu_serial_link_state_t link;
    EMU_CHECK(emu_session_serial_link(
                  session, EMU_SERIAL_LINK_UI, &link, &error) == EMU_OK);
    EMU_CHECK(link.available && link.attachment == EMU_SERIAL_LINK_UI);
    EMU_CHECK(link.rx_bytes == 3 && link.tx_bytes == 3);
    EMU_CHECK(emu_session_sim(session, 1, &error) == EMU_OK);
    EMU_CHECK(emu_session_battery(session, 50, 1, &error) == EMU_OK);
    EMU_CHECK(emu_session_snapshot(session, "snap", 0, &error) == EMU_OK);
    EMU_CHECK(emu_session_debugger(session, "regs", &error) == EMU_OK);
    EMU_CHECK(emu_session_coverage(session, 1, &error) == EMU_OK);

    emu_artifact_request_t artifact = {
        EMU_ARTIFACT_LCD_DDRAM, EMU_ARTIFACT_CAPTURE, NULL,
    };
    emu_artifact_result_t artifact_result;
    size_t before_artifact = callbacks.count;
    EMU_CHECK(emu_session_artifact(
                  session, &artifact, &artifact_result, &error) == EMU_OK);
    EMU_CHECK(artifact_result.kind == EMU_ARTIFACT_LCD_DDRAM);
    EMU_CHECK(artifact_result.sequence == 1 && artifact_result.icount == 7);
    EMU_CHECK(callbacks.count == before_artifact + 2);
    EMU_CHECK(callbacks.last_frame_kind == EMU_FRAME_DDRAM);
    EMU_CHECK(callbacks.last_event_kind == EMU_EVENT_ARTIFACT);

    before_artifact = callbacks.count;
    emu_fake_fail_next_artifact();
    EMU_CHECK(emu_session_artifact(
                  session, &artifact, &artifact_result, &error) ==
              EMU_ERR_ENGINE);
    EMU_CHECK(callbacks.count == before_artifact);

    selection.kind_mask = 0;
    EMU_CHECK(emu_session_select_events(
                  session, &selection, &error) == EMU_OK);
    size_t before_poll = callbacks.count;
    EMU_CHECK(emu_session_poll(session, &error) == EMU_OK);
    EMU_CHECK(callbacks.count == before_poll + 1);
    EMU_CHECK(callbacks.order[before_poll] == 'F');

    EMU_CHECK(emu_session_request_stop(session, &error) == EMU_OK);
    EMU_CHECK(emu_session_request_stop(session, &error) == EMU_OK);
    EMU_CHECK(emu_session_run(session, 5, &first, &error) == EMU_OK &&
              first.status == EMU_RUN_STOPPED && first.ticks == 0);
    EMU_CHECK(emu_session_run(session, 1, &first, &error) ==
              EMU_ERR_LIFECYCLE);
    EMU_CHECK(emu_session_reset(session, &error) == EMU_OK);
    emu_engine_state_t state;
    EMU_CHECK(emu_session_query(session, &state, &error) == EMU_OK);
    EMU_CHECK(state.icount == 0 && !state.stop_requested);
    emu_session_destroy(&session);
    emu_session_destroy(&session);
    EMU_CHECK(emu_fake_live_sessions() == 0);

    EMU_CHECK(emu_session_run(NULL, 1, &first, &error) ==
              EMU_ERR_LIFECYCLE);
    EMU_CHECK(emu_session_query(NULL, &state, &error) ==
              EMU_ERR_LIFECYCLE);
    EMU_CHECK(emu_session_key_query(NULL, "power", &key, &error) ==
              EMU_ERR_LIFECYCLE);
    EMU_CHECK(emu_session_artifact(NULL, &artifact, &artifact_result,
                                   &error) == EMU_ERR_LIFECYCLE);
    emu_fake_fail_next_create();
    EMU_CHECK(emu_session_create(
                  &registry, "fake", &prepared, NULL, &session, &error) ==
              EMU_ERR_ENGINE);
    EMU_CHECK(!session && emu_fake_live_sessions() == 0);

    snprintf(prepared.selected_device, sizeof prepared.selected_device,
             "other");
    EMU_CHECK(emu_session_create(
                  &registry, "fake", &prepared, NULL, &session, &error) ==
              EMU_ERR_UNSUPPORTED);
    snprintf(prepared.selected_device, sizeof prepared.selected_device,
             "fake");
    prepared.chips[0].size = 17;
    EMU_CHECK(emu_prepared_validate(
                  &prepared, emu_fake_engine_descriptor.capabilities,
                  &error) == EMU_ERR_INVALID_PREPARED_SESSION);
    prepared.chips[0].size = 16;
    EMU_CHECK(emu_prepared_validate(&prepared, 0, &error) ==
              EMU_ERR_UNSUPPORTED);
    emu_prepared_free(&prepared);
    puts("fake engine stream/control contract: PASS");
    return 0;
}
