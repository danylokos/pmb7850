#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "emu_cemu_adapter.h"
#include "emu_engine.h"
#include "emu_product.h"
#include "emu_trace.h"



typedef struct {
    char bytes[32768];
    size_t size;
    int fail;
} text_output_t;

static int text_write(void *opaque, const char *bytes, size_t size) {
    text_output_t *output = opaque;
    if (output->fail || size >= sizeof output->bytes - output->size)
        return -1;
    memcpy(output->bytes + output->size, bytes, size);
    output->size += size;
    output->bytes[output->size] = 0;
    return 0;
}

static ptrdiff_t text_eof(void *opaque, char *bytes, size_t size) {
    (void)opaque;
    (void)bytes;
    (void)size;
    return 0;
}

typedef struct {
    uint64_t ticks, guest;
    unsigned paused, running, restored, waits;
    int interrupt, fail, fail_running, fail_wait, retry_read;
} service_t;
static int service(void *opaque, int phase, uint64_t ticks, uint64_t guest) {
    service_t *s = opaque;
    EMU_CHECK(ticks >= s->ticks && guest >= s->guest && guest <= ticks);
    s->ticks = ticks; s->guest = guest;
    if (phase == 0) s->paused++;
    if (phase == 1) s->running++;
    if (phase == 2) s->restored++;
    if (s->fail || (s->fail_running && phase == 1 && ticks)) return -1;
    if (phase == 1 && ticks && s->interrupt) {
        s->interrupt = 0;
        return 1;
    }
    return 0;
}
static int input_wait(void *opaque, int timeout_ms) {
    service_t *s = opaque;
    EMU_CHECK(timeout_ms == 10);
    if (s->fail_wait) return -1;
    return ++s->waits > 3;
}

static ptrdiff_t text_retry(void *opaque, char *bytes, size_t size) {
    service_t *s = opaque;
    (void)bytes; (void)size;
    if (!s->retry_read++) return -2;
    return 0;
}

static void rehash(emu_prepared_session_t *prepared) {
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

static void put16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void seed_eeprom(uint8_t *flash) {
    const size_t base = 0x7A0000;
    const size_t magic = base + 0x12;
    flash[magic - 2] = flash[magic - 1] = 0xFE;
    memcpy(flash + magic, "EELITE", 6);
    memcpy(flash + magic + 0x20000, "EEFULL", 6);
    memcpy(flash + magic + 0x40000, "EEFULL", 6);
    uint8_t *record = flash + base + 0x200;
    put16(record, 0x01FC);
    put16(record + 2, 16);
    put16(record + 4, 0x1000);
    put16(record + 6, 0x00FA);
    put16(record + 8, 77);
    put16(record + 10, 0xFC00);
    flash[base + 0x1000] = 0x11;
    flash[base + 0x1001] = 0x22;
    flash[base + 0x1002] = 0x33;
    flash[base + 0x1003] = 0x44;
}

int main(int argc, char **argv) {
    EMU_CHECK(argc == 2);
    emu_error_t error = {0};
    emu_prepared_session_t prepared;
    emu_prepared_init(&prepared);
    EMU_CHECK(emu_prepared_load_source(
                  &prepared, argv[1], &error) == EMU_OK);
    EMU_CHECK(emu_product_prepare_image(
                  &prepared, "c55", &error) == EMU_OK);
    seed_eeprom(prepared.source.bytes);
    rehash(&prepared);

    emu_engine_registry_t registry = {0};
    EMU_CHECK(emu_registry_register(
                  &registry, &emu_cemu_engine_descriptor,
                  &error) == EMU_OK);
    emu_session_t *session = NULL;
    EMU_CHECK(emu_session_create(
                  &registry, "cemu", &prepared, NULL,
                  &session, &error) == EMU_OK);

    emu_trace_event_t event = {.kind = "input_owner"};
    EMU_CHECK(emu_session_trace_host_event(session, &event, &error) == EMU_ERR_UNSUPPORTED);

    text_output_t output = {0};
    emu_debugger_request_t debugger = {
        .script = "regs",
        .command = "help; eeprom list; eeprom show 77 0 4; "
                   "checkpoint; restore; eeprom watches; quit",
        .batch_idle = 1,
        .output = {.opaque = &output, .write = text_write},
    };
    EMU_CHECK(emu_session_debugger_run(
                  session, &debugger, &error) == EMU_OK);
    EMU_CHECK(strstr(output.bytes, "pc=0x00000000") != NULL);
    EMU_CHECK(strstr(output.bytes, "Commands:") == NULL);
    EMU_CHECK(strstr(output.bytes, "eeprom list [CHIP]") != NULL);
    EMU_CHECK(strstr(output.bytes, "block=0x004d (77)") != NULL);
    EMU_CHECK(strstr(output.bytes, "0x0000: 11 22 33 44") != NULL);
    EMU_CHECK(strstr(output.bytes, "restored to icount=0") != NULL);
    EMU_CHECK(strstr(output.bytes, "no logical EEPROM watches") != NULL);

    memset(&output, 0, sizeof output);
    debugger.script = NULL;
    debugger.command = NULL;
    debugger.interactive = 1;
    debugger.input.read = text_eof;
    EMU_CHECK(emu_session_debugger_run(
                  session, &debugger, &error) == EMU_OK);
    EMU_CHECK(strstr(output.bytes, "c55 debugger") != NULL);
    EMU_CHECK(strstr(output.bytes, "(dbg pc=0x00000000)") != NULL);

    output.fail = 1;
    debugger.interactive = 0;
    debugger.command = "regs";
    EMU_CHECK(emu_session_debugger_run(
                  session, &debugger, &error) == EMU_ERR_IO);
    memset(&output, 0, sizeof output);
    debugger.command = "quit";
    EMU_CHECK(emu_session_debugger_run(
                  session, &debugger, &error) == EMU_OK);
    service_t serviced = {0};
    memset(&output, 0, sizeof output);
    debugger = (emu_debugger_request_t){
        .command = "setr pc 0; step 1; checkpoint; step 2; restore; step 1; quit",
        .output = {.opaque = &output, .write = text_write},
        .service = service, .service_opaque = &serviced,
    };
    EMU_CHECK(emu_session_debugger_run(session, &debugger, &error) == EMU_OK);
    EMU_CHECK(serviced.ticks == 4 && serviced.guest == 4 && serviced.restored == 1);
    EMU_CHECK(serviced.paused >= 7);
    emu_engine_state_t debug_state;
    EMU_CHECK(emu_session_query_request(session, 0, &debug_state, &error) == EMU_OK);
    EMU_CHECK(debug_state.icount == 2); /* execution total survives rewind */

    serviced = (service_t){0};
    memset(&output, 0, sizeof output);
    debugger.command = "setr pc 0; break 0x800100; cont; step; cont 1; quit";
    EMU_CHECK(emu_session_debugger_run(session, &debugger, &error) == EMU_OK);
    EMU_CHECK(strstr(output.bytes, "[break] pc=0x00800100") != NULL);
    EMU_CHECK(serviced.ticks == 3); /* stop before breakpoint, step still exact */

    serviced = (service_t){0};
    debugger.command = NULL; debugger.interactive = 1;
    debugger.input = (emu_text_io_t){.opaque = &serviced, .read = text_eof};
    debugger.input_wait = input_wait;
    EMU_CHECK(emu_session_debugger_run(session, &debugger, &error) == EMU_OK);
    EMU_CHECK(serviced.waits == 4 && serviced.paused >= 4 && serviced.ticks == 0);
    debugger.input.read = text_retry;
    EMU_CHECK(emu_session_debugger_run(session, &debugger, &error) == EMU_OK);
    EMU_CHECK(serviced.retry_read == 2);
    serviced.fail_wait = 1;
    EMU_CHECK(emu_session_debugger_run(session, &debugger, &error) == EMU_ERR_IO);
    serviced.fail_wait = 0;
    serviced.fail = 1;
    EMU_CHECK(emu_session_debugger_run(session, &debugger, &error) == EMU_ERR_IO);
    EMU_CHECK(strstr(error.message, "service failed"));
    emu_session_destroy(&session);

    prepared.source.bytes[0x100] = 0x0D;
    prepared.source.bytes[0x101] = 0xFE;
    rehash(&prepared);
    EMU_CHECK(emu_session_create(
                  &registry, "cemu", &prepared, NULL,
                  &session, &error) == EMU_OK);
    serviced = (service_t){.interrupt = 1};
    memset(&output, 0, sizeof output);
    debugger = (emu_debugger_request_t){
        .command = "setr pc 0x800100; cont; step; quit",
        .output = {.opaque = &output, .write = text_write},
        .service = service, .service_opaque = &serviced,
    };
    EMU_CHECK(emu_session_debugger_run(session, &debugger, &error) == EMU_OK);
    EMU_CHECK(strstr(output.bytes, "[interrupted]") && strstr(output.bytes, "[step]"));
    EMU_CHECK(serviced.ticks > 1 && serviced.guest == serviced.ticks);
    serviced = (service_t){.fail_running = 1};
    memset(&output, 0, sizeof output);
    debugger.command = "cont; regs";
    EMU_CHECK(emu_session_debugger_run(session, &debugger, &error) == EMU_ERR_IO);
    EMU_CHECK(strstr(output.bytes, "[error]") && !strstr(output.bytes, "psw="));
    EMU_CHECK(emu_session_monitor_start(
                  session, 16, 2000000, &error) == EMU_OK);
    EMU_CHECK(emu_session_monitor_start(
                  session, 16, 2000000, &error) == EMU_ERR_LIFECYCLE);
    emu_monitor_verdict_t verdict = {0};
    emu_run_request_t request = {
        .tick_budget = 10000,
        .allow_idle_batch = 0,
    };
    emu_run_result_t result;
    EMU_CHECK(emu_session_run_request(
                  session, &request, &result, &error) == EMU_OK);
    EMU_CHECK(result.status == EMU_RUN_STOPPED);
    EMU_CHECK(result.ticks < request.tick_budget);
    EMU_CHECK(emu_session_monitor_poll(
                  session, &verdict, &error) == EMU_OK);
    EMU_CHECK(!strcmp(verdict.status, "spin"));
    EMU_CHECK(strstr(verdict.reason, "stuck spin") != NULL);
    emu_engine_state_t state;
    EMU_CHECK(emu_session_query_request(
                  session, 0, &state, &error) == EMU_OK);
    char summary[1u << 18];
    emu_summary_request_t summary_request = {
        .status = verdict.status,
        .reason = verdict.reason,
        .steps = state.icount,
        .pc = state.pc,
    };
    EMU_CHECK(emu_session_summary_format(
                  session, &summary_request, summary,
                  sizeof summary, &error) == EMU_OK);
    EMU_CHECK(strstr(summary, "status: spin") != NULL);
    EMU_CHECK(strstr(summary, verdict.reason) != NULL);
    EMU_CHECK(emu_session_monitor_stop(session, &error) == EMU_OK);
    EMU_CHECK(emu_session_monitor_poll(
                  session, &verdict, &error) == EMU_ERR_LIFECYCLE);
    emu_session_destroy(&session);
    emu_prepared_free(&prepared);
    puts("CEMU native diagnostic bridge: PASS");
    return 0;
}
