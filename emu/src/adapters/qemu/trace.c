#define _POSIX_C_SOURCE 200809L

#include "emu_qemu_trace.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "emu_trace_schema.h"
#include "emu_product.h"

static int fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return -1;
}

static int append(char *output, size_t capacity, size_t *used,
                  const char *event) {
    int count = snprintf(output + *used, capacity - *used, "%strace:%s",
                         *used ? "," : "", event);
    if (count < 0 || (size_t)count >= capacity - *used) return -1;
    *used += (size_t)count;
    return 0;
}

int emu_qemu_trace_log_mask(emu_trace_mask_t mask, char *output,
                            size_t capacity) {
    size_t used = 0;
    if (!output || !capacity || (mask & ~EMU_QEMU_TRACE_MASK)) return -1;
    output[0] = 0;
#define ADD(mask_value, event) \
    do { if ((mask & (mask_value)) && append(output, capacity, &used, event)) \
        return -1; } while (0)
    ADD(EMU_TRACE_MASK_SERIAL, "pmb7850_serial");
    ADD(EMU_TRACE_BATTERY_SAMPLE, "pmb7850_adc_conversion");
    ADD(EMU_TRACE_BATTERY_STATE, "siemens_battery_state");
    ADD(EMU_TRACE_XBUS_ACCESS, "siemens_xbus_access");
    ADD(EMU_TRACE_XBUS_MAILBOX_COMPLETE, "siemens_xbus_mailbox");
    ADD(EMU_TRACE_XBUS_AUDIO_PACKET, "x55_audio_packet");
    ADD(EMU_TRACE_MASK_PEC, "pmb7850_pec");
    ADD(EMU_TRACE_KEYPAD_COMMAND | EMU_TRACE_KEYPAD_RESULT,
        "siemens_keypad_access");
    ADD(EMU_TRACE_KEYPAD_INPUT, "siemens_keypad_input");
    ADD(EMU_TRACE_KEYPAD_DEFERRED_RELEASE | EMU_TRACE_INPUT_OWNER |
        EMU_TRACE_SERIAL_HISTORY, "siemens_host_service");
    ADD(EMU_TRACE_KEYPAD_RESULT, "siemens_keypad_startup_release");
    ADD(EMU_TRACE_KEYPAD_COMMAND, "siemens_keypad_matrix_select");
    ADD(EMU_TRACE_TDMA_TICK, "pmb7850_tdma");
    ADD(EMU_TRACE_TDMA_IRQ, "pmb7850_tdma_irq");
    ADD(EMU_TRACE_MASK_SSC0, "pmb7850_ssc");
    ADD(EMU_TRACE_LCD_SELECT, "pcf8813_select");
    ADD(EMU_TRACE_LCD_COMMAND, "pcf8813_command");
    ADD(EMU_TRACE_LCD_COMMAND, "hm17_command");
    ADD(EMU_TRACE_LCD_COMMAND, "x55_color_lcd_command");
    ADD(EMU_TRACE_LCD_DATA, "pcf8813_data");
    ADD(EMU_TRACE_LCD_DATA, "hm17_data");
    ADD(EMU_TRACE_LCD_DATA, "x55_color_lcd_data");
    ADD(EMU_TRACE_LCD_RESET, "hm17_reset");
    ADD(EMU_TRACE_LCD_RESET, "x55_color_lcd_reset");
    ADD(EMU_TRACE_LCD_FRAME, "pcf8813_frame");
    ADD(EMU_TRACE_LCD_FRAME, "hm17_frame");
    ADD(EMU_TRACE_LCD_FRAME, "x55_color_lcd_frame");
    ADD(EMU_TRACE_LCD_FRAME, "x55_lcd_sweep_frame");
    ADD(EMU_TRACE_LCD_TRANSACTION, "x55_lcd_sweep_transaction");
    ADD(EMU_TRACE_LCD_STATUS, "pcf8813_status");
    ADD(EMU_TRACE_LCD_TRANSACTION, "pcf8813_transaction");
    ADD(EMU_TRACE_LCD_TRANSACTION, "hm17_transaction");
    ADD(EMU_TRACE_MASK_LIFECYCLE, "pmb7850_lifecycle");
    ADD(EMU_TRACE_MASK_SIM, "pmb7850_sim_*");
    ADD(EMU_TRACE_MASK_FLASH, "m58lw064d_*");
    ADD(EMU_TRACE_MASK_FLASH, "am29lv_*");
    ADD(EMU_TRACE_MASK_FLASH, "w30_*");
    ADD(EMU_TRACE_EEPROM_ACCESS, "x55_nor_read");
    ADD(EMU_TRACE_MASK_EEPROM, "x55_nor_mutation");
#undef ADD
    return 0;
}

static int emit(emu_trace_sink_t *sink, emu_trace_event_t *event,
                char *error, size_t capacity) {
    if (emu_trace_schema_validate_event(event, error, capacity)) return -1;
    if (emu_trace_sink_accepts(sink, event->kind))
        emu_trace_emit(sink, event);
    return 1;
}

/* Service records use whitespace-delimited, bounded ASCII tokens. They carry
 * only schema-validated host diagnostics, never guest input or key mutations. */
static int token_valid(const char *text) {
    if (!text || !*text) return 0;
    for (; *text; text++)
        if (!isalnum((unsigned char)*text) && *text != '_' && *text != '-') return 0;
    return 1;
}

int emu_qemu_trace_host_encode(const emu_trace_event_t *event, char *out, size_t capacity) {
    char error[256];
    if (!event || !event->kind || !out || !capacity ||
        (strcmp(event->kind, "input_owner") && strcmp(event->kind, "serial_history") &&
         strcmp(event->kind, "keypad_deferred_release")) ||
        emu_trace_schema_validate_event(event, error, sizeof error) ||
        !token_valid(event->detail)) return -1;
    int n = snprintf(out, capacity, "%s %s", event->kind, event->detail);
    if (n < 0 || (size_t)n >= capacity) return -1;
    size_t used = (size_t)n;
    for (int i = 0; i < event->info.n; i++) {
        const emu_trace_field_t *f = &event->info.kv[i];
        char number[32];
        snprintf(number, sizeof number, "%ld", f->ival);
        const char *value = f->kind == EMU_TRACE_VALUE_STRING ? f->sval : number;
        if (!token_valid(f->key) || !token_valid(value) ||
            (f->kind != EMU_TRACE_VALUE_STRING && f->kind != EMU_TRACE_VALUE_I64 &&
             f->kind != EMU_TRACE_VALUE_BOOL)) return -1;
        n = snprintf(out + used, capacity - used, " %s %u %s", f->key, f->kind, value);
        if (n < 0 || (size_t)n >= capacity - used) return -1;
        used += (size_t)n;
    }
    return 0;
}

static int parse_host_service(emu_trace_sink_t *sink, const char *line,
                              char *error, size_t capacity) {
    unsigned long long icount;
    int offset = 0;
    if (sscanf(line, "siemens_host_service icount=%llu event=%n", &icount, &offset) != 1 ||
        !offset || strlen(line + offset) >= EMU_QEMU_HOST_EVENT_MAX)
        return fail(error, capacity, "malformed QEMU host service record");
    char copy[EMU_QEMU_HOST_EVENT_MAX];
    strcpy(copy, line + offset);
    char *save = NULL;
    emu_trace_event_t event = {.icount = icount, .pc = 0};
    event.kind = strtok_r(copy, " \n", &save);
    event.detail = strtok_r(NULL, " \n", &save);
    if (!event.kind || !event.detail ||
        (strcmp(event.kind, "input_owner") && strcmp(event.kind, "serial_history") &&
         strcmp(event.kind, "keypad_deferred_release")))
        return fail(error, capacity, "invalid QEMU host service kind");
    char *key;
    while ((key = strtok_r(NULL, " \n", &save))) {
        char *type = strtok_r(NULL, " \n", &save);
        char *value = strtok_r(NULL, " \n", &save);
        if (!type || !value || event.info.n == 32)
            return fail(error, capacity, "invalid QEMU host service field");
        if (!strcmp(type, "3")) emu_trace_info_str(&event.info, key, value);
        else if (!strcmp(type, "0") || !strcmp(type, "1")) {
            char *end;
            errno = 0;
            long number = strtol(value, &end, 10);
            if (errno || *end || (type[0] == '1' && number != 0 && number != 1))
                return fail(error, capacity, "invalid QEMU host service number");
            if (type[0] == '1') emu_trace_info_bool(&event.info, key, number);
            else emu_trace_info_int(&event.info, key, number);
        } else return fail(error, capacity, "invalid QEMU host service type");
    }
    return emit(sink, &event, error, capacity);
}

static int parse_serial(emu_trace_sink_t *sink, const char *line,
                        char *error, size_t capacity) {
    unsigned long long icount;
    unsigned pc, value;
    char unit[16], action[16];
    if (sscanf(line, "pmb7850_serial icount=%llu pc=0x%x %15s %15s value=0x%x",
               &icount, &pc, unit, action, &value) != 5)
        return fail(error, capacity, "malformed QEMU serial trace record");
    const char *kind = !strcmp(action, "tx") ? "serial_tx" :
                       !strcmp(action, "rx") ? "serial_rx" : NULL;
    if (!kind) return fail(error, capacity, "unknown QEMU serial action");
    emu_trace_event_t event = {
        .kind = kind, .icount = icount, .pc = pc,
        .has_value = 1, .value = value, .detail = unit,
    };
    return emit(sink, &event, error, capacity);
}

static int parse_battery(emu_trace_sink_t *sink, const char *line,
                         char *error, size_t capacity) {
    unsigned long long icount;
    unsigned pc, command, channel, word, result0, result1, level, millivolts;
    int raw, charging;
    emu_trace_event_t event = {0};
    if (!strncmp(line, "pmb7850_adc_conversion ",
                 sizeof "pmb7850_adc_conversion " - 1u)) {
        if (sscanf(line, "pmb7850_adc_conversion icount=%llu pc=0x%x command=0x%x channel=%u raw=%d result_word=%u result0=0x%x result1=0x%x",
                   &icount, &pc, &command, &channel, &raw, &word,
                   &result0, &result1) != 8 || word > 1 ||
            command > 65535 || !(command & 0x0800) ||
            channel != (command & 0xff) || raw < -32768 || raw > 32767 ||
            (word == 0 ? result1 : result0) != 0 ||
            (word == 0 ? result0 : result1) != (unsigned)(uint16_t)raw)
            return fail(error, capacity, "malformed QEMU battery sample");
        event.kind = "battery_sample";
        event.has_addr = event.has_value = event.has_size = 1;
        event.addr = 0xe062;
        event.size = 2;
        event.value = command;
        emu_trace_info_int(&event.info, "channel", channel);
        emu_trace_info_int(&event.info, "result_word", word);
        emu_trace_info_int(&event.info, "result0", result0);
        emu_trace_info_int(&event.info, "result1", result1);
    } else {
        if (sscanf(line, "siemens_battery_state icount=%llu pc=0x%x level=%u charging=%d millivolts=%u raw=%d",
                   &icount, &pc, &level, &charging, &millivolts, &raw) != 6 ||
            level > 100 || (charging != 0 && charging != 1) ||
            raw < -32768 || raw > 32767)
            return fail(error, capacity, "malformed QEMU battery state");
        event.kind = "battery_state";
        emu_trace_info_int(&event.info, "level", level);
        emu_trace_info_int(&event.info, "millivolts", millivolts);
        emu_trace_info_bool(&event.info, "charging", charging);
    }
    event.icount = icount;
    event.pc = pc;
    event.detail = "";
    emu_trace_info_int(&event.info, "adc_raw", raw);
    return emit(sink, &event, error, capacity);
}

static int parse_xbus(emu_trace_sink_t *sink, const char *line,
                      char *error, size_t capacity) {
    unsigned long long icount, value, transaction;
    unsigned pc, addr, size, status, result;
    int write, irq;
    char phase[32];
    if (!strncmp(line, "siemens_xbus_access ",
                 sizeof "siemens_xbus_access " - 1u)) {
        if (sscanf(line, "siemens_xbus_access icount=%llu pc=0x%x write=%d addr=0x%x size=%u value=0x%llx",
                   &icount, &pc, &write, &addr, &size, &value) != 6)
            return fail(error, capacity, "malformed QEMU XBUS trace record");
        emu_trace_event_t event = {
            .kind = "xbus_access", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = addr, .has_size = 1, .size = (int)size,
            .has_value = 1, .value = (uint32_t)value, .detail = "",
        };
        emu_trace_info_str(&event.info, "access", write ? "write" : "read");
        return emit(sink, &event, error, capacity);
    }
    unsigned control;
    int irq_mode, used = 0;
    if (sscanf(line, "siemens_xbus_mailbox icount=%llu pc=0x%x phase=%31s transaction=%llu status=0x%x result=0x%x irq=%d%n",
               &icount, &pc, phase, &transaction, &status, &result, &irq, &used) != 7)
        return fail(error, capacity, "malformed QEMU XBUS mailbox record");
    const char *mode = line + used;
    int has_mode = strspn(mode, " \r\n") != strlen(mode);
    if (has_mode) {
        used = 0;
        if (sscanf(mode, " control=0x%x async=%d%n", &control, &irq_mode,
                   &used) != 2 || !used || control > 0xffff ||
            (irq_mode != 0 && irq_mode != 1) ||
            strspn(mode + used, " \r\n") != strlen(mode + used))
            return fail(error, capacity, "malformed QEMU XBUS completion mode");
    }
    emu_trace_event_t event = {
        .kind = "xbus_mailbox_complete", .icount = icount, .pc = pc,
        .has_value = 1, .value = result, .detail = phase,
    };
    emu_trace_info_int(&event.info, "irq", irq);
    emu_trace_info_int(&event.info, "result", result);
    emu_trace_info_int(&event.info, "status_before", status);
    emu_trace_info_int(&event.info, "transaction_id", (long)transaction);
    if (has_mode) {
        emu_trace_info_int(&event.info, "control", control);
        emu_trace_info_bool(&event.info, "async", irq_mode);
    }
    return emit(sink, &event, error, capacity);
}

static int parse_audio(emu_trace_sink_t *sink, const char *line,
                       char *error, size_t capacity) {
    unsigned long long tick, icount, sequence;
    unsigned pc, command, kind, depth;
    char phase[24], outcome[32], payload[257] = "";
    int used = 0;
    if (sscanf(line, "x55_audio_packet tick=%llu icount=%llu pc=0x%x phase=%23s command=0x%x kind=%u sequence=%llu depth=%u outcome=%31s bytes=%n",
               &tick, &icount, &pc, phase, &command, &kind, &sequence,
               &depth, outcome, &used) != 9 || !used || pc > 0xffffff ||
        command > 0xffff || kind > 2 || depth > 8 ||
        (!strcmp(phase, "accepted") && !depth))
        return fail(error, capacity, "malformed QEMU audio packet record");
    size_t n = strcspn(line + used, "\r\n");
    if (n > 256 || (n & 1) || strspn(line + used, "0123456789abcdef") != n ||
        strspn(line + used + n, "\r\n") != strlen(line + used + n))
        return fail(error, capacity, "malformed QEMU audio packet payload");
    if (strcmp(phase, "command") && strcmp(phase, "retained") &&
        strcmp(phase, "replaced") && strcmp(phase, "accepted") &&
        strcmp(phase, "retry") && strcmp(phase, "released") &&
        strcmp(phase, "source") && strcmp(phase, "drain"))
        return fail(error, capacity, "unknown QEMU audio packet phase");
    memcpy(payload, line + used, n);
    emu_trace_event_t event = {
        .kind = "xbus_audio_packet", .icount = icount, .pc = pc,
        .has_addr = 1, .addr = !strcmp(phase, "command") ? 0xe836 : 0xeb80,
        .detail = phase,
    };
    emu_trace_info_int(&event.info, "tick", tick);
    emu_trace_info_int(&event.info, "command", command);
    emu_trace_info_str(&event.info, "phase", phase);
    emu_trace_info_str(&event.info, "profile", "xbus-unknown1-v1");
    emu_trace_info_str(&event.info, "stream_kind",
                       kind == 1 ? "sampled" : kind == 2 ? "si3" : "none");
    emu_trace_info_int(&event.info, "sequence", sequence);
    emu_trace_info_int(&event.info, "queue_depth", depth);
    emu_trace_info_str(&event.info, "decoder_outcome", outcome);
    emu_trace_info_str(&event.info, "payload_hex", payload);
    emu_trace_info_int(&event.info, "payload_length", n / 2);
    return emit(sink, &event, error, capacity);
}

static int parse_pec(emu_trace_sink_t *sink, const char *line,
                     char *error, size_t capacity) {
    unsigned long long icount;
    unsigned pc, channel, src, dst, size, value, before, after;
    if (sscanf(line, "pmb7850_pec icount=%llu pc=0x%x channel=%u src=0x%x dst=0x%x size=%u value=0x%x count=%u->%u",
               &icount, &pc, &channel, &src, &dst, &size, &value,
               &before, &after) != 9)
        return fail(error, capacity, "malformed QEMU PEC trace record");
    emu_trace_event_t event = {
        .kind = "pec_transfer", .icount = icount, .pc = pc,
        .has_addr = 1, .addr = dst, .has_size = 1, .size = (int)size,
        .has_value = 1, .value = value, .detail = "",
    };
    emu_trace_info_int(&event.info, "channel", channel);
    emu_trace_info_int(&event.info, "count_after", after);
    emu_trace_info_int(&event.info, "count_before", before);
    emu_trace_info_int(&event.info, "dst", dst);
    emu_trace_info_int(&event.info, "src", src);
    return emit(sink, &event, error, capacity);
}

static int parse_keypad(emu_trace_sink_t *sink, const char *line,
                        char *error, size_t capacity) {
    unsigned long long icount, value;
    unsigned pc, offset, size, command, result, raw, logical;
    int write, acknowledged, pressed;
    char name[32];
    if (!strncmp(line, "siemens_keypad_deferred_release ",
                 sizeof "siemens_keypad_deferred_release " - 1u)) {
        unsigned index;
        char phase[16];
        if (sscanf(line, "siemens_keypad_deferred_release icount=%llu pc=0x%x name=%31s raw=0x%x index=%u phase=%15s",
                   &icount, &pc, name, &raw, &index, phase) != 6 ||
            index >= EMU_MAX_LOGICAL_KEYS ||
            (strcmp(phase, "queued") && strcmp(phase, "sampled") &&
             strcmp(phase, "cancelled") && strcmp(phase, "forced")))
            return fail(error, capacity, "malformed QEMU deferred key release");
        emu_trace_event_t event = {
            .kind = "keypad_deferred_release", .icount = icount, .pc = pc,
            .has_value = 1, .value = raw,
            .detail = "browser keypad release lifecycle",
        };
        emu_trace_info_str(&event.info, "button", name);
        emu_trace_info_int(&event.info, "key_index", index);
        emu_trace_info_str(&event.info, "phase", phase);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "siemens_keypad_startup_release ",
                 sizeof "siemens_keypad_startup_release " - 1u)) {
        if (sscanf(line, "siemens_keypad_startup_release icount=%llu pc=0x%x result=0x%x",
                   &icount, &pc, &result) != 3)
            return fail(error, capacity, "malformed QEMU startup release");
        emu_trace_event_t event = {
            .kind = "keypad_startup_release", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = 0xef1a, .has_size = 1, .size = 2,
            .has_value = 1, .value = result, .detail = "result consumed",
        };
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "siemens_keypad_matrix_select ",
                 sizeof "siemens_keypad_matrix_select " - 1u)) {
        unsigned column;
        int startup;
        if (sscanf(line, "siemens_keypad_matrix_select icount=%llu pc=0x%x column=0x%x command=0x%x result=0x%x startup=%d",
                   &icount, &pc, &column, &command, &result, &startup) != 6)
            return fail(error, capacity, "malformed QEMU matrix selection");
        emu_trace_event_t event = {
            .kind = "keypad_command", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = 0xffc6, .has_size = 1, .size = 2,
            .has_value = 1, .value = command, .detail = "matrix selection",
        };
        emu_trace_info_int(&event.info, "column", column);
        emu_trace_info_bool(&event.info, "matrix", 1);
        emu_trace_info_bool(&event.info, "startup", startup);
        emu_trace_info_bool(&event.info, "startup_power_pending", startup);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "siemens_keypad_input ",
                 sizeof "siemens_keypad_input " - 1u)) {
        if (sscanf(line, "siemens_keypad_input icount=%llu pc=0x%x name=%31s raw=0x%x logical=0x%x pressed=%d",
                   &icount, &pc, name, &raw, &logical, &pressed) != 6)
            return fail(error, capacity, "malformed QEMU keypad input record");
        emu_trace_event_t event = {
            .kind = "keypad_input", .icount = icount, .pc = pc,
            .has_value = 1, .value = raw, .detail = "",
        };
        emu_trace_info_str(&event.info, "button", name);
        emu_trace_info_int(&event.info, "logical_code", logical);
        emu_trace_info_bool(&event.info, "pressed", pressed);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "siemens_keypad_access icount=%llu pc=0x%x write=%d offset=0x%x size=%u value=0x%llx command=0x%x result=0x%x acknowledged=%d",
               &icount, &pc, &write, &offset, &size, &value, &command,
               &result, &acknowledged) != 9)
        return fail(error, capacity, "malformed QEMU keypad access record");
    if ((write && offset != 2) || (!write && offset >= 2)) return 0;
    emu_trace_event_t event = {
        .kind = write ? "keypad_command" : "keypad_result",
        .icount = icount, .pc = pc, .has_addr = 1, .addr = 0xef1a + offset,
        .has_size = 1, .size = (int)size, .has_value = 1,
        .value = write ? command : (uint32_t)value, .detail = "",
    };
    if (write)
        emu_trace_info_bool(&event.info, "startup_power_pending", !acknowledged);
    else
        emu_trace_info_int(&event.info, "command", command);
    return emit(sink, &event, error, capacity);
}

static int parse_tdma(emu_trace_sink_t *sink, const char *line,
                      char *error, size_t capacity) {
    unsigned long long icount;
    unsigned pc, before, after, top, compare, counter;
    int pending;
    char name[32];
    if (!strncmp(line, "pmb7850_tdma_irq ",
                 sizeof "pmb7850_tdma_irq " - 1u)) {
        if (sscanf(line, "pmb7850_tdma_irq icount=%llu pc=0x%x %31s compare=0x%x counter=0x%x already-pending=%d",
                   &icount, &pc, name, &compare, &counter, &pending) != 6)
            return fail(error, capacity, "malformed QEMU TDMA IRQ record");
        emu_trace_event_t event = {
            .kind = "tdma_irq", .icount = icount, .pc = pc,
            .has_value = 1, .value = counter, .detail = name,
        };
        emu_trace_info_bool(&event.info, "already_pending", pending);
        emu_trace_info_int(&event.info, "compare", compare);
        emu_trace_info_int(&event.info, "counter", counter);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "pmb7850_tdma icount=%llu pc=0x%x counter=0x%x->0x%x top=0x%x",
               &icount, &pc, &before, &after, &top) != 5)
        return fail(error, capacity, "malformed QEMU TDMA trace record");
    emu_trace_event_t event = {
        .kind = "tdma_tick", .icount = icount, .pc = pc,
        .has_value = 1, .value = after, .detail = "",
    };
    emu_trace_info_int(&event.info, "count_before", before);
    emu_trace_info_int(&event.info, "top", top);
    return emit(sink, &event, error, capacity);
}

static int parse_ssc(emu_trace_sink_t *sink, const char *line,
                     char *error, size_t capacity) {
    unsigned long long icount;
    unsigned pc, value, bits, divisor;
    int msb_first, queued;
    char phase[16];
    if (sscanf(line, "pmb7850_ssc icount=%llu pc=0x%x phase=%15s value=0x%x bits=%u msb-first=%d divisor=%u queued=%d",
               &icount, &pc, phase, &value, &bits, &msb_first, &divisor,
               &queued) != 8)
        return fail(error, capacity, "malformed QEMU SSC trace record");
    const char *kind = !strcmp(phase, "start") ? "ssc0_transfer_start" :
                       !strcmp(phase, "complete") ?
                       "ssc0_transfer_complete" : NULL;
    if (!kind) return fail(error, capacity, "unknown QEMU SSC phase");
    emu_trace_event_t event = {
        .kind = kind, .icount = icount, .pc = pc,
        .has_value = 1, .value = value, .detail = "",
    };
    if (!strcmp(phase, "start")) {
        emu_trace_info_int(&event.info, "baud_divisor", divisor);
        emu_trace_info_int(&event.info, "bits", bits);
        emu_trace_info_bool(&event.info, "queued", queued);
    } else {
        emu_trace_info_int(&event.info, "bits", bits);
        emu_trace_info_bool(&event.info, "queued", queued);
        emu_trace_info_int(&event.info, "rx", value);
    }
    (void)msb_first;
    return emit(sink, &event, error, capacity);
}

static int parse_lifecycle(emu_trace_sink_t *sink, const char *line,
                           char *error, size_t capacity) {
    unsigned long long icount;
    unsigned pc;
    char action[32];
    if (sscanf(line, "pmb7850_lifecycle icount=%llu pc=0x%x %31s",
               &icount, &pc, action) != 3)
        return fail(error, capacity, "malformed QEMU lifecycle record");
    emu_trace_event_t event = {
        .kind = "lifecycle", .icount = icount, .pc = pc,
        .detail = action,
    };
    return emit(sink, &event, error, capacity);
}

static int parse_sim(emu_trace_sink_t *sink, const char *line,
                     char *error, size_t capacity) {
    unsigned long long icount, sequence;
    unsigned pc, before, control, addr, value, pending;
    char phase[24], reason[32], direction[24], path[32];

    if (sscanf(line, "pmb7850_sim_control icount=%llu pc=0x%x before=0x%x control=0x%x phase=%23s",
               &icount, &pc, &before, &control, phase) == 5) {
        emu_trace_event_t event = {
            .kind = "sim_control", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = 0xef50, .has_size = 1, .size = 2,
            .has_value = 1, .value = control, .detail = phase,
        };
        emu_trace_info_int(&event.info, "before", before);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "pmb7850_sim_irq icount=%llu pc=0x%x addr=0x%x already-pending=%u reason=%31s",
               &icount, &pc, &addr, &pending, reason) == 5) {
        emu_trace_event_t event = {
            .kind = "sim_irq", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = addr, .detail = reason,
        };
        emu_trace_info_bool(&event.info, "already_pending", pending);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "pmb7850_sim_byte icount=%llu pc=0x%x direction=%23s value=0x%x sequence=%llu path=%31s",
               &icount, &pc, direction, &value, &sequence, path) == 6) {
        emu_trace_event_t event = {
            .kind = "sim_byte", .icount = icount, .pc = pc,
            .has_addr = 1,
            .addr = !strcmp(direction, "card_to_phone") ? 0xef5a : 0xef58,
            .has_size = 1, .size = 1, .has_value = 1, .value = value,
            .detail = path,
        };
        emu_trace_info_str(&event.info, "direction", direction);
        emu_trace_info_int(&event.info, "sequence", (long)sequence);
        return emit(sink, &event, error, capacity);
    }
    {
        unsigned status, response;
        unsigned long long command, selection;
        if (sscanf(line, "pmb7850_sim_apdu icount=%llu pc=0x%x command=0x%llx selection=0x%llx status=0x%x response=%u",
                   &icount, &pc, &command, &selection, &status,
                   &response) == 6) {
            unsigned cla = (command >> 32) & 0xff;
            unsigned ins = (command >> 24) & 0xff;
            unsigned p1 = (command >> 16) & 0xff;
            unsigned p2 = (command >> 8) & 0xff;
            unsigned p3 = command & 0xff;
            unsigned df_before = selection >> 48;
            unsigned ef_before = (selection >> 32) & 0xffff;
            unsigned df = (selection >> 16) & 0xffff;
            unsigned ef = selection & 0xffff;
            emu_trace_event_t event = {
                .kind = "sim_apdu", .icount = icount, .pc = pc,
                .has_size = 1,
                .size = 5 + ((ins == 0xa4 || ins == 0xd6 ||
                              ins == 0xdc || ins == 0x20) ? p3 : 0),
                .has_value = 1,
                .value = status, .detail = "GSM T=0 APDU",
            };
            emu_trace_info_int(&event.info, "cla", cla);
            emu_trace_info_int(&event.info, "ins", ins);
            emu_trace_info_int(&event.info, "p1", p1);
            emu_trace_info_int(&event.info, "p2", p2);
            emu_trace_info_int(&event.info, "p3", p3);
            if (command & (UINT64_C(1) << 56))
                emu_trace_info_int(&event.info, "requested_file",
                                   (command >> 40) & 0xffff);
            else
                emu_trace_info_null(&event.info, "requested_file");
            emu_trace_info_int(&event.info, "response_length", response);
            emu_trace_info_int(&event.info, "selected_df", df);
            emu_trace_info_int(&event.info, "selected_df_before", df_before);
            emu_trace_info_int(&event.info, "selected_ef", ef);
            emu_trace_info_int(&event.info, "selected_ef_before", ef_before);
            emu_trace_info_int(&event.info, "status_word", status);
            return emit(sink, &event, error, capacity);
        }
    }
    return fail(error, capacity, "malformed QEMU SIM trace record");
}

static int parse_flash(emu_trace_sink_t *sink, const char *line,
                       char *error, size_t capacity) {
    unsigned long long icount, value;
    unsigned pc, offset, size, bytes, status;
    int valid, result;
    char view[16], mode[32], phase[32], buffer[32];
    emu_trace_event_t event = {.icount = 0, .pc = 0, .detail = ""};
    if (!strncmp(line, "m58lw064d_command ",
                 sizeof "m58lw064d_command " - 1u)) {
        if (sscanf(line, "m58lw064d_command icount=%llu pc=0x%x view=%15s offset=0x%x size=%u value=0x%llx mode=%31s status=0x%x phase=%31s buffer=%31s",
                   &icount, &pc, view, &offset, &size, &value, mode,
                   &status, phase, buffer) != 10)
            return fail(error, capacity, "malformed QEMU flash command record");
        event = (emu_trace_event_t){
            .kind = "flash_command", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = offset, .has_size = 1,
            .size = (int)size, .has_value = 1, .value = (uint32_t)value,
            .detail = phase,
        };
        emu_trace_info_str(&event.info, "mode", mode);
        emu_trace_info_str(&event.info, "operation", phase);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "m58lw064d_program ",
                 sizeof "m58lw064d_program " - 1u)) {
        if (sscanf(line, "m58lw064d_program icount=%llu pc=0x%x offset=0x%x size=%u value=0x%llx valid=%d status=0x%x",
                   &icount, &pc, &offset, &size, &value, &valid, &status) != 7)
            return fail(error, capacity, "malformed QEMU flash program record");
        event = (emu_trace_event_t){
            .kind = valid ? "flash_program_start" : "flash_rejected_command",
            .icount = icount, .pc = pc, .has_addr = 1, .addr = offset,
            .has_size = 1, .size = (int)size, .has_value = 1,
            .value = (uint32_t)value, .detail = "program",
        };
        emu_trace_info_int(&event.info, "operation_offset", offset);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "m58lw064d_erase ",
                 sizeof "m58lw064d_erase " - 1u)) {
        if (sscanf(line, "m58lw064d_erase icount=%llu pc=0x%x offset=0x%x bytes=0x%x status=0x%x",
                   &icount, &pc, &offset, &bytes, &status) != 5)
            return fail(error, capacity, "malformed QEMU flash erase record");
        event = (emu_trace_event_t){
            .kind = "flash_erase_start", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = offset, .has_size = 1,
            .size = (int)bytes, .detail = "erase",
        };
        emu_trace_info_int(&event.info, "operation_offset", offset);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "m58lw064d_persistence ",
                 sizeof "m58lw064d_persistence " - 1u)) {
        if (sscanf(line, "m58lw064d_persistence icount=%llu pc=0x%x offset=0x%x bytes=0x%x result=%d",
                   &icount, &pc, &offset, &bytes, &result) != 5)
            return fail(error, capacity, "malformed QEMU flash persistence record");
        event = (emu_trace_event_t){
            .kind = "flash_program_complete", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = offset, .has_size = 1,
            .size = (int)bytes, .has_value = 1, .value = (uint32_t)result,
            .detail = "persistence",
        };
        emu_trace_info_int(&event.info, "operation_offset", offset);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "m58lw064d_reset ",
                 sizeof "m58lw064d_reset " - 1u)) {
        if (sscanf(line, "m58lw064d_reset icount=%llu pc=0x%x reset",
                   &icount, &pc) != 2)
            return fail(error, capacity, "malformed QEMU flash reset record");
        event = (emu_trace_event_t){
            .kind = "flash_status", .icount = icount, .pc = pc,
            .detail = "reset",
        };
        return emit(sink, &event, error, capacity);
    }
    /* Reads and buffer bookkeeping are native-only: their semantics do not
     * correspond one-to-one with a shared event kind. */
    return 0;
}

static int parse_lcd(emu_trace_sink_t *sink, const char *line,
                     char *error, size_t capacity) {
    unsigned value, x, y, bank, bytes;
    unsigned long long sequence, digest;
    int extended, asserted, driven, level;
    char model[32], name[64], disposition[16];
    emu_trace_event_t event = {.icount = 0, .pc = 0, .detail = ""};

    if (!strncmp(line, "x55_lcd_sweep_", sizeof "x55_lcd_sweep_" - 1)) {
        unsigned long long icount, total_bytes, writes, corners;
        unsigned pc;
        char reason[24];
        if (sscanf(line, "x55_lcd_sweep_frame icount=%llu pc=0x%x model=%31s sequence=%llu bytes=%llu writes=%llu corners=0x%llx reason=%23s digest=0x%llx",
                   &icount, &pc, model, &sequence, &total_bytes, &writes,
                   &corners, reason, &digest) == 9) {
            if (strcmp(reason, "window_wrap") && strcmp(reason, "viewport_complete") &&
                strcmp(reason, "origin_restart") && strcmp(reason, "fallback"))
                return fail(error, capacity, "invalid QEMU LCD boundary");
            event.kind = "lcd_frame";
            emu_trace_info_int(&event.info, "storage_writes", (long)writes);
            emu_trace_info_str(&event.info, "boundary_reason", reason);
            emu_trace_info_int(&event.info, "start_x", corners >> 48);
            emu_trace_info_int(&event.info, "start_y", (corners >> 32) & 0xffff);
            emu_trace_info_int(&event.info, "end_x", (corners >> 16) & 0xffff);
            emu_trace_info_int(&event.info, "end_y", corners & 0xffff);
            emu_trace_info_bool(&event.info, "flushed", false);
        } else if (sscanf(line, "x55_lcd_sweep_transaction icount=%llu pc=0x%x model=%31s sequence=%llu bytes=%llu x=%u y=%u disposition=%15s",
                          &icount, &pc, model, &sequence, &total_bytes,
                          &x, &y, disposition) == 8) {
            if (strcmp(disposition, "held") && strcmp(disposition, "committed"))
                return fail(error, capacity, "invalid QEMU LCD disposition");
            event.kind = "lcd_transaction";
            emu_trace_info_str(&event.info, "disposition", disposition);
            emu_trace_info_int(&event.info, "x", x);
            emu_trace_info_int(&event.info, "y", y);
        } else {
            return fail(error, capacity, "malformed QEMU LCD sweep record");
        }
        event.icount = icount;
        event.pc = pc;
        event.detail = model;
        event.value = (uint32_t)sequence;
        event.has_value = 1;
        emu_trace_info_int(&event.info, "data_bytes", (long)total_bytes);
        emu_trace_info_int(&event.info, "sequence", (long)sequence);
        return emit(sink, &event, error, capacity);
    }

    if (sscanf(line, "pcf8813_transaction sequence=%llu data-bytes=%u x=%u y=%u disposition=%15s",
               &sequence, &bytes, &x, &y, disposition) == 5) {
        event.kind = "lcd_transaction";
        event.value = (uint32_t)sequence;
        event.has_value = 1;
        event.detail = "pcf8813";
        emu_trace_info_int(&event.info, "data_bytes", bytes);
        emu_trace_info_str(&event.info, "disposition", disposition);
        emu_trace_info_int(&event.info, "sequence", (long)sequence);
        emu_trace_info_int(&event.info, "x", x);
        emu_trace_info_int(&event.info, "y", y);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "hm17_transaction %31s sequence=%llu bytes=%u x=%u y=%u disposition=%15s",
               model, &sequence, &bytes, &x, &y, disposition) == 6) {
        event.kind = "lcd_transaction";
        event.value = (uint32_t)sequence;
        event.has_value = 1;
        event.detail = model;
        emu_trace_info_int(&event.info, "data_bytes", bytes);
        emu_trace_info_str(&event.info, "disposition", disposition);
        emu_trace_info_int(&event.info, "sequence", (long)sequence);
        emu_trace_info_int(&event.info, "x", x);
        emu_trace_info_int(&event.info, "y", y);
        return emit(sink, &event, error, capacity);
    }

    if (sscanf(line, "pcf8813_frame sequence=%llu data-bytes=%u digest=0x%llx",
               &sequence, &bytes, &digest) == 3) {
        event.kind = "lcd_frame";
        event.value = (uint32_t)sequence;
        event.has_value = 1;
        event.detail = "pcf8813";
        emu_trace_info_int(&event.info, "data_bytes", bytes);
        emu_trace_info_bool(&event.info, "flushed", false);
        emu_trace_info_int(&event.info, "sequence", (long)sequence);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "hm17_frame %31s frame=%llu bytes=%u digest=0x%llx",
               model, &sequence, &bytes, &digest) == 4 ||
        sscanf(line, "x55_color_lcd_frame %31s frame=%llu bytes=%u digest=0x%llx",
               model, &sequence, &bytes, &digest) == 4) {
        event.kind = "lcd_frame";
        event.value = (uint32_t)sequence;
        event.has_value = 1;
        event.detail = model;
        emu_trace_info_int(&event.info, "data_bytes", bytes);
        emu_trace_info_bool(&event.info, "flushed", false);
        emu_trace_info_int(&event.info, "sequence", (long)sequence);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "pcf8813_select selected=%d", &level) == 1) {
        event.kind = "lcd_select";
        event.value = (uint32_t)level;
        event.has_value = 1;
        event.detail = "pcf8813";
        emu_trace_info_bool(&event.info, "selected", level);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "pcf8813_command value=0x%x extended=%d x=%u y=%u name=%63s",
               &value, &extended, &x, &y, name) == 5) {
        event.kind = "lcd_command";
        event.value = value;
        event.has_value = 1;
        event.detail = name;
        emu_trace_info_int(&event.info, "command", value);
        emu_trace_info_bool(&event.info, "extended", extended);
        emu_trace_info_int(&event.info, "x", x);
        emu_trace_info_int(&event.info, "y", y);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "hm17_command %31s command=0x%x bank=%u x=%u y=%u",
               model, &value, &bank, &x, &y) == 5) {
        event.kind = "lcd_command";
        event.value = value;
        event.has_value = 1;
        event.detail = model;
        emu_trace_info_int(&event.info, "bank", bank);
        emu_trace_info_int(&event.info, "command", value);
        emu_trace_info_int(&event.info, "x", x);
        emu_trace_info_int(&event.info, "y", y);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "x55_color_lcd_command %31s command=0x%x x=%u y=%u",
               model, &value, &x, &y) == 4) {
        event.kind = "lcd_command";
        event.value = value;
        event.has_value = 1;
        event.detail = model;
        emu_trace_info_int(&event.info, "command", value);
        emu_trace_info_int(&event.info, "x", x);
        emu_trace_info_int(&event.info, "y", y);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "pcf8813_data value=0x%x x=%u y=%u",
               &value, &x, &y) == 3 ||
        sscanf(line, "hm17_data %31s data=0x%x x=%u y=%u",
               model, &value, &x, &y) == 4 ||
        sscanf(line, "x55_color_lcd_data %31s data=0x%x x=%u y=%u",
               model, &value, &x, &y) == 4) {
        event.kind = "lcd_data";
        event.value = value;
        event.has_value = 1;
        event.detail = !strncmp(line, "pcf8813_", 8) ? "pcf8813" : model;
        emu_trace_info_int(&event.info, "x", x);
        emu_trace_info_int(&event.info, "y", y);
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "pcf8813_status value=0x%x", &value) == 1) {
        event.kind = "lcd_status";
        event.value = value;
        event.has_value = 1;
        event.detail = "pcf8813";
        return emit(sink, &event, error, capacity);
    }
    if (sscanf(line, "hm17_reset %31s asserted=%d driven=%d level=%d",
               model, &asserted, &driven, &level) == 4 ||
        sscanf(line, "x55_color_lcd_reset %31s asserted=%d driven=%d level=%d",
               model, &asserted, &driven, &level) == 4) {
        event.kind = "lcd_reset";
        event.value = (uint32_t)asserted;
        event.has_value = 1;
        event.detail = model;
        emu_trace_info_bool(&event.info, "asserted", asserted);
        emu_trace_info_bool(&event.info, "capcom_driven", driven);
        emu_trace_info_int(&event.info, "idle_level", level);
        return emit(sink, &event, error, capacity);
    }
    return fail(error, capacity, "malformed QEMU LCD trace record");
}

static int parse_am29_flash(emu_trace_sink_t *sink, const char *line,
                            char *error, size_t capacity) {
    unsigned long long icount, value;
    unsigned pc, offset, size, bytes, remaining_ticks;
    int valid, result, erase_active, erase_suspended, status;
    char view[16], mode[32], phase[32], action[32];
    emu_trace_event_t event = {.detail = ""};

    if (!strncmp(line, "am29lv_read ", sizeof "am29lv_read " - 1u)) {
        if (sscanf(line, "am29lv_read icount=%llu pc=0x%x view=%15s offset=0x%x size=%u value=0x%llx mode=%31s erase-active=%d erase-suspended=%d status=%d",
                   &icount, &pc, view, &offset, &size, &value, mode,
                   &erase_active, &erase_suspended, &status) != 10)
            return fail(error, capacity, "malformed QEMU AM29 read record");
        if (!status) return 0;
        event = (emu_trace_event_t){
            .kind = "flash_status", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = offset, .has_size = 1,
            .size = (int)size, .has_value = 1, .value = (uint32_t)value,
            .detail = "sector-erase-status",
        };
        emu_trace_info_str(&event.info, "mode", mode);
        emu_trace_info_str(&event.info, "subtype", "status");
        emu_trace_info_bool(&event.info, "erase_suspended", erase_suspended);
        return emit(sink, &event, error, capacity);
    }

    if (!strncmp(line, "am29lv_command ", sizeof "am29lv_command " - 1u)) {
        if (sscanf(line, "am29lv_command icount=%llu pc=0x%x view=%15s offset=0x%x size=%u value=0x%llx mode=%31s phase=%31s",
                   &icount, &pc, view, &offset, &size, &value, mode,
                   phase) != 8)
            return fail(error, capacity, "malformed QEMU AM29 command record");
        event = (emu_trace_event_t){
            .kind = "flash_command", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = offset, .has_size = 1,
            .size = (int)size, .has_value = 1, .value = (uint32_t)value,
            .detail = phase,
        };
        emu_trace_info_str(&event.info, "mode", mode);
        emu_trace_info_str(&event.info, "operation", phase);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "am29lv_program ", sizeof "am29lv_program " - 1u)) {
        if (sscanf(line, "am29lv_program icount=%llu pc=0x%x offset=0x%x size=%u value=0x%llx valid=%d",
                   &icount, &pc, &offset, &size, &value, &valid) != 6)
            return fail(error, capacity, "malformed QEMU AM29 program record");
        event = (emu_trace_event_t){
            .kind = valid ? "flash_program_start" : "flash_rejected_command",
            .icount = icount, .pc = pc, .has_addr = 1, .addr = offset,
            .has_size = 1, .size = (int)size, .has_value = 1,
            .value = (uint32_t)value, .detail = "program",
        };
        emu_trace_info_int(&event.info, "operation_offset", offset);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "am29lv_erase ", sizeof "am29lv_erase " - 1u)) {
        if (sscanf(line, "am29lv_erase icount=%llu pc=0x%x action=%31s offset=0x%x remaining-ticks=%u",
                   &icount, &pc, action, &offset, &remaining_ticks) != 5)
            return fail(error, capacity, "malformed QEMU AM29 erase record");
        const char *kind = !strcmp(action, "start") ? "flash_erase_start" :
                           !strcmp(action, "complete") ? "flash_erase_complete" :
                           !strcmp(action, "suspend") ? "flash_erase_suspend" :
                           !strcmp(action, "resume") ? "flash_erase_resume" : NULL;
        if (!kind) return fail(error, capacity, "unknown QEMU AM29 erase action");
        event = (emu_trace_event_t){
            .kind = kind, .icount = icount, .pc = pc,
            .has_addr = 1, .addr = offset, .detail = "erase",
        };
        emu_trace_info_str(&event.info, "operation", "erase");
        emu_trace_info_int(&event.info, "operation_offset", offset);
        emu_trace_info_int(&event.info, "remaining_ticks", remaining_ticks);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "am29lv_persistence ", sizeof "am29lv_persistence " - 1u)) {
        if (sscanf(line, "am29lv_persistence icount=%llu pc=0x%x offset=0x%x bytes=0x%x result=%d",
                   &icount, &pc, &offset, &bytes, &result) != 5)
            return fail(error, capacity, "malformed QEMU AM29 persistence record");
        event = (emu_trace_event_t){
            .kind = "flash_program_complete", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = offset, .has_size = 1,
            .size = (int)bytes, .has_value = 1, .value = (uint32_t)result,
            .detail = "persistence",
        };
        emu_trace_info_int(&event.info, "operation_offset", offset);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "am29lv_reset ", sizeof "am29lv_reset " - 1u)) {
        if (sscanf(line, "am29lv_reset icount=%llu pc=0x%x reset",
                   &icount, &pc) != 2)
            return fail(error, capacity, "malformed QEMU AM29 reset record");
        event = (emu_trace_event_t){
            .kind = "flash_status", .icount = icount, .pc = pc,
            .detail = "reset",
        };
        return emit(sink, &event, error, capacity);
    }
    return 0;
}

static int parse_w30_flash(emu_trace_sink_t *sink, const char *line,
                           char *error, size_t capacity) {
    unsigned long long icount, value;
    long long remaining;
    unsigned pc, offset, size, bytes;
    int result;
    char view[16], mode[32], phase[32], operation[32], action[32];
    emu_trace_event_t event = {.detail = ""};

    if (!strncmp(line, "w30_command ", sizeof "w30_command " - 1u)) {
        if (sscanf(line, "w30_command icount=%llu pc=0x%x view=%15s offset=0x%x size=%u value=0x%llx mode=%31s phase=%31s operation=%31s",
                   &icount, &pc, view, &offset, &size, &value, mode,
                   phase, operation) != 9)
            return fail(error, capacity, "malformed QEMU W30 command record");
        event = (emu_trace_event_t){
            .kind = "flash_command", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = offset, .has_size = 1,
            .size = (int)size, .has_value = 1, .value = (uint32_t)value,
            .detail = phase,
        };
        emu_trace_info_str(&event.info, "mode", mode);
        emu_trace_info_str(&event.info, "operation", operation);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "w30_operation ", sizeof "w30_operation " - 1u)) {
        if (sscanf(line, "w30_operation icount=%llu pc=0x%x action=%31s operation=%31s offset=0x%x remaining-ns=%lld",
                   &icount, &pc, action, operation, &offset, &remaining) != 6)
            return fail(error, capacity, "malformed QEMU W30 operation record");
        const char *kind = !strcmp(operation, "erase") ?
            (!strcmp(action, "start") ? "flash_erase_start" :
             !strcmp(action, "complete") ? "flash_erase_complete" :
             !strcmp(action, "suspend") ? "flash_erase_suspend" :
             !strcmp(action, "resume") ? "flash_erase_resume" : NULL) :
            (!strcmp(action, "start") ? "flash_program_start" :
             !strcmp(action, "complete") ? "flash_program_complete" :
             !strcmp(action, "suspend") ? "flash_program_suspend" :
             !strcmp(action, "resume") ? "flash_program_resume" : NULL);
        if (!kind) return fail(error, capacity, "unknown QEMU W30 operation");
        event = (emu_trace_event_t){
            .kind = kind, .icount = icount, .pc = pc,
            .has_addr = 1, .addr = offset, .detail = operation,
        };
        emu_trace_info_str(&event.info, "operation", operation);
        emu_trace_info_int(&event.info, "operation_offset", offset);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "w30_persistence ", sizeof "w30_persistence " - 1u)) {
        if (sscanf(line, "w30_persistence icount=%llu pc=0x%x offset=0x%x bytes=0x%x result=%d",
                   &icount, &pc, &offset, &bytes, &result) != 5)
            return fail(error, capacity, "malformed QEMU W30 persistence record");
        event = (emu_trace_event_t){
            .kind = "flash_program_complete", .icount = icount, .pc = pc,
            .has_addr = 1, .addr = offset, .has_size = 1,
            .size = (int)bytes, .has_value = 1, .value = (uint32_t)result,
            .detail = "persistence",
        };
        emu_trace_info_int(&event.info, "operation_offset", offset);
        return emit(sink, &event, error, capacity);
    }
    if (!strncmp(line, "w30_reset ", sizeof "w30_reset " - 1u)) {
        if (sscanf(line, "w30_reset icount=%llu pc=0x%x reset",
                   &icount, &pc) != 2)
            return fail(error, capacity, "malformed QEMU W30 reset record");
        event = (emu_trace_event_t){
            .kind = "flash_status", .icount = icount, .pc = pc,
            .detail = "reset",
        };
        return emit(sink, &event, error, capacity);
    }
    return 0; /* Reads are native-only. */
}

int emu_qemu_trace_parse_line(emu_trace_sink_t *sink, const char *line,
                              char *error, size_t capacity) {
    if (!sink || !line) return fail(error, capacity, "invalid QEMU trace input");
    if (!strncmp(line, "siemens_host_service ", 21))
        return parse_host_service(sink, line, error, capacity);
    if (!strncmp(line, "pmb7850_serial ", sizeof "pmb7850_serial " - 1u))
        return parse_serial(sink, line, error, capacity);
    if (!strncmp(line, "pmb7850_adc_conversion ",
                 sizeof "pmb7850_adc_conversion " - 1u) ||
        !strncmp(line, "siemens_battery_state ",
                 sizeof "siemens_battery_state " - 1u))
        return parse_battery(sink, line, error, capacity);
    if (!strncmp(line, "siemens_xbus_", sizeof "siemens_xbus_" - 1u))
        return parse_xbus(sink, line, error, capacity);
    if (!strncmp(line, "x55_audio_packet ", sizeof "x55_audio_packet " - 1u))
        return parse_audio(sink, line, error, capacity);
    if (!strncmp(line, "pmb7850_pec ", sizeof "pmb7850_pec " - 1u))
        return parse_pec(sink, line, error, capacity);
    if (!strncmp(line, "siemens_keypad_", sizeof "siemens_keypad_" - 1u))
        return parse_keypad(sink, line, error, capacity);
    if (!strncmp(line, "pmb7850_tdma", sizeof "pmb7850_tdma" - 1u))
        return parse_tdma(sink, line, error, capacity);
    if (!strncmp(line, "pmb7850_ssc ", sizeof "pmb7850_ssc " - 1u))
        return parse_ssc(sink, line, error, capacity);
    if (!strncmp(line, "pcf8813_", sizeof "pcf8813_" - 1u) ||
        !strncmp(line, "hm17_", sizeof "hm17_" - 1u) ||
        !strncmp(line, "x55_lcd_sweep_", sizeof "x55_lcd_sweep_" - 1u) ||
        !strncmp(line, "x55_color_lcd_",
                 sizeof "x55_color_lcd_" - 1u))
        return parse_lcd(sink, line, error, capacity);
    if (!strncmp(line, "pmb7850_lifecycle ",
                 sizeof "pmb7850_lifecycle " - 1u))
        return parse_lifecycle(sink, line, error, capacity);
    if (!strncmp(line, "pmb7850_sim_", sizeof "pmb7850_sim_" - 1u))
        return parse_sim(sink, line, error, capacity);
    if (!strncmp(line, "m58lw064d_", sizeof "m58lw064d_" - 1u))
        return parse_flash(sink, line, error, capacity);
    if (!strncmp(line, "am29lv_", sizeof "am29lv_" - 1u))
        return parse_am29_flash(sink, line, error, capacity);
    if (!strncmp(line, "w30_", sizeof "w30_" - 1u))
        return parse_w30_flash(sink, line, error, capacity);
    return 0;
}

static int read_exact(FILE *file, void *output, size_t size) {
    return fread(output, 1, size, file) == size ? 0 : -1;
}

static int skip_exact(FILE *file, size_t size) {
    unsigned char buffer[1024];
    while (size) {
        size_t chunk = size < sizeof buffer ? size : sizeof buffer;
        if (read_exact(file, buffer, chunk)) return -1;
        size -= chunk;
    }
    return 0;
}

int emu_qemu_trace_validate_simple(const char *path, char *error,
                                   size_t capacity) {
    static const uint64_t header_id = UINT64_MAX;
    static const uint64_t magic = UINT64_C(0xf2b177cb0aa429b4);
    FILE *file = fopen(path, "rb");
    if (!file) return fail(error, capacity, "cannot open QEMU native trace");
    uint64_t header[3];
    if (read_exact(file, header, sizeof header)) {
        fclose(file);
        return fail(error, capacity, "truncated QEMU native trace header");
    }
    if (header[0] != header_id || header[1] != magic || header[2] != 4) {
        if (error && capacity)
            snprintf(error, capacity,
                     "incompatible QEMU native trace header %016llx/%016llx/%llu",
                     (unsigned long long)header[0],
                     (unsigned long long)header[1],
                     (unsigned long long)header[2]);
        fclose(file);
        return -1;
    }
    for (;;) {
        uint64_t type;
        size_t got = fread(&type, 1, sizeof type, file);
        if (!got) break;
        if (got != sizeof type) {
            fclose(file);
            return fail(error, capacity, "truncated QEMU native trace record");
        }
        if (type == 0) {
            uint64_t event_id;
            uint32_t name_size;
            if (read_exact(file, &event_id, sizeof event_id) ||
                read_exact(file, &name_size, sizeof name_size) ||
                name_size > 4096 || skip_exact(file, name_size)) {
                fclose(file);
                return fail(error, capacity, "truncated QEMU trace mapping");
            }
        } else {
            uint64_t timestamp;
            uint32_t record_size, pid;
            if (type == UINT64_C(0xfffffffffffffffe)) {
                fclose(file);
                return fail(error, capacity, "QEMU native trace dropped records");
            }
            if (read_exact(file, &timestamp, sizeof timestamp) ||
                read_exact(file, &record_size, sizeof record_size) ||
                read_exact(file, &pid, sizeof pid) || record_size < 24u ||
                skip_exact(file, record_size - 24u)) {
                fclose(file);
                return fail(error, capacity, "truncated QEMU native trace event");
            }
        }
    }
    if (ferror(file)) {
        fclose(file);
        return fail(error, capacity, "cannot read QEMU native trace");
    }
    if (fclose(file)) return fail(error, capacity, "cannot close QEMU native trace");
    if (error && capacity) error[0] = 0;
    return 0;
}
