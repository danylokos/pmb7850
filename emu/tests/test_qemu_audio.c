#include <stdio.h>
#include <string.h>
#include "emu_qemu_audio.h"
#include "test_support.h"

typedef struct {
    unsigned calls, resets;
    size_t frames;
    int16_t first, last;
} Capture;

static void output(void *opaque, const emu_audio_output_t *event) {
    Capture *c = opaque;
    EMU_CHECK(event->completion_tick == EMU_AUDIO_TIME_UNKNOWN);
    c->calls++;
    c->frames += event->frame_count;
    c->first = event->samples[0];
    c->last = event->samples[event->frame_count - 1];
}

static void reset(void *opaque, const emu_audio_reset_t *event) {
    Capture *c = opaque;
    EMU_CHECK(event->icount == EMU_AUDIO_TIME_UNKNOWN);
    EMU_CHECK(event->reason == EMU_AUDIO_RESET_BACKEND);
    c->resets++;
}

static emu_error_code_t call(emu_qemu_audio_t *a, const char *method, GVariant *args) {
    emu_error_t error = {0};
    args = g_variant_ref_sink(args);
    emu_error_code_t result = emu_qemu_audio_method(a, method, args, &error);
    g_variant_unref(args);
    return result;
}

static GVariant *init(unsigned rate) {
    return g_variant_new("(tybbuyuub)", (guint64)7, 16, TRUE, FALSE,
                          rate, 1, 2u, 96000u, FALSE);
}

static GVariant *write_block(uint64_t id, size_t size) {
    uint8_t bytes[514] = {0};
    EMU_CHECK(size <= sizeof bytes);
    bytes[1] = 0x80;
    if (size > 2) {
        bytes[size - 2] = 0xff;
        bytes[size - 1] = 0x7f;
    }
    return g_variant_new("(t@ay)", (guint64)id,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes, size, 1));
}

int main(void) {
    emu_qemu_audio_t audio;
    Capture capture = {0};
    emu_callbacks_t callbacks = {.opaque = &capture, .audio_output = output,
                                 .audio_reset = reset};
    emu_qemu_audio_init(&audio, &callbacks);
    EMU_CHECK(call(&audio, "Init", init(48000)) == EMU_OK);
    EMU_CHECK(capture.resets == 1);
    EMU_CHECK(call(&audio, "SetEnabled", g_variant_new("(tb)", (guint64)7, TRUE)) == EMU_OK);
    EMU_CHECK(call(&audio, "Write", write_block(7, 512)) == EMU_OK);
    EMU_CHECK(capture.calls == 1 && capture.frames == 256);
    EMU_CHECK(capture.first == -32768 && capture.last == 32767);
    EMU_CHECK(call(&audio, "Write", write_block(7, 6)) == EMU_OK);
    EMU_CHECK(capture.calls == 2 && capture.frames == 259);
    EMU_CHECK(call(&audio, "Fini", g_variant_new("(t)", (guint64)7)) == EMU_OK);
    EMU_CHECK(capture.resets == 2 && !audio.initialized);
    EMU_CHECK(call(&audio, "Init", init(48000)) == EMU_OK);
    EMU_CHECK(call(&audio, "SetEnabled", g_variant_new("(tb)", (guint64)7, TRUE)) == EMU_OK);
    EMU_CHECK(call(&audio, "Write", write_block(7, 2)) == EMU_OK);
    EMU_CHECK(capture.calls == 3 && capture.frames == 260 && capture.last == -32768);
    emu_qemu_audio_destroy(&audio);
    EMU_CHECK(capture.resets == 4);
    /* Invalid formats, byte counts, IDs and lifecycle order fail closed. */
    for (unsigned scenario = 0; scenario < 8; scenario++) {
        emu_qemu_audio_init(&audio, &callbacks);
        if (scenario == 0) {
            EMU_CHECK(call(&audio, "Init", init(44100)) != EMU_OK);
        } else if (scenario == 1) {
            EMU_CHECK(call(&audio, "Write", write_block(7, 2)) != EMU_OK);
        } else {
            EMU_CHECK(call(&audio, "Init", init(48000)) == EMU_OK);
            if (scenario == 2) {
                EMU_CHECK(call(&audio, "Write", write_block(7, 2)) != EMU_OK);
            } else if (scenario == 3) {
                EMU_CHECK(call(&audio, "Init", init(48000)) != EMU_OK);
            } else {
                EMU_CHECK(call(&audio, "SetEnabled", g_variant_new("(tb)", (guint64)7, TRUE)) == EMU_OK);
                EMU_CHECK(call(&audio, "Write", write_block(scenario == 4 ? 8 : 7,
                    scenario == 5 ? 0 : scenario == 6 ? 3 : 514)) != EMU_OK);
            }
        }
        EMU_CHECK(audio.failed);
        emu_qemu_audio_destroy(&audio);
    }
    puts("QEMU D-Bus final PCM lifecycle: PASS");
    return 0;
}
