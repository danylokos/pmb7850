#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "emu_cemu_adapter.h"
#include "emu_engine.h"
#include "emu_identity.h"
#include "emu_patch.h"
#include "emu_product.h"

typedef struct {
    size_t frames;
    size_t events;
    emu_frame_kind_t frame_kind;
    emu_event_kind_t event_kind;
    uint64_t frame_icount;
    uint64_t event_icount;
    size_t audio_resets;
    emu_audio_reset_reason_t audio_reset_reason;
} adapter_callbacks_t;

static void adapter_frame(void *opaque, const emu_frame_t *frame) {
    adapter_callbacks_t *callbacks = opaque;
    EMU_CHECK(frame->sequence > callbacks->frames);
    EMU_CHECK(frame->rgb && frame->rgb_size ==
              (size_t)frame->width * frame->height * 3u);
    callbacks->frames++;
    callbacks->frame_kind = frame->kind;
    callbacks->frame_icount = frame->icount;
}

static void adapter_event(void *opaque, const emu_event_t *event) {
    adapter_callbacks_t *callbacks = opaque;
    EMU_CHECK(event->sequence > callbacks->events);
    EMU_CHECK(event->field_count == 2);
    EMU_CHECK(event->fields[0].kind == EMU_EVENT_VALUE_STRING);
    EMU_CHECK(!strcmp(event->fields[0].value.string, "cemu"));
    callbacks->events++;
    callbacks->event_kind = event->kind;
    callbacks->event_icount = event->icount;
}

static void adapter_audio_reset(void *opaque, const emu_audio_reset_t *reset) {
    adapter_callbacks_t *callbacks = opaque;
    callbacks->audio_resets++;
    callbacks->audio_reset_reason = reset->reason;
}

static void rehash_prepared(emu_prepared_session_t *prepared) {
    emu_sha256(prepared->source.bytes, prepared->source.size,
               prepared->source.sha256);
    emu_sha256_hex(prepared->source.sha256,
                   prepared->source.sha256_hex);
    for (size_t i = 0; i < prepared->chip_count; i++) {
        emu_sha256(prepared->source.bytes + prepared->chips[i].source_offset,
                   prepared->chips[i].size, prepared->chips[i].sha256);
        emu_sha256_hex(prepared->chips[i].sha256,
                       prepared->chips[i].sha256_hex);
    }
}

static void test_storage_rejection(const emu_engine_registry_t *registry,
                                   emu_prepared_session_t *prepared,
                                   emu_error_t *error) {
    size_t offset = 0x100;
    uint8_t current = prepared->source.bytes[offset];
    uint8_t changed = (uint8_t)(current ^ 0x5A);
    uint8_t impossible = (uint8_t)(current ^ 0xA5);
    size_t group = 1;
    EMU_CHECK(emu_prepared_add_grouped_storage_operation(
                  prepared, EMU_STORAGE_STAGE_PRE_RESET, group, 0,
                  EMU_STORAGE_MAIN_ARRAY, offset, &current, 1, &changed, 1,
                  "mixed-original", error) == EMU_OK);
    EMU_CHECK(emu_prepared_add_grouped_storage_operation(
                  prepared, EMU_STORAGE_STAGE_PRE_RESET, group, 0,
                  EMU_STORAGE_MAIN_ARRAY, offset + 1u, &impossible, 1,
                  prepared->source.bytes + offset + 1u, 1,
                  "mixed-replaced", error) == EMU_OK);
    emu_session_t *session = NULL;
    EMU_CHECK(emu_session_create(registry, "cemu", prepared, NULL,
                                 &session, error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(session == NULL);
    EMU_CHECK(strstr(error->message, "partially applied") != NULL);
    EMU_CHECK(prepared->source.bytes[offset] == current);
}

static void test_storage_spaces(const emu_engine_registry_t *registry,
                                const char *path, const char *device,
                                int with_patch, emu_error_t *error) {
    emu_prepared_session_t prepared;
    emu_prepared_init(&prepared);
    EMU_CHECK(emu_prepared_load_source(&prepared, path, error) == EMU_OK);
    EMU_CHECK(emu_product_prepare_image(&prepared, device, error) == EMU_OK);
    emu_run_result_t baseline;
    EMU_CHECK(emu_cemu_raw_run(&prepared, 1, &baseline, error) == EMU_OK);

    emu_identity_plan_result_t identity;
    /* Exercise actual grouped writes even when the input already contains
     * the default synthesized identity. These deliberately altered payloads
     * are an in-memory storage fixture, not a bootable identity bundle. */
    emu_identity_bundle_t fixture_identity = *emu_identity_default_bundle();
    for (size_t i = 0; i < EMU_IDENTITY_BLOCK_COUNT; i++) {
        if (fixture_identity.blocks[i].length)
            fixture_identity.blocks[i].bytes[0] ^= 0x5Au;
    }
    EMU_CHECK(emu_identity_plan_bundle(&prepared, &fixture_identity,
                  "storage-test-fixture", 0, &identity, error) == EMU_OK);
    size_t identity_operations = prepared.operation_count;
    EMU_CHECK(identity_operations >= 2);
    for (size_t i = 0; i < identity_operations; i++) {
        EMU_CHECK(prepared.operations[i].stage ==
                  EMU_STORAGE_STAGE_PRE_RESET);
        EMU_CHECK(prepared.operations[i].group == 1);
    }
    if (with_patch) {
        emu_patch_plan_result_t patch;
        EMU_CHECK(emu_patch_plan(&prepared, 1, &patch, error) == EMU_OK);
        EMU_CHECK(patch.applicable == 1);
        EMU_CHECK(prepared.operations[identity_operations].stage ==
                  EMU_STORAGE_STAGE_POST_RESTORE);
        EMU_CHECK(prepared.operations[identity_operations].group == 2);
    }

    emu_session_t *session = NULL;
    const emu_storage_operation_t *first = &prepared.operations[0];
    EMU_CHECK(first->space == EMU_STORAGE_MAIN_ARRAY);
    EMU_CHECK(first->expected_size == first->replacement_size);
    uint8_t *target = prepared.source.bytes +
                      prepared.chips[first->chip_index].source_offset +
                      first->offset;
    uint8_t *saved = malloc(first->expected_size);
    EMU_CHECK(saved != NULL);
    memcpy(saved, target, first->expected_size);
    memcpy(target, first->replacement, first->replacement_size);
    rehash_prepared(&prepared);
    EMU_CHECK(emu_session_create(registry, "cemu", &prepared, NULL,
                                 &session, error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(session == NULL);
    EMU_CHECK(strstr(error->message, "partially applied") != NULL);

    memcpy(target, saved, first->expected_size);
    target[0] ^= 0x7Fu;
    if (target[0] == first->replacement[0]) target[0] ^= 0x40u;
    rehash_prepared(&prepared);
    EMU_CHECK(emu_session_create(registry, "cemu", &prepared, NULL,
                                 &session, error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(session == NULL);
    EMU_CHECK(strstr(error->message, "unexpected current bytes") != NULL);
    memcpy(target, saved, first->expected_size);
    free(saved);
    rehash_prepared(&prepared);

    unsigned affected_mask = 0;
    for (size_t i = 0; i < prepared.operation_count; i++)
        affected_mask |= 1u << prepared.operations[i].chip_index;
    int affected_count = 0;
    for (size_t i = 0; i < prepared.chip_count; i++)
        affected_count += !!(affected_mask & (1u << i));
    emu_cemu_test_fail_storage_copy_after(affected_count > 1 ? 1 : 0);
    EMU_CHECK(emu_session_create(registry, "cemu", &prepared, NULL,
                                 &session, error) == EMU_ERR_NOMEM);
    EMU_CHECK(session == NULL);
    EMU_CHECK(strstr(error->message, "clone CEMU storage") != NULL);
    EMU_CHECK(emu_prepared_rehash_source(&prepared, error) == EMU_OK);

    EMU_CHECK(emu_session_create(registry, "cemu", &prepared, NULL,
                                 &session, error) == EMU_OK);
    emu_run_result_t initialized;
    EMU_CHECK(emu_session_run(session, 1, &initialized, error) == EMU_OK);
    EMU_CHECK(initialized.digest != baseline.digest);
    EMU_CHECK(emu_session_reset(session, error) == EMU_OK);
    emu_engine_state_t reset;
    EMU_CHECK(emu_session_query(session, &reset, error) == EMU_OK);
    EMU_CHECK(reset.icount == 0);
    emu_session_destroy(&session);
    EMU_CHECK(emu_prepared_rehash_source(&prepared, error) == EMU_OK);
    emu_prepared_free(&prepared);
}

int main(int argc, char **argv) {
    EMU_CHECK(argc == 4);
    EMU_CHECK(emu_cemu_engine_descriptor.diagnostics != NULL);
    EMU_CHECK(!strcmp(emu_cemu_engine_descriptor.diagnostics->engine_name,
                      "cemu"));
    EMU_CHECK(!strcmp(emu_cemu_engine_descriptor.diagnostics->profile_name,
                      "plain"));
    EMU_CHECK(emu_cemu_engine_descriptor.diagnostics->capabilities ==
              EMU_DIAG_SNAPSHOT_RESTORE);
    EMU_CHECK(emu_cemu_engine_descriptor.diagnostics->synthetic_parse != NULL);

    emu_prepared_session_t prepared;
    emu_prepared_init(&prepared);
    emu_error_t error = {0};
    EMU_CHECK(emu_prepared_load_source(&prepared, argv[1], &error) == EMU_OK);
    EMU_CHECK(emu_product_prepare_image(&prepared, NULL, &error) == EMU_OK);
    EMU_CHECK(!strcmp(prepared.selected_device, "c55"));
    EMU_CHECK(prepared.chip_count == 1);

    emu_engine_registry_t registry;
    EMU_CHECK(emu_registry_init(&registry, &error) == EMU_OK);
    EMU_CHECK(registry.count == 1);
    EMU_CHECK(!strcmp(registry.items[0]->name, "cemu"));
    EMU_CHECK(registry.items[0]->capabilities & EMU_CAP_STORAGE_INIT);
    EMU_CHECK(registry.items[0]->capabilities & EMU_CAP_KEY_SAMPLING);
    EMU_CHECK(registry.items[0]->capabilities & EMU_CAP_SERIAL_LINK);
    EMU_CHECK(registry.items[0]->capabilities & EMU_CAP_ARTIFACT_REQ);
    EMU_CHECK(registry.items[0]->capabilities & EMU_CAP_AUDIO);

    /* Structurally valid host input must still agree with private CEMU board
     * wiring. The adapter rejects it without inspecting source bytes. */
    emu_prepared_session_t conflicting = prepared;
    snprintf(conflicting.chips[0].role, sizeof conflicting.chips[0].role,
             "secondary");
    emu_session_t *session = NULL;
    EMU_CHECK(emu_session_create(&registry, "cemu", &conflicting, NULL,
                                 &session, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(session == NULL);
    EMU_CHECK(strstr(error.message, "board wiring") != NULL);

    adapter_callbacks_t callbacks = {0};
    emu_callbacks_t callback_set = {
        .opaque = &callbacks,
        .frame = adapter_frame,
        .event = adapter_event,
        .audio_reset = adapter_audio_reset,
    };
    EMU_CHECK(emu_session_create(&registry, "cemu", &prepared, &callback_set,
                                 &session, &error) == EMU_OK);
    EMU_CHECK(callbacks.frames == 0 && callbacks.events == 0);
    EMU_CHECK(emu_session_snapshot(session, "unused", 0, &error) ==
              EMU_ERR_UNSUPPORTED);
    EMU_CHECK(emu_session_monitor_start(session, 16, 1000, &error) ==
              EMU_ERR_UNSUPPORTED);

    emu_engine_state_t reset;
    EMU_CHECK(emu_session_query(session, &reset, &error) == EMU_OK);
    EMU_CHECK(reset.pc == 0 && reset.icount == 0 && reset.audio_available);

    emu_run_result_t hosted;
    emu_run_result_t raw;
    emu_event_selection_t selection = {
        .kind_mask = EMU_EVENT_MASK(EMU_EVENT_SLICE),
    };
    EMU_CHECK(emu_session_select_events(session, &selection, &error) == EMU_OK);
    EMU_CHECK(emu_session_run(session, 5, &hosted, &error) == EMU_OK);
    EMU_CHECK(callbacks.frames == 1 && callbacks.events == 1);
    EMU_CHECK(callbacks.frame_kind == EMU_FRAME_DISPLAY);
    EMU_CHECK(callbacks.event_kind == EMU_EVENT_SLICE);
    EMU_CHECK(callbacks.frame_icount == 5 && callbacks.event_icount == 5);
    EMU_CHECK(emu_cemu_raw_run(&prepared, 5, &raw, &error) == EMU_OK);
    EMU_CHECK(!memcmp(&hosted, &raw, sizeof hosted));

    size_t callback_count = callbacks.frames + callbacks.events;
    EMU_CHECK(emu_session_key(session, "power", 1, &error) == EMU_OK);
    emu_key_sample_t key;
    EMU_CHECK(emu_session_key_query(session, "power", &key, &error) == EMU_OK);
    EMU_CHECK(key.requested_pressed && key.icount == 5);
    EMU_CHECK(emu_session_key(session, "power", 0, &error) == EMU_OK);
    EMU_CHECK(emu_session_key_query(session, "power", &key, &error) == EMU_OK);
    EMU_CHECK(!key.requested_pressed);
    EMU_CHECK(emu_session_serial_rx(
                  session, (const uint8_t *)"A", 1, &error) == EMU_OK);
    emu_serial_link_state_t link;
    EMU_CHECK(emu_session_serial_link(
                  session, EMU_SERIAL_LINK_HOST, &link, &error) == EMU_OK);
    EMU_CHECK(link.available && link.attachment == EMU_SERIAL_LINK_HOST);
    EMU_CHECK(link.rx_bytes == 1);
    EMU_CHECK(callbacks.frames + callbacks.events == callback_count);

    selection.kind_mask = EMU_EVENT_MASK(EMU_EVENT_ARTIFACT);
    EMU_CHECK(emu_session_select_events(session, &selection, &error) == EMU_OK);
    emu_artifact_request_t artifact = {
        EMU_ARTIFACT_LCD_DDRAM, EMU_ARTIFACT_CAPTURE, NULL,
    };
    emu_artifact_result_t artifact_result;
    EMU_CHECK(emu_session_artifact(
                  session, &artifact, &artifact_result, &error) == EMU_OK);
    EMU_CHECK(artifact_result.kind == EMU_ARTIFACT_LCD_DDRAM);
    EMU_CHECK(artifact_result.icount == 5);
    EMU_CHECK(callbacks.frames == 2 && callbacks.events == 2);
    EMU_CHECK(callbacks.frame_kind == EMU_FRAME_DDRAM);
    EMU_CHECK(callbacks.event_kind == EMU_EVENT_ARTIFACT);
    callback_count = callbacks.frames + callbacks.events;
    artifact.kind = EMU_ARTIFACT_TRACE;
    artifact.action = EMU_ARTIFACT_START;
    artifact.path = "trace.parquet";
    EMU_CHECK(emu_session_artifact(
                  session, &artifact, &artifact_result, &error) ==
              EMU_ERR_UNSUPPORTED);
    EMU_CHECK(callbacks.frames + callbacks.events == callback_count);
    EMU_CHECK(emu_session_serial_link(
                  session, EMU_SERIAL_LINK_DETACHED, &link, &error) == EMU_OK);

    emu_run_request_t request = {
        .tick_budget = 1,
        .allow_idle_batch = 1,
        .compute_digest = 0,
    };
    emu_run_result_t request_result;
    EMU_CHECK(emu_session_run_request(
                  session, &request, &request_result, &error) == EMU_OK);
    EMU_CHECK(request_result.ticks == 1 && request_result.digest == 0);
    emu_engine_state_t cheap_state, digest_state;
    EMU_CHECK(emu_session_query_request(
                  session, 0, &cheap_state, &error) == EMU_OK);
    EMU_CHECK(cheap_state.digest == 0);
    EMU_CHECK(emu_session_query(
                  session, &digest_state, &error) == EMU_OK);
    EMU_CHECK(digest_state.digest != 0);
    request.tick_budget = 0;
    EMU_CHECK(emu_session_run_request(
                  session, &request, &request_result, &error) ==
              EMU_ERR_ARGUMENT);

    EMU_CHECK(emu_session_reset(session, &error) == EMU_OK);
    EMU_CHECK(callbacks.audio_resets == 1 &&
              callbacks.audio_reset_reason == EMU_AUDIO_RESET_ENGINE);
    emu_engine_state_t reset_again;
    EMU_CHECK(emu_session_query(session, &reset_again, &error) == EMU_OK);
    EMU_CHECK(reset.pc == reset_again.pc);
    EMU_CHECK(reset.icount == reset_again.icount);
    EMU_CHECK(reset.digest == reset_again.digest);
    EMU_CHECK(emu_session_request_stop(session, &error) == EMU_OK);
    EMU_CHECK(callbacks.audio_resets == 2 &&
              callbacks.audio_reset_reason == EMU_AUDIO_RESET_STOP);

    emu_session_destroy(&session);
    EMU_CHECK(emu_prepared_rehash_source(&prepared, &error) == EMU_OK);
    emu_prepared_free(&prepared);

    emu_prepared_init(&prepared);
    EMU_CHECK(emu_prepared_load_source(&prepared, argv[1], &error) == EMU_OK);
    EMU_CHECK(emu_product_prepare_image(&prepared, NULL, &error) == EMU_OK);
    test_storage_rejection(&registry, &prepared, &error);
    emu_prepared_free(&prepared);

    test_storage_spaces(&registry, argv[2], "c55", 1, &error);
    test_storage_spaces(&registry, argv[3], "s55", 0, &error);
    puts("CEMU adapter parity and atomic storage plans: PASS");
    return 0;
}
