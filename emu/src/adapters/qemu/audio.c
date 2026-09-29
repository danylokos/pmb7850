#include "emu_qemu_audio.h"
#include "emu_qemu_display.h"

#include <stdio.h>
#include <string.h>

static const char audio_xml[] =
    "<node><interface name='org.qemu.Display1.AudioOutListener'>"
    "<method name='Init'><arg type='t' direction='in'/><arg type='y' direction='in'/>"
    "<arg type='b' direction='in'/><arg type='b' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='y' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='b' direction='in'/></method>"
    "<method name='Fini'><arg type='t' direction='in'/></method>"
    "<method name='SetEnabled'><arg type='t' direction='in'/><arg type='b' direction='in'/></method>"
    "<method name='SetVolume'><arg type='t' direction='in'/><arg type='b' direction='in'/>"
    "<arg type='ay' direction='in'/></method>"
    "<method name='Write'><arg type='t' direction='in'/><arg type='ay' direction='in'/></method>"
    "<property name='Interfaces' type='as' access='read'/></interface></node>";

static emu_error_code_t fail(emu_qemu_audio_t *audio, emu_error_t *error,
                              const char *message) {
    audio->failed = 1;
    snprintf(audio->failure, sizeof audio->failure, "%s", message);
    if (error) {
        error->code = EMU_ERR_ENGINE;
        snprintf(error->message, sizeof error->message, "%s", message);
    }
    return EMU_ERR_ENGINE;
}

static void reset(emu_qemu_audio_t *audio) {
    if (audio->callbacks.audio_reset) {
        emu_audio_reset_t event = {.reason = EMU_AUDIO_RESET_BACKEND,
                                   .icount = EMU_AUDIO_TIME_UNKNOWN};
        audio->callbacks.audio_reset(audio->callbacks.opaque, &event);
    }
}

void emu_qemu_audio_init(emu_qemu_audio_t *audio,
                         const emu_callbacks_t *callbacks) {
    memset(audio, 0, sizeof *audio);
    if (callbacks) audio->callbacks = *callbacks;
}

emu_error_code_t emu_qemu_audio_method(emu_qemu_audio_t *audio,
        const char *method, GVariant *parameters, emu_error_t *error) {
    guint64 id;
    GVariant *payload = NULL;
    if (audio->failed) {
        if (error) {
            error->code = EMU_ERR_ENGINE;
            snprintf(error->message, sizeof error->message, "%s", audio->failure);
        }
        return EMU_ERR_ENGINE;
    }
    if (!strcmp(method, "Init")) {
        guchar bits, channels;
        gboolean is_signed, is_float, be;
        guint rate, frame_bytes, second_bytes;
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(tybbuyuub)")))
            return fail(audio, error, "invalid QEMU audio Init signature");
        g_variant_get(parameters, "(tybbuyuub)", &id, &bits, &is_signed,
                      &is_float, &rate, &channels, &frame_bytes, &second_bytes, &be);
        if (bits != 16 || !is_signed || is_float || rate != 48000 ||
            channels != 1 || frame_bytes != 2 || second_bytes != 96000 || be)
            return fail(audio, error, "QEMU audio must be 48000 Hz mono S16LE");
        if (audio->initialized)
            return fail(audio, error, "QEMU audio opened a second stream");
        reset(audio);
        audio->stream = id;
        audio->initialized = 1;
        audio->enabled = 0;
        return EMU_OK;
    }
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE_TUPLE) ||
        g_variant_n_children(parameters) < 1)
        return fail(audio, error, "invalid QEMU audio method signature");
    GVariant *first = g_variant_get_child_value(parameters, 0);
    int valid_id = g_variant_is_of_type(first, G_VARIANT_TYPE_UINT64);
    id = valid_id ? g_variant_get_uint64(first) : 0;
    g_variant_unref(first);
    if (!valid_id || !audio->initialized || id != audio->stream)
        return fail(audio, error, "QEMU audio references an unknown stream");
    if (!strcmp(method, "Fini") &&
        g_variant_is_of_type(parameters, G_VARIANT_TYPE("(t)"))) {
        reset(audio);
        audio->initialized = audio->enabled = 0;
    } else if (!strcmp(method, "SetEnabled") &&
               g_variant_is_of_type(parameters, G_VARIANT_TYPE("(tb)"))) {
        gboolean enabled;
        g_variant_get(parameters, "(tb)", &id, &enabled);
        if (!enabled && audio->enabled) reset(audio);
        audio->enabled = enabled;
    } else if (!strcmp(method, "SetVolume") &&
               g_variant_is_of_type(parameters, G_VARIANT_TYPE("(tbay)"))) {
        /* The model has already applied source gains and final clipping. */
    } else if (!strcmp(method, "Write") &&
               g_variant_is_of_type(parameters, G_VARIANT_TYPE("(tay)"))) {
        gsize size;
        g_variant_get(parameters, "(t@ay)", &id, &payload);
        const uint8_t *bytes = g_variant_get_fixed_array(payload, &size, 1);
        if (!audio->enabled || !size || (size & 1) || size > 512) {
            g_variant_unref(payload);
            return fail(audio, error, "invalid QEMU final-speaker PCM block");
        }
        int16_t pcm[256];
        for (size_t i = 0; i < size / 2; i++)
            pcm[i] = (int16_t)((uint16_t)bytes[2 * i] | (uint16_t)bytes[2 * i + 1] << 8);
        emu_audio_output_t event = {.completion_tick = EMU_AUDIO_TIME_UNKNOWN,
                                    .samples = pcm, .frame_count = size / 2};
        if (audio->callbacks.audio_output)
            audio->callbacks.audio_output(audio->callbacks.opaque, &event);
        audio->frames += size / 2;
        g_variant_unref(payload);
    } else {
        return fail(audio, error, "unknown QEMU audio method or signature");
    }
    return EMU_OK;
}

static void method_call(GDBusConnection *connection, const gchar *sender,
        const gchar *path, const gchar *interface, const gchar *method,
        GVariant *parameters, GDBusMethodInvocation *invocation, gpointer opaque) {
    (void)connection; (void)sender; (void)path; (void)interface;
    emu_error_t error = {0};
    if (emu_qemu_audio_method(opaque, method, parameters, &error) != EMU_OK)
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.qemu.Display1.Error.Unsupported", error.message);
    else
        g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *get_property(GDBusConnection *connection, const gchar *sender,
        const gchar *path, const gchar *interface, const gchar *property,
        GError **error, gpointer opaque) {
    (void)connection; (void)sender; (void)path; (void)interface; (void)error;
    emu_qemu_audio_t *audio = opaque;
    if (!strcmp(property, "Interfaces")) {
        audio->ready = 1;
        return g_variant_new_strv(NULL, 0);
    }
    return NULL;
}

static const GDBusInterfaceVTable vtable = {
    .method_call = method_call, .get_property = get_property,
};

emu_error_code_t emu_qemu_audio_attach(emu_qemu_audio_t *audio,
        GDBusConnection *control, emu_error_t *error) {
    GError *ge = NULL;
    audio->listener = emu_qemu_peer_listener(control, "/org/qemu/Display1/Audio",
        "org.qemu.Display1.Audio", "RegisterOutListener", &ge);
    if (!audio->listener) goto failure;
    audio->info = g_dbus_node_info_new_for_xml(audio_xml, &ge);
    if (!audio->info) goto failure;
    audio->registration = g_dbus_connection_register_object(audio->listener,
        "/org/qemu/Display1/AudioOutListener", audio->info->interfaces[0],
        &vtable, audio, NULL, &ge);
    if (!audio->registration) goto failure;
    g_dbus_connection_start_message_processing(audio->listener);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while ((!audio->ready || !audio->initialized) && !audio->failed &&
           g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
    if (!audio->ready || !audio->initialized || audio->failed) {
        if (!audio->failed) fail(audio, error, "QEMU audio listener negotiation timed out");
        else if (error) {
            error->code = EMU_ERR_ENGINE;
            snprintf(error->message, sizeof error->message, "%s", audio->failure);
        }
        return EMU_ERR_ENGINE;
    }
    return EMU_OK;
failure:
    fail(audio, error, ge ? ge->message : "cannot attach QEMU audio listener");
    g_clear_error(&ge);
    return EMU_ERR_ENGINE;
}

emu_error_code_t emu_qemu_audio_pump(emu_qemu_audio_t *audio, emu_error_t *error) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    if (audio->listener && g_dbus_connection_is_closed(audio->listener) && !audio->failed) {
        reset(audio);
        return fail(audio, error, "QEMU audio listener disconnected");
    }
    if (audio->failed) {
        if (error) {
            error->code = EMU_ERR_ENGINE;
            snprintf(error->message, sizeof error->message, "%s", audio->failure);
        }
        return EMU_ERR_ENGINE;
    }
    return EMU_OK;
}

void emu_qemu_audio_destroy(emu_qemu_audio_t *audio) {
    if (audio->initialized) reset(audio);
    if (audio->listener && audio->registration)
        g_dbus_connection_unregister_object(audio->listener, audio->registration);
    if (audio->listener) g_dbus_connection_close_sync(audio->listener, NULL, NULL);
    g_clear_object(&audio->listener);
    g_clear_pointer(&audio->info, g_dbus_node_info_unref);
    memset(audio, 0, sizeof *audio);
}
