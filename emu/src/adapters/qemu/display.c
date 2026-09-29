#define _POSIX_C_SOURCE 200809L

#include "emu_qemu_display.h"

#include <errno.h>
#include <pixman.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdlist.h>

static const char listener_xml[] =
    "<node>"
    "<interface name='org.qemu.Display1.Listener'>"
    "<method name='Scanout'><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='u' direction='in'/><arg type='ay' direction='in'/></method>"
    "<method name='Update'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='ay' direction='in'/></method>"
    "<method name='ScanoutDMABUF'><arg type='h' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='u' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "<arg type='t' direction='in'/><arg type='b' direction='in'/></method>"
    "<method name='UpdateDMABUF'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='Disable'/><method name='MouseSet'><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "<method name='CursorDefine'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='ay' direction='in'/></method>"
    "<property name='Interfaces' type='as' access='read'/>"
    "</interface></node>";

static emu_error_code_t display_fail(emu_error_t *error,
                                     emu_error_code_t code,
                                     const char *format, ...) {
    if (error) {
        va_list arguments;
        error->code = code;
        va_start(arguments, format);
        vsnprintf(error->message, sizeof error->message, format, arguments);
        va_end(arguments);
    }
    return code;
}

static void display_ok(emu_error_t *error) {
    if (error) {
        error->code = EMU_OK;
        error->message[0] = 0;
    }
}

void emu_qemu_display_init(emu_qemu_display_t *display,
                           emu_frame_callback_t callback, void *opaque,
                           unsigned width, unsigned height) {
    if (!display) return;
    memset(display, 0, sizeof *display);
    display->callback = callback;
    display->callback_opaque = opaque;
    if (width <= EMU_QEMU_DISPLAY_MAX_WIDTH &&
        height <= EMU_QEMU_DISPLAY_MAX_HEIGHT) {
        display->width = width;
        display->height = height;
    }
}

static int supported_format(unsigned format) {
    return format == PIXMAN_x8r8g8b8 || format == PIXMAN_a8r8g8b8;
}

static void emit_frame(emu_qemu_display_t *display) {
    size_t pixel_count = (size_t)display->width * display->height;
    for (size_t i = 0; i < pixel_count; i++) {
        uint32_t pixel;
        memcpy(&pixel, display->pixels + 4u * i, sizeof pixel);
        display->rgb[3u * i] = (uint8_t)(pixel >> 16);
        display->rgb[3u * i + 1u] = (uint8_t)(pixel >> 8);
        display->rgb[3u * i + 2u] = (uint8_t)pixel;
    }
    if (display->callback) {
        emu_frame_t frame = {
            .kind = EMU_FRAME_DISPLAY,
            .sequence = display->sequence++,
            .icount = display->icount,
            .width = display->width,
            .height = display->height,
            .rgb = display->rgb,
            .rgb_size = pixel_count * 3u,
        };
        display->callback(display->callback_opaque, &frame);
    }
}

emu_error_code_t emu_qemu_display_scanout(
        emu_qemu_display_t *display, unsigned width, unsigned height,
        unsigned stride, unsigned format, const uint8_t *data, size_t size,
        emu_error_t *error) {
    if (!display || !data)
        return display_fail(error, EMU_ERR_ARGUMENT,
                            "invalid QEMU display scanout");
    if (!display->width || !display->height ||
        width != display->width || height != display->height ||
        stride < width * 4u ||
        size < (size_t)stride * height || !supported_format(format))
        return display_fail(error, EMU_ERR_UNSUPPORTED,
                            "unsupported QEMU scanout %ux%u stride=%u format=0x%x",
                            width, height, stride, format);
    for (unsigned y = 0; y < height; y++)
        memcpy(display->pixels + y * width * 4u,
               data + (size_t)y * stride, width * 4u);
    emit_frame(display);
    display->enabled = 1;
    display_ok(error);
    return EMU_OK;
}

emu_error_code_t emu_qemu_display_update(
        emu_qemu_display_t *display, int x, int y, int width, int height,
        unsigned stride, unsigned format, const uint8_t *data, size_t size,
        emu_error_t *error) {
    if (!display || !data || !display->enabled)
        return display_fail(error, EMU_ERR_LIFECYCLE,
                            "QEMU display update precedes scanout");
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x + width > (int)display->width ||
        y + height > (int)display->height ||
        stride < (unsigned)width * 4u ||
        size < (size_t)stride * (unsigned)height ||
        !supported_format(format))
        return display_fail(error, EMU_ERR_UNSUPPORTED,
                            "unsupported QEMU display update");
    for (int row = 0; row < height; row++)
        memcpy(display->pixels +
                   ((size_t)(y + row) * display->width +
                    (unsigned)x) * 4u,
               data + (size_t)row * stride, (size_t)width * 4u);
    emit_frame(display);
    display_ok(error);
    return EMU_OK;
}

static void remember_failure(emu_qemu_display_t *display,
                             const emu_error_t *error) {
    display->failed = 1;
    snprintf(display->failure, sizeof display->failure, "%s",
             error && error->message[0] ? error->message :
             "QEMU D-Bus display failed");
}

static void listener_method_call(GDBusConnection *connection,
                                 const gchar *sender,
                                 const gchar *object_path,
                                 const gchar *interface_name,
                                 const gchar *method_name,
                                 GVariant *parameters,
                                 GDBusMethodInvocation *invocation,
                                 gpointer user_data) {
    emu_qemu_display_t *display = user_data;
    emu_error_t error = {0};
    GVariant *data = NULL;
    gsize size = 0;
    const uint8_t *bytes;
    emu_error_code_t code = EMU_OK;
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    if (!strcmp(method_name, "Scanout")) {
        guint width, height, stride, format;
        g_variant_get(parameters, "(uuuu@ay)", &width, &height, &stride,
                      &format, &data);
        bytes = g_variant_get_fixed_array(data, &size, sizeof(uint8_t));
        code = emu_qemu_display_scanout(display, width, height, stride,
                                        format, bytes, size, &error);
    } else if (!strcmp(method_name, "Update")) {
        gint x, y, width, height;
        guint stride, format;
        g_variant_get(parameters, "(iiiiuu@ay)", &x, &y, &width, &height,
                      &stride, &format, &data);
        bytes = g_variant_get_fixed_array(data, &size, sizeof(uint8_t));
        code = emu_qemu_display_update(display, x, y, width, height,
                                       stride, format, bytes, size, &error);
    } else if (!strcmp(method_name, "Disable")) {
        display->enabled = 0;
    } else if (!strcmp(method_name, "ScanoutDMABUF") ||
               !strcmp(method_name, "UpdateDMABUF")) {
        code = display_fail(&error, EMU_ERR_UNSUPPORTED,
                            "QEMU DMABUF display is unsupported");
    }
    if (data) g_variant_unref(data);
    if (code != EMU_OK) {
        remember_failure(display, &error);
        g_dbus_method_invocation_return_dbus_error(
            invocation, "org.qemu.Display1.Error.Unsupported",
            error.message);
    } else {
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static GVariant *listener_get_property(GDBusConnection *connection,
                                       const gchar *sender,
                                       const gchar *object_path,
                                       const gchar *interface_name,
                                       const gchar *property_name,
                                       GError **error,
                                       gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    emu_qemu_display_t *display = user_data;
    if (!strcmp(property_name, "Interfaces")) {
        display->ready = 1;
        return g_variant_new_strv(NULL, 0);
    }
    return NULL;
}

static const GDBusInterfaceVTable listener_vtable = {
    .method_call = listener_method_call,
    .get_property = listener_get_property,
};

static GDBusConnection *connection_from_fd(int fd,
                                           GDBusConnectionFlags flags,
                                           GError **error) {
    GSocket *socket = g_socket_new_from_fd(fd, error);
    if (!socket) return NULL;
    GSocketConnection *stream =
        g_socket_connection_factory_create_connection(socket);
    g_object_unref(socket);
    GDBusConnection *connection = g_dbus_connection_new_sync(
        G_IO_STREAM(stream), NULL, flags, NULL, NULL, error);
    g_object_unref(stream);
    return connection;
}

typedef struct {
    int fd;
    GDBusConnection *connection;
    GError *error;
    GMutex mutex;
    GCond condition;
    int done;
} listener_connection_request_t;

static gpointer create_listener_connection(gpointer opaque) {
    listener_connection_request_t *request = opaque;
    request->connection = connection_from_fd(
        request->fd, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                     G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING,
        &request->error);
    g_mutex_lock(&request->mutex);
    request->done = 1;
    g_cond_signal(&request->condition);
    g_mutex_unlock(&request->mutex);
    return NULL;
}

GDBusConnection *emu_qemu_peer_listener(
        GDBusConnection *control, const char *path, const char *interface,
        const char *method, GError **error) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot create QEMU listener: %s", strerror(errno));
        return NULL;
    }
    int cancel_fd = dup(pair[0]);
    if (cancel_fd < 0) {
        close(pair[0]); close(pair[1]);
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot retain listener cancellation descriptor");
        return NULL;
    }
    GUnixFDList *fds = g_unix_fd_list_new();
    int handle = g_unix_fd_list_append(fds, pair[1], error);
    close(pair[1]);
    if (handle < 0) {
        close(pair[0]); close(cancel_fd);
        g_object_unref(fds);
        return NULL;
    }
    listener_connection_request_t request = {.fd = pair[0]};
    g_mutex_init(&request.mutex);
    g_cond_init(&request.condition);
    GThread *thread = g_thread_new("emu-qemu-listener",
                                   create_listener_connection, &request);
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
        control, NULL, path, interface, method, g_variant_new("(h)", handle),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, fds, NULL, NULL, error);
    g_object_unref(fds);
    if (!reply) shutdown(cancel_fd, SHUT_RDWR);
    /* Registration success does not guarantee that peer authentication ends. */
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    g_mutex_lock(&request.mutex);
    while (!request.done) {
        if (!g_cond_wait_until(&request.condition, &request.mutex, deadline)) {
            shutdown(cancel_fd, SHUT_RDWR);
            break;
        }
    }
    g_mutex_unlock(&request.mutex);
    g_thread_join(thread);
    g_cond_clear(&request.condition);
    g_mutex_clear(&request.mutex);
    close(cancel_fd);
    if (!reply) {
        g_clear_object(&request.connection);
        g_clear_error(&request.error);
        return NULL;
    }
    g_variant_unref(reply);
    if (!request.connection) g_propagate_error(error, request.error);
    return request.connection;
}

emu_error_code_t emu_qemu_display_attach(emu_qemu_display_t *display,
                                         emu_qmp_t *qmp,
                                         emu_error_t *error) {
    if (!display || !qmp || display->control || display->listener)
        return display_fail(error, EMU_ERR_ARGUMENT,
                            "invalid QEMU display attachment");
    int control_fds[2] = {-1, -1};
    GError *gerror = NULL;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, control_fds))
        return display_fail(error, EMU_ERR_IO,
                            "cannot create QEMU display socket: %s",
                            strerror(errno));
    emu_error_code_t code = emu_qmp_command(
        qmp, "getfd", "{\"fdname\":\"emu-display\"}", control_fds[1],
        NULL, 0, 5000, error);
    close(control_fds[1]);
    control_fds[1] = -1;
    if (code == EMU_OK)
        code = emu_qmp_command(
            qmp, "add_client",
            "{\"protocol\":\"@dbus-display\",\"fdname\":\"emu-display\"}",
            -1, NULL, 0, 5000, error);
    if (code != EMU_OK) goto fail;
    display->control = connection_from_fd(
        control_fds[0], G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
        &gerror);
    control_fds[0] = -1;
    if (!display->control) goto glib_fail;

    display->listener = emu_qemu_peer_listener(display->control,
        "/org/qemu/Display1/Console_0", "org.qemu.Display1.Console",
        "RegisterListener", &gerror);
    if (!display->listener) goto glib_fail;
    display->listener_info = g_dbus_node_info_new_for_xml(listener_xml,
                                                           &gerror);
    if (!display->listener_info) goto glib_fail;
    display->registration = g_dbus_connection_register_object(
        display->listener, "/org/qemu/Display1/Listener",
        display->listener_info->interfaces[0], &listener_vtable,
        display, NULL, &gerror);
    if (!display->registration) goto glib_fail;
    g_dbus_connection_start_message_processing(display->listener);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!display->ready && !display->failed &&
           g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        if (!display->ready) g_usleep(1000);
    }
    if (!display->ready) {
        display_fail(error, EMU_ERR_ENGINE,
                     "QEMU D-Bus listener negotiation timed out");
        goto fail;
    }
    display_ok(error);
    return EMU_OK;

glib_fail:
    display_fail(error, EMU_ERR_ENGINE, "cannot attach QEMU D-Bus display: %s",
                 gerror ? gerror->message : "unknown GDBus failure");
    g_clear_error(&gerror);
fail:
    if (control_fds[0] >= 0) close(control_fds[0]);
    if (control_fds[1] >= 0) close(control_fds[1]);
    emu_qemu_display_destroy(display);
    return error ? error->code : EMU_ERR_ENGINE;
}

emu_error_code_t emu_qemu_display_pump(emu_qemu_display_t *display,
                                       emu_error_t *error) {
    if (!display)
        return display_fail(error, EMU_ERR_ARGUMENT,
                            "invalid QEMU display pump");
    while (g_main_context_iteration(NULL, FALSE)) {}
    if (display->failed)
        return display_fail(error, EMU_ERR_ENGINE, "%s", display->failure);
    display_ok(error);
    return EMU_OK;
}

void emu_qemu_display_destroy(emu_qemu_display_t *display) {
    if (!display) return;
    if (display->listener && display->registration)
        g_dbus_connection_unregister_object(display->listener,
                                            display->registration);
    g_clear_pointer(&display->listener_info, g_dbus_node_info_unref);
    g_clear_object(&display->listener);
    g_clear_object(&display->control);
    memset(display, 0, sizeof *display);
}
