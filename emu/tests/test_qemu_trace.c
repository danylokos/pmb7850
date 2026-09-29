#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emu_qemu_trace.h"
#include "test_support.h"

typedef struct {
    char owner[32];
    unsigned count;
    char kind[64];
    uint64_t icount;
    uint32_t pc;
    int has_data_bytes;
    long data_bytes;
    char disposition[16];
    uint32_t addr;
    uint32_t value;
    int size;
    int has_remaining_ticks;
    long remaining_ticks;
    long adc_raw;
    long result_word;
    long result0;
    long result1;
    long storage_writes;
    long start_x, start_y, end_x, end_y;
    char boundary[24];
    long audio_tick, audio_command, audio_length, audio_depth;
    char audio_payload[257], audio_outcome[32], audio_kind[16];
    int has_mailbox_mode, mailbox_async;
    long mailbox_control;
} capture_t;

static void capture(void *opaque, const emu_trace_event_t *event,
                    uint64_t sequence) {
    capture_t *state = opaque;
    (void)sequence;
    state->count++;
    snprintf(state->kind, sizeof state->kind, "%s", event->kind);
    state->icount = event->icount;
    state->pc = event->pc;
    state->addr = event->addr;
    state->value = event->value;
    state->size = event->size;
    for (int i = 0; i < event->info.n; i++) {
        const emu_trace_field_t *field = &event->info.kv[i];
        if (!strcmp(field->key, "control")) state->mailbox_control = field->ival;
        if (!strcmp(field->key, "async")) {
            state->has_mailbox_mode = 1;
            state->mailbox_async = field->ival;
        }
        if (!strcmp(field->key, "owner")) snprintf(state->owner, sizeof state->owner, "%s", field->sval);
        if (!strcmp(field->key, "tick")) state->audio_tick = field->ival;
        if (!strcmp(field->key, "command")) state->audio_command = field->ival;
        if (!strcmp(field->key, "payload_length")) state->audio_length = field->ival;
        if (!strcmp(field->key, "queue_depth")) state->audio_depth = field->ival;
        if (!strcmp(field->key, "payload_hex"))
            snprintf(state->audio_payload, sizeof state->audio_payload, "%s", field->sval);
        if (!strcmp(field->key, "decoder_outcome"))
            snprintf(state->audio_outcome, sizeof state->audio_outcome, "%s", field->sval);
        if (!strcmp(field->key, "stream_kind"))
            snprintf(state->audio_kind, sizeof state->audio_kind, "%s", field->sval);
        if (!strcmp(field->key, "storage_writes")) state->storage_writes = field->ival;
        if (!strcmp(field->key, "start_x")) state->start_x = field->ival;
        if (!strcmp(field->key, "start_y")) state->start_y = field->ival;
        if (!strcmp(field->key, "end_x")) state->end_x = field->ival;
        if (!strcmp(field->key, "end_y")) state->end_y = field->ival;
        if (!strcmp(field->key, "boundary_reason"))
            snprintf(state->boundary, sizeof state->boundary, "%s", field->sval);
        if (!strcmp(event->info.kv[i].key, "data_bytes")) {
            state->has_data_bytes = 1;
            state->data_bytes = event->info.kv[i].ival;
        } else if (!strcmp(event->info.kv[i].key, "disposition")) {
            snprintf(state->disposition, sizeof state->disposition, "%s",
                     event->info.kv[i].sval);
        } else if (!strcmp(event->info.kv[i].key, "remaining_ticks")) {
            state->has_remaining_ticks = 1;
            state->remaining_ticks = event->info.kv[i].ival;
        } else if (!strcmp(event->info.kv[i].key, "adc_raw")) {
            state->adc_raw = event->info.kv[i].ival;
        } else if (!strcmp(event->info.kv[i].key, "result_word")) {
            state->result_word = event->info.kv[i].ival;
        } else if (!strcmp(event->info.kv[i].key, "result0")) {
            state->result0 = event->info.kv[i].ival;
        } else if (!strcmp(event->info.kv[i].key, "result1")) {
            state->result1 = event->info.kv[i].ival;
        }
    }
}

static void test_masks(void) {
    char text[2048];
    EMU_CHECK(!emu_qemu_trace_log_mask(EMU_TRACE_MASK_FIRMWARE_PATCH,
                                         text, sizeof text));
    EMU_CHECK(text[0] == 0);
    EMU_CHECK(!emu_qemu_trace_log_mask(EMU_QEMU_TRACE_MASK, text, sizeof text));
    EMU_CHECK(strstr(text, "trace:pmb7850_adc_conversion") != NULL);
    EMU_CHECK(strstr(text, "trace:siemens_battery_state") != NULL);
    EMU_CHECK(strstr(text, "trace:x55_nor_read") != NULL);
    EMU_CHECK(strstr(text, "trace:x55_nor_mutation") != NULL);
    EMU_CHECK(strstr(text, "trace:x55_audio_packet") != NULL);
    EMU_CHECK(!emu_qemu_trace_log_mask(EMU_TRACE_EEPROM_MAP, text, sizeof text));
    EMU_CHECK(strstr(text, "trace:x55_nor_read") == NULL);
    EMU_CHECK(strstr(text, "trace:x55_nor_mutation") != NULL);
    EMU_CHECK(!emu_qemu_trace_log_mask(EMU_QEMU_TRACE_MASK, text, sizeof text));
    EMU_CHECK(strstr(text, "trace:siemens_host_service") != NULL);
    EMU_CHECK(!emu_qemu_trace_log_mask(EMU_TRACE_MASK_KEYPAD, text, sizeof text));
    EMU_CHECK(!emu_qemu_trace_log_mask(
        EMU_TRACE_MASK_SERIAL | EMU_TRACE_MASK_FLASH, text, sizeof text));
    EMU_CHECK(strstr(text, "trace:pmb7850_serial") != NULL);
    EMU_CHECK(strstr(text, "trace:m58lw064d_*") != NULL);
    EMU_CHECK(strstr(text, "trace:am29lv_*") != NULL);
    EMU_CHECK(strstr(text, "trace:w30_*") != NULL);
    EMU_CHECK(!emu_qemu_trace_log_mask(
        EMU_TRACE_MASK_LCD, text, sizeof text));
    EMU_CHECK(strstr(text, "trace:pcf8813_select") != NULL);
    EMU_CHECK(strstr(text, "trace:hm17_command") != NULL);
    EMU_CHECK(strstr(text, "trace:x55_color_lcd_frame") != NULL);
    EMU_CHECK(strstr(text, "trace:hm17_transaction") != NULL);
    EMU_CHECK(!emu_qemu_trace_log_mask(
        EMU_TRACE_LCD_FRAME, text, sizeof text));
    EMU_CHECK(strstr(text, "trace:pcf8813_frame") != NULL);
    EMU_CHECK(strstr(text, "trace:pcf8813_data") == NULL);
    EMU_CHECK(strstr(text, "trace:hm17_command") == NULL);
    EMU_CHECK(strstr(text, "trace:hm17_transaction") == NULL);
    EMU_CHECK(!emu_qemu_trace_log_mask(
        EMU_TRACE_LCD_TRANSACTION, text, sizeof text));
    EMU_CHECK(strstr(text, "trace:pcf8813_transaction") != NULL);
    EMU_CHECK(strstr(text, "trace:hm17_transaction") != NULL);
    EMU_CHECK(!emu_qemu_trace_log_mask(
        EMU_TRACE_MASK_SIM, text, sizeof text));
    EMU_CHECK(strstr(text, "trace:pmb7850_sim_*") != NULL);
    EMU_CHECK(emu_qemu_trace_log_mask(EMU_TRACE_EXEC, text,
                                     sizeof text) < 0);
}

static void test_host_service_roundtrip(void) {
    capture_t state = {0};
    char record[EMU_QEMU_HOST_EVENT_MAX], line[EMU_QEMU_HOST_EVENT_MAX + 128], error[256];
    emu_trace_sink_t *sink = emu_trace_open_callback(EMU_QEMU_TRACE_MASK, capture, &state);
    EMU_CHECK(sink != NULL);
    emu_trace_event_t event = {.kind = "input_owner", .detail = "key"};
    emu_trace_info_str(&event.info, "owner", "18446744073709551615");
    emu_trace_info_bool(&event.info, "changed", 1);
    emu_trace_info_int(&event.info, "result", -1);
    EMU_CHECK(!emu_qemu_trace_host_encode(&event, record, sizeof record));
    snprintf(line, sizeof line, "siemens_host_service icount=42 event=%s\n", record);
    EMU_CHECK(emu_qemu_trace_parse_line(sink, line, error, sizeof error) == 1);
    EMU_CHECK(state.count == 1 && state.icount == 42 && state.pc == 0);
    EMU_CHECK(!strcmp(state.owner, "18446744073709551615"));
    EMU_CHECK(emu_qemu_trace_host_encode(&event, record, 8) == -1);
    event.detail = "bad\nrecord";
    EMU_CHECK(emu_qemu_trace_host_encode(&event, record, sizeof record) == -1);
    const char *bad[] = {
        "siemens_host_service icount=42 event=input_owner key owner 3",
        "siemens_host_service icount=42 event=input_owner key changed 1 2",
        "siemens_host_service icount=42 event=input_owner key result 0 9x",
        "siemens_host_service icount=42 event=unknown key",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        EMU_CHECK(emu_qemu_trace_parse_line(sink, bad[i], error, sizeof error) < 0);
    EMU_CHECK(emu_trace_close(sink) == 0);
}

static void test_normalization(void) {
    capture_t state = {0};
    char error[256];
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_QEMU_TRACE_MASK, capture, &state);
    EMU_CHECK(sink != NULL);
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "pmb7850_serial icount=42 pc=0x123456 ASC0 tx value=0x41",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 1 && !strcmp(state.kind, "serial_tx"));
    EMU_CHECK(state.icount == 42 && state.pc == 0x123456);
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "siemens_keypad_input icount=43 pc=0x123458 name=soft-left raw=0x010e logical=0x01 pressed=1",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 2 && !strcmp(state.kind, "keypad_input"));
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "m58lw064d_program icount=44 pc=0x12345a offset=0x001000 size=2 value=0x1234 valid=1 status=0x80",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 3 && !strcmp(state.kind, "flash_program_start"));
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "am29lv_program icount=44 pc=0x12345a offset=0x001000 size=2 value=0x1234 valid=1",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 4 && !strcmp(state.kind, "flash_program_start"));
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "am29lv_read icount=45 pc=0x12345c view=native offset=0x500000 size=2 value=0x008c mode=array erase-active=1 erase-suspended=1 status=1",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 5 && !strcmp(state.kind, "flash_status"));
    EMU_CHECK(state.addr == 0x500000 && state.size == 2 &&
              state.value == 0x008c);
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "am29lv_read icount=46 pc=0x12345e view=native offset=0x400000 size=2 value=0x1234 mode=array erase-active=1 erase-suspended=1 status=0",
        error, sizeof error) == 0);
    EMU_CHECK(state.count == 5);
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "am29lv_erase icount=47 pc=0x123460 action=suspend offset=0x500000 remaining-ticks=79123",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 6 && !strcmp(state.kind, "flash_erase_suspend"));
    EMU_CHECK(state.has_remaining_ticks && state.remaining_ticks == 79123);
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "w30_operation icount=44 pc=0x12345a action=start operation=erase offset=0x390000 remaining-ns=1000",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 7 && !strcmp(state.kind, "flash_erase_start"));
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "hm17_frame hm17cm4096 frame=2 bytes=16160 digest=0x1234",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 8 && !strcmp(state.kind, "lcd_frame"));
    EMU_CHECK(state.has_data_bytes && state.data_bytes == 16160);
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "hm17_transaction hm17cm256 sequence=80 bytes=101 x=13 y=79 disposition=committed",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 9 && !strcmp(state.kind, "lcd_transaction"));
    EMU_CHECK(state.has_data_bytes && state.data_bytes == 101);
    EMU_CHECK(!strcmp(state.disposition, "committed"));
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "pcf8813_transaction sequence=8 data-bytes=102 x=0 y=7 disposition=held",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 10 && !strcmp(state.kind, "lcd_transaction"));
    EMU_CHECK(state.has_data_bytes && state.data_bytes == 102);
    EMU_CHECK(!strcmp(state.disposition, "held"));
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "hm17_frame hm17cm256 frame=3 bytes=8080 digest=0x5678",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 11 && !strcmp(state.kind, "lcd_frame"));
    EMU_CHECK(state.data_bytes == 8080);
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "pcf8813_frame sequence=4 data-bytes=816 digest=0x9abc",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 12 && !strcmp(state.kind, "lcd_frame"));
    EMU_CHECK(state.data_bytes == 816);
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "pmb7850_sim_byte icount=45 pc=0x12345c direction=card_to_phone value=0x90 sequence=1 path=SIM_RX",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 13 && !strcmp(state.kind, "sim_byte"));
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "pmb7850_sim_apdu icount=46 pc=0x12345e command=0x017f20a0a4000002 selection=0x3f0000007f200000 status=0x9f0f response=0",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 14 && !strcmp(state.kind, "sim_apdu"));
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "QEMU unrelated diagnostic", error, sizeof error) == 0);
    EMU_CHECK(emu_qemu_trace_parse_line(
        sink, "pmb7850_serial malformed", error, sizeof error) < 0);
    EMU_CHECK(emu_trace_close(sink) == 0);
}

static void test_simple_validation(void) {
    char path[] = "/tmp/emu-qemu-trace-XXXXXX";
    int fd = mkstemp(path);
    EMU_CHECK(fd >= 0);
    uint64_t header[] = {
        UINT64_MAX, UINT64_C(0xf2b177cb0aa429b4), 4,
    };
    EMU_CHECK(write(fd, header, sizeof header) == (ssize_t)sizeof header);
    EMU_CHECK(close(fd) == 0);
    char error[256];
    EMU_CHECK(!emu_qemu_trace_validate_simple(path, error, sizeof error));
    fd = open(path, O_WRONLY | O_TRUNC);
    EMU_CHECK(fd >= 0);
    EMU_CHECK(write(fd, header, sizeof header - 1u) ==
              (ssize_t)(sizeof header - 1u));
    EMU_CHECK(close(fd) == 0);
    EMU_CHECK(emu_qemu_trace_validate_simple(path, error, sizeof error) < 0);
    fd = open(path, O_WRONLY | O_TRUNC);
    EMU_CHECK(fd >= 0);
    EMU_CHECK(write(fd, header, sizeof header) == (ssize_t)sizeof header);
    uint64_t event_id = 7;
    uint64_t timestamp = 9;
    uint32_t record_size = 32;
    uint32_t pid = 1;
    EMU_CHECK(write(fd, &event_id, sizeof event_id) ==
              (ssize_t)sizeof event_id);
    EMU_CHECK(write(fd, &timestamp, sizeof timestamp) ==
              (ssize_t)sizeof timestamp);
    EMU_CHECK(write(fd, &record_size, sizeof record_size) ==
              (ssize_t)sizeof record_size);
    EMU_CHECK(write(fd, &pid, sizeof pid) == (ssize_t)sizeof pid);
    EMU_CHECK(close(fd) == 0);
    EMU_CHECK(emu_qemu_trace_validate_simple(path, error, sizeof error) < 0);
    EMU_CHECK(unlink(path) == 0);
}

static void test_battery_normalization(void) {
    capture_t state = {0};
    char error[256];
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_MASK_BATTERY, capture, &state);
    EMU_CHECK(sink != NULL);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "siemens_battery_state icount=123 pc=0x001234 level=100 charging=1 millivolts=4200 raw=-1763",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 1 && !strcmp(state.kind, "battery_state"));
    EMU_CHECK(state.icount == 123 && state.pc == 0x1234);
    EMU_CHECK(state.adc_raw == -1763);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "pmb7850_adc_conversion icount=456 pc=0x004568 command=0x0801 channel=1 raw=-1763 result_word=0 result0=0xf91d result1=0x0000",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 2 && !strcmp(state.kind, "battery_sample"));
    EMU_CHECK(state.icount == 456 && state.pc == 0x4568);
    EMU_CHECK(state.adc_raw == -1763 && state.result_word == 0);
    EMU_CHECK(state.result0 == 0xf91d && state.result1 == 0);
    EMU_CHECK(state.addr == 0xe062 && state.value == 0x0801);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "pmb7850_adc_conversion icount=789 pc=0x004570 command=0x0801 channel=1 raw=325 result_word=1 result0=0x0000 result1=0x0145",
        error, sizeof error) == 1);
    EMU_CHECK(state.result_word == 1 && state.result0 == 0);
    EMU_CHECK(state.result1 == 325 && state.adc_raw == 325);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "pmb7850_adc_conversion icount=789 pc=0x004570 command=0x0801 channel=1 raw=325 result_word=1 result0=0x0001 result1=0x0145",
        error, sizeof error) < 0);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "pmb7850_adc_conversion icount=789 pc=0x004570 command=0x0801 channel=1 raw=325 result_word=2 result0=0x0000 result1=0x0145",
        error, sizeof error) < 0);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "siemens_battery_state icount=0 pc=0x0 level=101 charging=0 millivolts=4200 raw=-1763",
        error, sizeof error) < 0);
    EMU_CHECK(state.count == 3);
    emu_trace_close(sink);
}

static void test_startup_normalization(void) {
    capture_t state = {0};
    char error[256];
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_MASK_KEYPAD, capture, &state);
    EMU_CHECK(sink != NULL);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "siemens_keypad_matrix_select icount=42 pc=0x012344 column=0x0000 command=0x0000 result=0x000d startup=1",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 1 && !strcmp(state.kind, "keypad_command"));
    EMU_CHECK(state.addr == 0xffc6 && state.value == 0);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "siemens_keypad_startup_release icount=43 pc=0x012346 result=0x000d",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 2 && !strcmp(state.kind, "keypad_startup_release"));
    EMU_CHECK(state.icount == 43 && state.pc == 0x12346);
    EMU_CHECK(state.addr == 0xef1a && state.value == 0x000d);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "siemens_keypad_access icount=44 pc=0x012348 write=1 offset=0x02 size=2 value=0x003d command=0x003d result=0x000f acknowledged=1",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 3 && !strcmp(state.kind, "keypad_command"));
    EMU_CHECK(state.addr == 0xef1c && state.value == 0x003d);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "siemens_keypad_access icount=45 pc=0x01234a write=0 offset=0x01 size=1 value=0x0000 command=0x003d result=0x000f acknowledged=1",
        error, sizeof error) == 1);
    EMU_CHECK(state.count == 4 && !strcmp(state.kind, "keypad_result"));
    EMU_CHECK(state.addr == 0xef1b && state.value == 0 && state.size == 1);
    const char *phases[] = {"queued", "sampled", "cancelled", "forced"};
    for (size_t i = 0; i < sizeof phases / sizeof phases[0]; i++) {
        char line[256];
        snprintf(line, sizeof line,
            "siemens_keypad_deferred_release icount=46 pc=0x000000 name=soft-left raw=0x010e index=12 phase=%s",
            phases[i]);
        EMU_CHECK(emu_qemu_trace_parse_line(sink, line, error, sizeof error) == 1);
        EMU_CHECK(!strcmp(state.kind, "keypad_deferred_release"));
        EMU_CHECK(state.icount == 46 && state.pc == 0 && state.value == 0x010e);
    }
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "siemens_keypad_deferred_release icount=46 pc=0x000000 name=soft-left raw=0x010e index=12 phase=bad",
        error, sizeof error) < 0);
    emu_trace_close(sink);
}

static void test_sweep_normalization(void) {
    capture_t state = {0};
    char error[256];
    emu_trace_sink_t *sink = emu_trace_open_callback(EMU_TRACE_MASK_LCD, capture, &state);
    EMU_CHECK(sink != NULL);
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "x55_lcd_sweep_frame icount=123 pc=0xabcdef model=hm17cm4096 sequence=9 bytes=8 writes=4 corners=0x0030000200330003 reason=window_wrap digest=0x123456789abcdef0",
        error, sizeof error) == 1);
    EMU_CHECK(state.icount == 123 && state.pc == 0xabcdef);
    EMU_CHECK(state.value == 9 && state.data_bytes == 8 && state.storage_writes == 4);
    EMU_CHECK(state.start_x == 48 && state.start_y == 2 && state.end_x == 51 && state.end_y == 3);
    EMU_CHECK(!strcmp(state.boundary, "window_wrap"));
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "x55_lcd_sweep_transaction icount=124 pc=0xabcd00 model=hm17cm4096 sequence=10 bytes=2 x=48 y=2 disposition=held",
        error, sizeof error) == 1);
    EMU_CHECK(state.icount == 124 && state.value == 10 && state.data_bytes == 2);
    EMU_CHECK(!strcmp(state.disposition, "held"));
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "x55_lcd_sweep_frame icount=123 pc=0xabcdef model=hm17cm4096 sequence=9 bytes=8 writes=4 corners=0x0030000200330003 reason=invalid digest=0x1234",
        error, sizeof error) < 0);
    emu_trace_close(sink);
}

static void test_audio_normalization(void) {
    capture_t state = {0};
    char error[256], line[384];
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_XBUS_AUDIO_PACKET, capture, &state);
    EMU_CHECK(sink != NULL);
    const char *phases[] = {"command", "retained", "replaced", "accepted",
                            "retry", "released", "source", "drain"};
    for (size_t i = 0; i < sizeof phases / sizeof phases[0]; i++) {
        snprintf(line, sizeof line,
            "x55_audio_packet tick=120 icount=123 pc=0xabcdef phase=%s command=0x8016 kind=2 sequence=7 depth=1 outcome=pcm bytes=ccc1\n",
            phases[i]);
        EMU_CHECK(emu_qemu_trace_parse_line(sink, line, error, sizeof error) == 1);
        EMU_CHECK(!strcmp(state.kind, "xbus_audio_packet"));
        EMU_CHECK(state.icount == 123 && state.pc == 0xabcdef);
        EMU_CHECK(state.audio_tick == 120 && state.audio_command == 0x8016);
        EMU_CHECK(state.audio_length == 2 && state.audio_depth == 1);
        EMU_CHECK(!strcmp(state.audio_payload, "ccc1"));
        EMU_CHECK(!strcmp(state.audio_outcome, "pcm"));
        EMU_CHECK(!strcmp(state.audio_kind, "si3"));
    }
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "x55_audio_packet tick=1 icount=1 pc=0x0 phase=command command=0x8014 kind=0 sequence=0 depth=0 outcome=ack bytes=",
        error, sizeof error) == 1);
    EMU_CHECK(state.audio_length == 0 && !strcmp(state.audio_payload, ""));
    char full[259];
    memset(full, 'c', sizeof full - 1); full[258] = 0;
    char extended[512];
    snprintf(extended, sizeof extended,
        "x55_audio_packet tick=1 icount=1 pc=0x0 phase=accepted command=0x8016 kind=2 sequence=1 depth=1 outcome=pcm bytes=%s", full);
    EMU_CHECK(emu_qemu_trace_parse_line(sink, extended, error, sizeof error) < 0);
    full[256] = 0;
    snprintf(extended, sizeof extended,
        "x55_audio_packet tick=1 icount=1 pc=0x0 phase=accepted command=0x8016 kind=2 sequence=1 depth=1 outcome=pcm bytes=%s", full);
    EMU_CHECK(emu_qemu_trace_parse_line(sink, extended, error, sizeof error) == 1);
    EMU_CHECK(state.audio_length == 128 && !strcmp(state.audio_payload, full));
    const char *invalid[] = {"0", "xx", "abcd ", "abcd\ntrailing", "CCC1"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        snprintf(line, sizeof line,
            "x55_audio_packet tick=1 icount=1 pc=0x0 phase=accepted command=0x8016 kind=2 sequence=1 depth=1 outcome=pcm bytes=%s",
            invalid[i]);
        EMU_CHECK(emu_qemu_trace_parse_line(sink, line, error, sizeof error) < 0);
    }
    EMU_CHECK(emu_qemu_trace_parse_line(sink,
        "x55_audio_packet tick=1 icount=1 pc=0x0 phase=accepted command=0x8016 kind=2 sequence=1 depth=0 outcome=pcm bytes=ccc1",
        error, sizeof error) < 0);
    emu_trace_close(sink);
}

static void test_mailbox_mode(void) {
    capture_t state = {0};
    char error[256], line[384];
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_XBUS_MAILBOX_COMPLETE, capture, &state);
    const char *base = "siemens_xbus_mailbox icount=9307713 pc=0xa13cb4 phase=sync-ready transaction=16 status=0x0002 result=0x0010 irq=0";
    EMU_CHECK(sink != NULL);
    /* Archived records remain readable, without fabricating a mode. */
    EMU_CHECK(emu_qemu_trace_parse_line(sink, base, error, sizeof error) == 1);
    EMU_CHECK(!state.has_mailbox_mode);
    snprintf(line, sizeof line, "%s control=0x1700 async=0\n", base);
    EMU_CHECK(emu_qemu_trace_parse_line(sink, line, error, sizeof error) == 1);
    EMU_CHECK(state.has_mailbox_mode && !state.mailbox_async);
    EMU_CHECK(state.mailbox_control == 0x1700);
    snprintf(line, sizeof line, "%s control=0x1701 async=1", base);
    EMU_CHECK(emu_qemu_trace_parse_line(sink, line, error, sizeof error) == 1);
    EMU_CHECK(state.mailbox_async && state.mailbox_control == 0x1701);
    const char *invalid[] = {" control=0x1700", " control=0x1700 async=2",
                            " control=0x11700 async=1", " garbage"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        snprintf(line, sizeof line, "%s%s", base, invalid[i]);
        EMU_CHECK(emu_qemu_trace_parse_line(sink, line, error, sizeof error) < 0);
    }
    emu_trace_close(sink);
}

int main(void) {
    test_mailbox_mode();
    test_masks();
    test_host_service_roundtrip();
    test_audio_normalization();
    test_sweep_normalization();
    test_startup_normalization();
    test_battery_normalization();
    test_normalization();
    test_simple_validation();
    puts("QEMU trace normalization: PASS");
    return 0;
}
