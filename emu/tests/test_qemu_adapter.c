#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "test_support.h"
#include "bundled_firmware.h"
#include "emu_prepare.h"
#include "emu_qemu_adapter.h"
#include "emu_qemu_console.h"
#include "emu_product.h"
#include "emu_trace.h"

typedef struct {
    unsigned console_warnings;
    char console_bytes[2048];
    size_t console_size;
    unsigned frames;
    unsigned serial_calls;
    unsigned releases;
    unsigned eeprom_maps;
    unsigned eeprom_accesses;
    uint64_t last_input_seq;
    size_t audio_frames;
    unsigned audio_resets;
    uint8_t pcm[8192];
} observations_t;

static void backend_console(void *opaque, emu_console_kind_t kind,
                             const char *bytes, size_t size) {
    observations_t *o = opaque;
    EMU_CHECK(o->console_size + size < sizeof o->console_bytes);
    memcpy(o->console_bytes + o->console_size, bytes, size);
    o->console_size += size;
    o->console_bytes[o->console_size] = 0;
    if (kind == EMU_CONSOLE_DELAY_WARNING) o->console_warnings++;
}

static void audio_output(void *opaque, const emu_audio_output_t *output) {
    observations_t *o = opaque;
    EMU_CHECK(output->completion_tick == EMU_AUDIO_TIME_UNKNOWN);
    EMU_CHECK((o->audio_frames + output->frame_count) * 2 <= sizeof o->pcm);
    for (size_t i = 0; i < output->frame_count; i++) {
        uint16_t value = (uint16_t)output->samples[i];
        o->pcm[2 * (o->audio_frames + i)] = value;
        o->pcm[2 * (o->audio_frames + i) + 1] = value >> 8;
    }
    o->audio_frames += output->frame_count;
}

static void audio_reset(void *opaque, const emu_audio_reset_t *reset) {
    observations_t *o = opaque;
    EMU_CHECK(reset->icount == EMU_AUDIO_TIME_UNKNOWN);
    EMU_CHECK(reset->reason == EMU_AUDIO_RESET_BACKEND);
    o->audio_resets++;
}

static void trace(void *opaque, const emu_trace_event_t *event, uint64_t seq) {
    observations_t *observations = opaque;
    if (!strcmp(event->kind, "eeprom_map")) observations->eeprom_maps++;
    if (!strcmp(event->kind, "eeprom_access")) observations->eeprom_accesses++;
    if (!strcmp(event->kind, "keypad_input")) observations->last_input_seq = seq;
    if (!strcmp(event->kind, "keypad_deferred_release")) {
        EMU_CHECK(seq > observations->last_input_seq);
        EMU_CHECK(event->pc == 0 && event->info.n == 9);
        observations->releases++;
    }
}

static void frame(void *opaque, const emu_frame_t *value) {
    observations_t *observations = opaque;
    EMU_CHECK(value->width == 101 && value->height == 64);
    observations->frames++;
}

static void serial(void *opaque, const uint8_t *bytes, size_t size) {
    observations_t *observations = opaque;
    EMU_CHECK(bytes != NULL && size > 0);
    observations->serial_calls++;
}

static unsigned unused_loopback_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    EMU_CHECK(fd >= 0);
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = 0,
    };
    EMU_CHECK(bind(fd, (const struct sockaddr *)&address, sizeof address) == 0);
    socklen_t size = sizeof address;
    EMU_CHECK(getsockname(fd, (struct sockaddr *)&address, &size) == 0);
    unsigned port = ntohs(address.sin_port);
    EMU_CHECK(close(fd) == 0);
    EMU_CHECK(port > 0);
    return port;
}

static int connect_gdb(unsigned port) {
    const struct timespec delay = {.tv_nsec = 10000000};
    for (unsigned attempt = 0; attempt < 500u; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        EMU_CHECK(fd >= 0);
        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
            .sin_port = htons((uint16_t)port),
        };
        if (connect(fd, (const struct sockaddr *)&address,
                    sizeof address) == 0) {
            struct timeval timeout = {.tv_sec = 5};
            EMU_CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                                &timeout, sizeof timeout) == 0);
            EMU_CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                                &timeout, sizeof timeout) == 0);
            return fd;
        }
        EMU_CHECK(errno == ECONNREFUSED || errno == EINTR);
        close(fd);
        nanosleep(&delay, NULL);
    }
    EMU_CHECK(0 && "cannot connect to QEMU GDB stub");
    return -1;
}

static void send_all(int fd, const char *bytes, size_t size) {
    while (size) {
        ssize_t sent = send(fd, bytes, size, 0);
        if (sent < 0 && errno == EINTR) continue;
        EMU_CHECK(sent > 0);
        bytes += sent;
        size -= (size_t)sent;
    }
}

static char receive_byte(int fd) {
    char byte;
    for (;;) {
        ssize_t received = recv(fd, &byte, 1, 0);
        if (received < 0 && errno == EINTR) continue;
        EMU_CHECK(received == 1);
        return byte;
    }
}

static void rsp_command(int fd, const char *command,
                        char *response, size_t response_size) {
    unsigned checksum = 0;
    for (const unsigned char *p = (const unsigned char *)command; *p; p++)
        checksum = (checksum + *p) & 0xffu;
    char packet[2048];
    int packet_size = snprintf(packet, sizeof packet, "$%s#%02x",
                               command, checksum);
    EMU_CHECK(packet_size > 0 && (size_t)packet_size < sizeof packet);
    send_all(fd, packet, (size_t)packet_size);
    EMU_CHECK(receive_byte(fd) == '+');
    while (receive_byte(fd) != '$') {}
    size_t used = 0;
    unsigned response_checksum = 0;
    for (;;) {
        char byte = receive_byte(fd);
        if (byte == '#') break;
        EMU_CHECK(used + 1u < response_size);
        response[used++] = byte;
        response_checksum = (response_checksum + (unsigned char)byte) & 0xffu;
    }
    char checksum_text[3] = {receive_byte(fd), receive_byte(fd), 0};
    char *end = NULL;
    unsigned long received_checksum = strtoul(checksum_text, &end, 16);
    EMU_CHECK(end && !*end && received_checksum == response_checksum);
    response[used] = 0;
    send_all(fd, "+", 1);
}

static void rsp_ok(int fd, const char *command) {
    char response[64];
    rsp_command(fd, command, response, sizeof response);
    EMU_CHECK(!strcmp(response, "OK"));
}

static void audio_ticks(int fd, emu_session_t *session, unsigned ticks) {
    char cmd[64], response[64];
    emu_error_t error = {0};
    emu_run_result_t result;
    int complete = 0;
    while (ticks) {
        unsigned count = ticks > 30000 ? 30000 : ticks;
        rsp_ok(fd, "P1e=00000100"); /* PC = 010000, synthetic local-RAM NOPs. */
        snprintf(cmd, sizeof cmd, "Z0,%x,2", 0x10000 + 2 * count);
        rsp_ok(fd, cmd);
        rsp_command(fd, "c", response, sizeof response);
        EMU_CHECK(response[0] == 'S' || response[0] == 'T');
        snprintf(cmd, sizeof cmd, "z0,%x,2", 0x10000 + 2 * count);
        rsp_ok(fd, cmd);
        EMU_CHECK(emu_session_pump(session, 0, &complete, &result, &error) == EMU_OK);
        EMU_CHECK(!complete);
        ticks -= count;
    }
}

static gboolean context_ready(gpointer opaque) {
    *(int *)opaque = 1;
    return G_SOURCE_REMOVE;
}

static void test_context_wakeup(emu_session_t *session) {
    emu_error_t error = {0};
    emu_run_result_t result;
    int complete = 0, ready = 0;
    gint64 start = g_get_monotonic_time();
    g_timeout_add(20, context_ready, &ready);
    while (!ready && g_get_monotonic_time() - start < 500000)
        EMU_CHECK(emu_session_pump(session, 200, &complete, &result, &error) == EMU_OK);
    EMU_CHECK(ready && !complete);
    /* A context deadline must interrupt a longer native poll. */
    EMU_CHECK(g_get_monotonic_time() - start < 150000);
}

static void audio_replay(int gdb, emu_session_t *session, observations_t *o) {
    char cmd[1100];
    uint8_t digest[32];
    char hex[65];
    for (unsigned pos = 0; pos < 60032; pos += 512) {
        int n = snprintf(cmd, sizeof cmd, "M%x,200:", 0x10000 + pos);
        for (unsigned i = 0; i < 256; i++) memcpy(cmd + n + i * 4, "cc00", 4);
        cmd[n + 1024] = 0;
        rsp_ok(gdb, cmd);
    }
    rsp_ok(gdb, "Me836,2:1600");
    rsp_ok(gdb, "Mef3a,2:0400");
    rsp_ok(gdb, "Meb80,10:01911018147314140a4d7252f8ffa0a0");
    audio_ticks(gdb, session, 40);
    for (unsigned i = 0; i < 4; i++) {
        rsp_ok(gdb, "Meb80,2:ccc1");
        audio_ticks(gdb, session, 40);
    }
    audio_ticks(gdb, session, 600000);
    EMU_CHECK(o->audio_frames == 768);
    emu_sha256(o->pcm, 768 * 2, digest);
    emu_sha256_hex(digest, hex);
    EMU_CHECK(!strcmp(hex, "f1cce669eeea097c23b979cbf99c8f69f844fa50db8391e60491dd560cbc672c"));
    unsigned resets = o->audio_resets;
    rsp_ok(gdb, "Me836,2:1700");
    rsp_ok(gdb, "Mef3a,2:0400");
    audio_ticks(gdb, session, 10000);
    EMU_CHECK(o->audio_frames == 768 && o->audio_resets > resets);
    rsp_ok(gdb, "Mfe1a,2:0900");
    rsp_ok(gdb, "Mff16,2:0004");
    rsp_ok(gdb, "Me836,2:1400");
    rsp_ok(gdb, "Me838,2:0100");
    rsp_ok(gdb, "Mef3a,2:0400");
    rsp_ok(gdb, "Meb80,c:bcad00000000000011111111");
    audio_ticks(gdb, session, 40);
    rsp_ok(gdb, "Meb80,8:bcad000000000000");
    audio_ticks(gdb, session, 40);
    rsp_ok(gdb, "Me836,2:1500");
    rsp_ok(gdb, "Mef3a,2:0400");
    audio_ticks(gdb, session, 40000);
    EMU_CHECK(o->audio_frames == 828);
    emu_sha256(o->pcm + 768 * 2, 60 * 2, digest);
    emu_sha256_hex(digest, hex);
    EMU_CHECK(!strcmp(hex, "babe03f70ab2a317a036510c12b988c5a92dad90ce9386621cff3d4e91594aeb"));
}

typedef struct {
    char bytes[8192];
    size_t size;
    unsigned warnings, ordinary;
} console_capture_t;

static void console_capture(void *opaque, emu_console_kind_t kind,
                             const char *bytes, size_t size) {
    console_capture_t *c = opaque;
    EMU_CHECK(c->size + size <= sizeof c->bytes);
    memcpy(c->bytes + c->size, bytes, size);
    c->size += size;
    if (kind == EMU_CONSOLE_DELAY_WARNING) c->warnings++;
    else c->ordinary++;
}

static void test_console(void) {
    const char warning[] = "Warning: The guest is now late by 2.0 to 3.0 seconds\n";
    for (size_t split = 0; split <= strlen(warning); split++) {
        emu_qemu_console_t parser = {0};
        console_capture_t capture = {0};
        emu_qemu_console_feed(&parser, console_capture, &capture, warning, split, 0);
        emu_qemu_console_feed(&parser, console_capture, &capture,
                              warning + split, strlen(warning) - split, 0);
        EMU_CHECK(capture.warnings == 1 && capture.ordinary == 0);
        EMU_CHECK(capture.size == strlen(warning));
        EMU_CHECK(!memcmp(capture.bytes, warning, capture.size));
    }
    emu_qemu_console_t parser = {0};
    console_capture_t capture = {0};
    const char lines[] = "diagnostic\n"
        "Warning: The guest is now late by 10.0 to 11.0 seconds\n"
        "Warning: The guest is now late by 2.0 to 3.0 seconds\n"
        "Warning: The guest is now late by 2.0 to 3.0 seconds extra\n"
        "tail";
    for (size_t i = 0; i < sizeof lines - 1; i++)
        emu_qemu_console_feed(&parser, console_capture, &capture, lines + i, 1, 0);
    emu_qemu_console_feed(&parser, console_capture, &capture, NULL, 0, 1);
    EMU_CHECK(capture.warnings == 2 && capture.ordinary == 3);
    EMU_CHECK(capture.size == sizeof lines - 1);
    EMU_CHECK(!memcmp(capture.bytes, lines, capture.size));
    capture = (console_capture_t){0};
    emu_qemu_console_feed(&parser, console_capture, &capture, lines, sizeof lines - 1, 1);
    EMU_CHECK(capture.warnings == 2 && capture.ordinary == 3);
    char large[4096];
    memset(large, 'x', sizeof large);
    memcpy(large + 255, warning, strlen(warning) - 1);
    large[sizeof large - 1] = '\n';
    capture = (console_capture_t){0};
    emu_qemu_console_feed(&parser, console_capture, &capture, large, sizeof large, 1);
    EMU_CHECK(capture.warnings == 0 && capture.ordinary > 1);
    EMU_CHECK(capture.size == sizeof large && !memcmp(capture.bytes, large, sizeof large));
    capture = (console_capture_t){0};
    emu_qemu_console_feed(&parser, console_capture, &capture, warning, strlen(warning) - 1, 1);
    EMU_CHECK(capture.warnings == 1);
    emu_qemu_console_feed(&parser, console_capture, &capture, NULL, 0, 1);
    EMU_CHECK(capture.warnings == 1);
}

int main(int argc, char **argv) {
    /* Exercise the real spawn/pipe/pump/EOF path before execing QEMU. */
    if (argc > 2 && !strcmp(argv[1], "-M") && getenv("EMU_TEST_CONSOLE_QEMU")) {
        fputs("Warning: The guest is now late by 10.0 to 11.0 seconds\n"
              "Warning: The guest is now late by 2.0 to 3.0 seconds\n"
              "ordinary backend diagnostic\n", stdout);
        for (unsigned i = 0; i < 600; i++) fputc('x', stdout);
        fputs("\nEOF tail", stdout);
        fflush(stdout);
        argv[0] = getenv("EMU_TEST_CONSOLE_QEMU");
        execv(argv[0], argv);
        return 2;
    }
    test_console();
    EMU_CHECK(argc == 2);
    EMU_CHECK(setenv("EMU_TEST_CONSOLE_QEMU", argv[1], 1) == 0);
    emu_preparation_request_t request = {
        .mode = EMU_STARTUP_FRESH,
        .source_path = BUNDLED_C55,
        .requested_device = "c55",
        .identity_source = EMU_IDENTITY_SOURCE_FSN,
        .fsn = 0x1234ABCDu,
        .runtime_options.trace_mask = EMU_TRACE_MASK_KEYPAD | EMU_TRACE_MASK_EEPROM,
    };
    snprintf(request.runtime_options.qemu_binary,
             sizeof request.runtime_options.qemu_binary, "%s", argv[0]);
    emu_prepared_session_t prepared;
    emu_preparation_result_t preparation;
    emu_error_t error = {0};
    EMU_CHECK(emu_prepare_startup(
        &request, &prepared, &preparation, &error) == EMU_OK);
    emu_engine_registry_t registry = {0};
    EMU_CHECK(emu_registry_register(
        &registry, &emu_qemu_engine_descriptor, &error) == EMU_OK);
    observations_t observations = {0};
    emu_callbacks_t callbacks = {
        .opaque = &observations,
        .console = backend_console,
        .frame = frame,
        .serial = serial,
        .audio_output = audio_output,
        .audio_reset = audio_reset,
    };
    emu_session_t *session = NULL;
    emu_error_code_t create_code = emu_session_create(
        &registry, "qemu", &prepared, &callbacks, &session, &error);
    if (create_code != EMU_OK)
        fprintf(stderr, "QEMU create failed: %s\n", error.message);
    EMU_CHECK(create_code == EMU_OK);
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_MASK_KEYPAD | EMU_TRACE_MASK_EEPROM, trace, &observations);
    EMU_CHECK(sink != NULL);
    EMU_CHECK(emu_session_trace_attach(session, sink, 0, 0, &error) == EMU_OK);
    EMU_CHECK(emu_session_battery(session, 50, 1, &error) == EMU_OK);
    EMU_CHECK(emu_session_key(session, "soft-left", 1, &error) == EMU_OK);
    emu_key_sample_t sample;
    EMU_CHECK(emu_session_key_query(
        session, "soft-left", &sample, &error) == EMU_OK);
    EMU_CHECK(sample.requested_pressed);
    const emu_product_t *product = emu_product_by_name("c55");
    size_t index = (size_t)(emu_product_key(product, "soft-left") - product->keys);
    emu_trace_event_t host_event = {.kind = "keypad_deferred_release", .detail = "queued"};
    emu_trace_info_str(&host_event.info, "button", "soft-left");
    emu_trace_info_bool(&host_event.info, "changed", 1);
    emu_trace_info_bool(&host_event.info, "down", 1);
    emu_trace_info_int(&host_event.info, "holders", 1);
    emu_trace_info_int(&host_event.info, "key_index", index);
    emu_trace_info_str(&host_event.info, "owner", "18446744073709551615");
    emu_trace_info_bool(&host_event.info, "pending", 1);
    emu_trace_info_str(&host_event.info, "phase", "queued");
    emu_trace_info_int(&host_event.info, "result", 0);
    const char *phases[] = {"queued", "sampled", "cancelled", "forced"};
    for (size_t i = 0; i < sizeof phases / sizeof phases[0]; i++) {
        host_event.detail = phases[i];
        host_event.info.kv[7].sval = phases[i];
        EMU_CHECK(emu_session_trace_host_event(session, &host_event, &error) == EMU_OK);
    }
    host_event.detail = "bad\nrecord";
    EMU_CHECK(emu_session_trace_host_event(session, &host_event, &error) == EMU_ERR_ARGUMENT);
    host_event.detail = "forced";
    EMU_CHECK(emu_session_key(session, "soft-left", 0, &error) == EMU_OK);
    EMU_CHECK(emu_session_start(session, &error) == EMU_OK);
    emu_run_result_t result;
    int complete = 0;
    for (unsigned i = 0; i < 100u && !observations.frames; i++)
        EMU_CHECK(emu_session_pump(
            session, 10, &complete, &result, &error) == EMU_OK);
    EMU_CHECK(!complete);
    emu_engine_state_t state;
    EMU_CHECK(emu_session_query(session, &state, &error) == EMU_OK);
    EMU_CHECK(state.guest_instructions > 0);
    EMU_CHECK(state.measured_ns > 0);
    emu_engine_state_t cached;
    EMU_CHECK(emu_session_query_request(session, 0, &cached, &error) == EMU_OK);
    EMU_CHECK(cached.measured_ns == state.measured_ns);
    EMU_CHECK(cached.guest_instructions == state.guest_instructions);
    EMU_CHECK(state.audio_available);
    emu_error_code_t stop_code = emu_session_request_stop(session, &error);
    if (stop_code != EMU_OK)
        fprintf(stderr, "QEMU stop failed: %s\n", error.message);
    EMU_CHECK(stop_code == EMU_OK);
    for (unsigned i = 0; i < 200u && !complete; i++)
        EMU_CHECK(emu_session_pump(
            session, 10, &complete, &result, &error) == EMU_OK);
    EMU_CHECK(complete);
    EMU_CHECK(emu_session_trace_host_event(session, &host_event, &error) == EMU_OK);
    EMU_CHECK(emu_session_trace_detach(session, &error) == EMU_OK);
    EMU_CHECK(observations.releases == 5);
    EMU_CHECK(emu_session_query_request(session, 0, &cached, &error) == EMU_OK);
    EMU_CHECK(cached.measured_ns > state.measured_ns);
    EMU_CHECK(cached.guest_instructions >= state.guest_instructions);
    EMU_CHECK(cached.ticks == result.ticks);
    EMU_CHECK(observations.eeprom_maps > 0);
    EMU_CHECK(emu_session_trace_host_event(session, &host_event, &error) == EMU_OK);
    EMU_CHECK(emu_trace_close(sink) == 0);
    emu_session_destroy(&session);
    EMU_CHECK(observations.console_warnings == 2);
    EMU_CHECK(strstr(observations.console_bytes, "ordinary backend diagnostic\n"));
    EMU_CHECK(strstr(observations.console_bytes, "\nEOF tail"));
    EMU_CHECK(observations.console_size ==
        strlen("Warning: The guest is now late by 10.0 to 11.0 seconds\n"
               "Warning: The guest is now late by 2.0 to 3.0 seconds\n"
               "ordinary backend diagnostic\n\nEOF tail") + 600);
    /* The remaining sessions retain the inherited-stdout path. */
    callbacks.console = NULL;
    snprintf(prepared.options.qemu_binary, sizeof prepared.options.qemu_binary,
             "%s", argv[1]);

    prepared.options.trace_mask = 0;
    prepared.options.sim_stub = 1;
    create_code = emu_session_create(
        &registry, "qemu", &prepared, &callbacks, &session, &error);
    if (create_code != EMU_OK)
        fprintf(stderr, "QEMU SIM create failed: %s\n", error.message);
    EMU_CHECK(create_code == EMU_OK);
    EMU_CHECK(emu_session_sim(session, 1, &error) == EMU_OK);
    EMU_CHECK(emu_session_sim(session, 0, &error) == EMU_ERR_UNSUPPORTED);
    EMU_CHECK(emu_session_start(session, &error) == EMU_OK);
    EMU_CHECK(emu_session_pump(
        session, 10, &complete, &result, &error) == EMU_OK);
    emu_session_destroy(&session);

    prepared.options.sim_stub = 0;
    prepared.options.gdb_enabled = 1;
    prepared.options.gdb_port = 0;
    EMU_CHECK(emu_session_create(
        &registry, "qemu", &prepared, &callbacks, &session, &error) ==
        EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(session == NULL);
    prepared.options.gdb_port = unused_loopback_port();
    create_code = emu_session_create(
        &registry, "qemu", &prepared, &callbacks, &session, &error);
    if (create_code != EMU_OK)
        fprintf(stderr, "QEMU GDB create failed: %s\n", error.message);
    EMU_CHECK(create_code == EMU_OK);
    EMU_CHECK(emu_session_start(session, &error) == EMU_OK);
    complete = 0;
    for (unsigned i = 0; i < 3u; i++)
        EMU_CHECK(emu_session_pump(
            session, 10, &complete, &result, &error) == EMU_OK);
    EMU_CHECK(!complete);
    EMU_CHECK(emu_session_query(session, &state, &error) == EMU_OK);
    EMU_CHECK(state.guest_instructions == 0);
    EMU_CHECK(emu_session_query_request(session, 0, &cached, &error) == EMU_OK);
    EMU_CHECK(cached.measured_ns == state.measured_ns);
    EMU_CHECK(emu_session_pump(session, 20, &complete, &result, &error) == EMU_OK);
    EMU_CHECK(emu_session_query_request(session, 0, &cached, &error) == EMU_OK);
    EMU_CHECK(cached.measured_ns > state.measured_ns);
    EMU_CHECK(cached.guest_instructions == 0);

    int gdb = connect_gdb(prepared.options.gdb_port);
    char response[2048];
    rsp_command(gdb, "?", response, sizeof response);
    EMU_CHECK(response[0] == 'S' || response[0] == 'T');
    rsp_command(gdb, "qXfer:features:read:target.xml:0,1000",
                response, sizeof response);
    EMU_CHECK(strstr(response, "c166-core.xml"));
    rsp_command(gdb, "p1e", response, sizeof response);
    EMU_CHECK(strlen(response) == 8u);
    rsp_command(gdb, "m800000,4", response, sizeof response);
    EMU_CHECK(!strcmp(response, "fa80c42f"));
    rsp_command(gdb, "Z0,802fc4,2", response, sizeof response);
    EMU_CHECK(!strcmp(response, "OK"));
    rsp_command(gdb, "c", response, sizeof response);
    EMU_CHECK(response[0] == 'S' || response[0] == 'T');
    rsp_command(gdb, "z0,802fc4,2", response, sizeof response);
    EMU_CHECK(!strcmp(response, "OK"));
    rsp_command(gdb, "s", response, sizeof response);
    EMU_CHECK(response[0] == 'S' || response[0] == 'T');
    EMU_CHECK(emu_session_query(session, &state, &error) == EMU_OK);
    EMU_CHECK(state.guest_instructions >= 2);
    uint64_t stopped_icount = state.guest_instructions;
    rsp_command(gdb, "D", response, sizeof response);
    EMU_CHECK(!strcmp(response, "OK"));
    EMU_CHECK(close(gdb) == 0);
    for (unsigned i = 0; i < 100u; i++) {
        EMU_CHECK(emu_session_pump(
            session, 10, &complete, &result, &error) == EMU_OK);
        EMU_CHECK(emu_session_query(session, &state, &error) == EMU_OK);
        if (state.guest_instructions > stopped_icount) break;
    }
    EMU_CHECK(state.guest_instructions > stopped_icount);
    EMU_CHECK(emu_session_request_stop(session, &error) == EMU_OK);
    for (unsigned i = 0; i < 200u && !complete; i++)
        EMU_CHECK(emu_session_pump(
            session, 10, &complete, &result, &error) == EMU_OK);
    EMU_CHECK(complete);
    emu_session_destroy(&session);

    observations.audio_frames = 0;
    create_code = emu_session_create(
        &registry, "qemu", &prepared, &callbacks, &session, &error);
    if (create_code != EMU_OK)
        fprintf(stderr, "QEMU audio create failed: %s\n", error.message);
    EMU_CHECK(create_code == EMU_OK);
    EMU_CHECK(emu_session_start(session, &error) == EMU_OK);
    gdb = connect_gdb(prepared.options.gdb_port);
    rsp_command(gdb, "?", response, sizeof response);
    test_context_wakeup(session);
    audio_replay(gdb, session, &observations);
    EMU_CHECK(close(gdb) == 0);
    emu_session_destroy(&session);

    EMU_CHECK(emu_prepared_rehash_source(&prepared, &error) == EMU_OK);
    emu_prepared_free(&prepared);
    puts("supervised QEMU adapter: PASS");
    return 0;
}
