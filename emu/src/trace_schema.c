#include "emu_trace_schema.h"

#include <stdio.h>
#include <string.h>

#define KI(n) {n, EMU_TRACE_SCHEMA_I64}
#define KB(n) {n, EMU_TRACE_SCHEMA_BOOL}
#define KS(n) {n, EMU_TRACE_SCHEMA_STR}
#define KIN(n) {n, EMU_TRACE_SCHEMA_I64 | EMU_TRACE_SCHEMA_NULL}
#define KBN(n) {n, EMU_TRACE_SCHEMA_BOOL | EMU_TRACE_SCHEMA_NULL}
#define KSN(n) {n, EMU_TRACE_SCHEMA_STR | EMU_TRACE_SCHEMA_NULL}
#define KEYS(a) a, sizeof(a) / sizeof((a)[0])
#define KIND(n, m, a) {n, m, KEYS(a)}
#define EMPTY_KIND(n, m) {n, m, NULL, 0}

static const emu_trace_schema_key_t MEM_KEYS[] = {
    KB("bus_active"), KIN("bus_end"), KI("bus_index"), KS("bus_kind"),
    KIN("bus_start"), KB("bus_window_active"), KI("chip_index"),
    KS("chip_name"), KI("chip_offset"), KS("device"), KS("mode"),
    KS("model"), KI("physical_offset"), KS("subtype"), KB("xbus_active"),
    KB("xbus_bus_active"), KIN("xbus_end"), KIN("xbus_start"),
    KI("xbus_window"), KB("xbus_xpen"), KB("xbus_xper_enabled"),
};
static const emu_trace_schema_key_t SERIAL_HISTORY_KEYS[] = {
    KS("action"), KS("boundary"), KB("changed"), KS("cursor"), KS("end"),
    KI("result"), KS("start"), KS("subscription"), KS("tail"),
};
static const emu_trace_schema_key_t SERIAL_KEYS[] = {KSN("char")};
static const emu_trace_schema_key_t XBUS_WINDOW_KEYS[] = {
    KB("active"), KB("bus_active"), KB("configured"), KIN("end"),
    KI("index"), KB("reserved"), KIN("size"), KIN("start"),
    KI("xadrs"), KI("xbcon"), KB("xpen"), KB("xper_enabled"),
};
static const emu_trace_schema_key_t BUS_WINDOW_KEYS[] = {
    KB("active"), KI("btyp"), KB("bus_active"), KB("configured"),
    KI("control"), KI("control_addr"), KI("csren"), KI("cswen"),
    KIN("end"), KI("ewen"), KI("index"), KS("kind"), KI("mctc"),
    KI("mttc"), KI("rdyen"), KB("reserved"), KI("rgsad"), KI("rgsz"),
    KI("rwdc"), KI("selector"), KI("selector_addr"), KIN("size"),
    KIN("start"), KBN("xpen"), KBN("xper_enabled"),
};
static const emu_trace_schema_key_t XBUS_ACCESS_KEYS[] = {
    KS("access"), KB("xbus_active"), KB("xbus_bus_active"),
    KIN("xbus_end"), KIN("xbus_start"), KI("xbus_window"),
    KB("xbus_xpen"), KB("xbus_xper_enabled"),
};
static const emu_trace_schema_key_t XBUS_MAILBOX_KEYS[] = {
    KB("already_pending"), KB("async"), KI("battery_level"), KB("charger_query"),
    KB("charging"), KI("command"), KB("complete_bit1"), KB("complete_bit2"),
    KI("control"), KI("deadline"), KI("index"), KI("irq"), KB("irq_asserted"),
    KI("irq_value"), KS("phase"), KI("request"), KI("result"),
    KI("status_before"), KI("trailer"), KI("transaction_id"),
};
static const emu_trace_schema_key_t XBUS_AUDIO_KEYS[] = {
    KI("command"), KS("completion_reason"), KS("decoder_outcome"),
    KI("new_token"), KI("old_token"), KS("payload_hex"),
    KI("payload_length"), KS("phase"), KS("profile"), KI("queue_depth"),
    KI("sequence"), KS("stream_kind"), KI("tick"),
};
static const emu_trace_schema_key_t PORT_KEYS[] = {
    KI("bit"), KS("edge"), KI("level"), KS("port"),
};
static const emu_trace_schema_key_t PEC_KEYS[] = {
    KI("channel"), KI("count_after"), KI("count_before"), KI("counter"),
    KI("dst"), KS("eop"), KI("ic"), KS("mode"), KI("src"),
};
static const emu_trace_schema_key_t LCD_SELECT_KEYS[] = {
    KI("level"), KB("reset"), KB("selected"),
};
static const emu_trace_schema_key_t LCD_COMMAND_KEYS[] = {
    KI("bank"), KI("command"), KI("count"), KB("extended"),
    KI("frame_bits"), KB("known"), KB("msb_first"), KI("parameter_index"),
    KI("pending_window_command"), KI("pending_window_index"), KB("reset"),
    KI("window_end_x"), KI("window_end_y"), KI("window_start_x"),
    KI("window_start_y"), KI("write_index"), KI("x"), KI("y"),
};
static const emu_trace_schema_key_t LCD_DATA_KEYS[] = {
    KB("abs"), KI("bank"), KB("bgr_recorded_not_applied"), KI("colmod"),
    KB("color_256"), KI("command"), KB("fixed_pwm"), KB("glsb"),
    KB("high_speed"), KI("madctl"), KI("parameter_index"), KB("ref"),
    KB("swap"), KB("wls"), KI("x"), KI("y"),
};
static const emu_trace_schema_key_t LCD_RESET_KEYS[] = {
    KB("asserted"), KI("capcom_channel"), KB("capcom_driven"), KI("idle_level"),
};
static const emu_trace_schema_key_t LCD_FRAME_KEYS[] = {
    KS("boundary_reason"), KI("data_bytes"), KI("end_x"), KI("end_y"),
    KB("flushed"), KI("sequence"), KI("start_x"), KI("start_y"),
    KI("storage_writes"),
};
static const emu_trace_schema_key_t LCD_STATUS_KEYS[] = {
    KB("busy"), KI("device"), KB("display_on"), KI("manufacturer"),
};
static const emu_trace_schema_key_t LCD_TRANSACTION_KEYS[] = {
    KI("data_bytes"), KS("disposition"), KI("sequence"), KI("x"), KI("y"),
};
static const emu_trace_schema_key_t KEYPAD_COMMAND_KEYS[] = {
    KI("button_count"), KI("column"), KB("handled"), KB("matrix"),
    KB("startup"), KB("startup_power_pending"),
};
static const emu_trace_schema_key_t KEYPAD_RESULT_KEYS[] = {KI("command")};
static const emu_trace_schema_key_t KEYPAD_INPUT_KEYS[] = {
    KI("activity_ic_addr"), KB("activity_irq_requested"), KS("button"),
    KI("logical_code"), KB("pressed"),
};
static const emu_trace_schema_key_t KEYPAD_DEFERRED_KEYS[] = {
    KS("button"), KB("changed"), KB("down"), KI("holders"), KI("key_index"),
    KS("owner"), KB("pending"), KS("phase"), KI("result"),
};
static const emu_trace_schema_key_t TDMA_TICK_KEYS[] = {
    KI("count_before"), KI("top"),
};
static const emu_trace_schema_key_t TDMA_IRQ_KEYS[] = {
    KB("already_pending"), KI("compare"), KI("counter"), KI("trap"),
};
static const emu_trace_schema_key_t TWI_EVENT_KEYS[] = {
    KB("accepted"), KB("ack"), KI("address"), KI("data"), KS("event"),
    KB("read"), KI("register"),
};
static const emu_trace_schema_key_t SSC_START_KEYS[] = {
    KI("baud_divisor"), KI("bits"), KI("completion_tick"), KB("queued"),
    KI("start_tick"),
};
static const emu_trace_schema_key_t SSC_COMPLETE_KEYS[] = {
    KI("bits"), KB("queued"), KI("rx"), KI("start_tick"), KI("tx"),
};
static const emu_trace_schema_key_t SIM_CONTROL_KEYS[] = {KI("before")};
static const emu_trace_schema_key_t SIM_IRQ_KEYS[] = {KB("already_pending")};
static const emu_trace_schema_key_t SIM_BYTE_KEYS[] = {
    KS("direction"), KI("sequence"),
};
static const emu_trace_schema_key_t SIM_APDU_KEYS[] = {
    KI("cla"), KI("ins"), KI("p1"), KI("p2"), KI("p3"),
    KIN("requested_file"), KI("response_length"), KI("selected_df"),
    KI("selected_df_before"), KI("selected_ef"), KI("selected_ef_before"),
    KI("status_word"),
};
static const emu_trace_schema_key_t GSM_REQUEST_KEYS[] = {
    KB("accepted"), KI("completion_tick"), KI("sequence"),
};
static const emu_trace_schema_key_t GSM_RESPONSE_KEYS[] = {
    KI("arfcn"), KI("command"), KI("result_sequence"), KI("sequence"),
    KB("synchronized"), KI("word0"), KI("word1"),
};
static const emu_trace_schema_key_t GSM_REGISTRATION_KEYS[] = {
    KI("event"), KI("payload_offset"), KI("payload_segment"), KI("plmn_bcd"),
    KI("synchronized_responses"),
};
static const emu_trace_schema_key_t GSM_SIGNAL_KEYS[] = {
    KI("event"), KI("level"), KI("registration_event"),
};
static const emu_trace_schema_key_t GSM_CELL_STATE_KEYS[] = {
    KB("available"), KI("command"), KI("interrupt"), KI("level"),
    KI("measurement_index"), KI("plmn_bcd"), KI("quality"),
    KI("sequence"), KB("service_allowed"), KB("synchronized"),
};
static const emu_trace_schema_key_t GSM_L1_FIRMWARE_KEYS[] = {
    KIN("caller_pc"), KIN("event_id"), KIN("handle_offset"),
    KIN("handle_segment"), KIN("hardware_address"),
    KSN("hardware_surface"), KIN("hardware_value"),
    KIN("normalized_event"), KIN("object_offset"),
    KIN("object_segment"), KIN("owner"), KIN("payload_size"),
    KS("profile"), KIN("queue"), KIN("raw_event"),
    KIN("receiver_state"), KS("role"), KIN("search_status"),
    KIN("selector"), KIN("source"), KIN("state_before"),
    KIN("state_requested"), KIN("target_pc"), KIN("timer_delay"),
    KIN("value"),
};
static const emu_trace_schema_key_t BATTERY_STATE_KEYS[] = {
    KI("adc_raw"), KB("charging"), KI("level"), KI("millivolts"),
    KB("old_charging"), KI("old_level"), KS("source"),
};
static const emu_trace_schema_key_t BATTERY_SAMPLE_KEYS[] = {
    KI("adc_raw"), KI("channel"), KB("charging"), KI("level"),
    KI("millivolts"), KI("result0"), KI("result1"),
    KI("result_sequence"), KI("result_word"),
};
static const emu_trace_schema_key_t CAPCOM_KEYS[] = {
    KI("channel"), KS("edge"), KI("mode"), KS("reason"),
    KI("source_channel"), KS("timer"), KI("timer_value"), KS("unit"),
};
static const emu_trace_schema_key_t LIFECYCLE_KEYS[] = {
    KB("init_locked"), KB("rstout"), KB("rstout_changed"),
};
static const emu_trace_schema_key_t FLASH_KEYS[] = {
    KI("chip_index"), KS("chip_name"), KI("chip_offset"), KS("device"),
    KB("erase_suspended"), KS("mode"), KS("model"), KS("operation"),
    KI("operation_offset"), KB("operation_suspended"), KI("partition"),
    KI("remaining_ticks"), KI("sector_offset"), KS("subtype"), KI("tick"),
};
static const emu_trace_schema_key_t FIRMWARE_PATCH_KEYS[] = {
    KI("chip_index"), KI("chip_offset"), KS("expected"), KS("patch"),
    KS("provenance"), KS("replacement"), KS("status"),
};
static const emu_trace_schema_key_t EEPROM_ACCESS_KEYS[] = {
    KS("access"), KS("area"), KI("area_offset"), KS("attribution"),
    KIN("block_id"), KIN("block_offset"), KI("chip_index"),
    KS("chip_name"), KI("chip_offset"), KI("mapping_generation"),
    KS("model"), KIN("mutation_offset"), KIN("mutation_size"),
    KI("tick"),
};
static const emu_trace_schema_key_t EEPROM_MAP_KEYS[] = {
    KI("block_id"), KS("change"), KI("chip_index"), KS("chip_name"),
    KBN("current_active"), KIN("current_descriptor_offset"),
    KIN("current_length"), KIN("current_linear"),
    KIN("current_payload_offset"), KIN("current_version"),
    KI("mapping_generation"), KS("model"), KBN("previous_active"),
    KIN("previous_descriptor_offset"), KIN("previous_length"),
    KIN("previous_linear"), KIN("previous_payload_offset"),
    KIN("previous_version"), KI("tick"),
};

static const emu_trace_schema_kind_t EMU_TRACE_CATALOG[] = {
    KIND("battery_sample", EMU_TRACE_BATTERY_SAMPLE, BATTERY_SAMPLE_KEYS),
    KIND("battery_state", EMU_TRACE_BATTERY_STATE, BATTERY_STATE_KEYS),
    KIND("bus_window", EMU_TRACE_BUS_WINDOW, BUS_WINDOW_KEYS),
    KIND("capcom_output", EMU_TRACE_CAPCOM_OUTPUT, CAPCOM_KEYS),
    KIND("eeprom_access", EMU_TRACE_EEPROM_ACCESS, EEPROM_ACCESS_KEYS),
    KIND("eeprom_map", EMU_TRACE_EEPROM_MAP, EEPROM_MAP_KEYS),
    KIND("ef_mailbox_command", EMU_TRACE_KEYPAD_COMMAND, KEYPAD_COMMAND_KEYS),
    KIND("ef_mailbox_response", EMU_TRACE_KEYPAD_RESULT, KEYPAD_RESULT_KEYS),
    EMPTY_KIND("exec", EMU_TRACE_EXEC),
    KIND("firmware_patch", EMU_TRACE_FIRMWARE_PATCH, FIRMWARE_PATCH_KEYS),
    KIND("flash_command", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_erase_complete", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_erase_resume", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_erase_start", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_erase_suspend", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_erase_timer", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_lock_change", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_program_complete", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_program_resume", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_program_start", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_program_suspend", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_protection_complete", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_protection_program", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_rejected_command", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_status", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("flash_write_absorbed", EMU_TRACE_FLASH, FLASH_KEYS),
    KIND("gsm_cell_state", EMU_TRACE_GSM_CELL_STATE, GSM_CELL_STATE_KEYS),
    KIND("gsm_l1_firmware", EMU_TRACE_GSM_L1_FIRMWARE,
         GSM_L1_FIRMWARE_KEYS),
    KIND("gsm_registration", EMU_TRACE_GSM_REGISTRATION, GSM_REGISTRATION_KEYS),
    KIND("gsm_request", EMU_TRACE_GSM_REQUEST, GSM_REQUEST_KEYS),
    KIND("gsm_response", EMU_TRACE_GSM_RESPONSE, GSM_RESPONSE_KEYS),
    KIND("gsm_signal", EMU_TRACE_GSM_SIGNAL, GSM_SIGNAL_KEYS),
    KIND("input_owner", EMU_TRACE_INPUT_OWNER, KEYPAD_DEFERRED_KEYS),
    KIND("keypad_command", EMU_TRACE_KEYPAD_COMMAND, KEYPAD_COMMAND_KEYS),
    KIND("keypad_deferred_release", EMU_TRACE_KEYPAD_DEFERRED_RELEASE,
         KEYPAD_DEFERRED_KEYS),
    KIND("keypad_input", EMU_TRACE_KEYPAD_INPUT, KEYPAD_INPUT_KEYS),
    KIND("keypad_result", EMU_TRACE_KEYPAD_RESULT, KEYPAD_RESULT_KEYS),
    EMPTY_KIND("keypad_startup_release", EMU_TRACE_KEYPAD_RESULT),
    KIND("lcd_command", EMU_TRACE_LCD_COMMAND, LCD_COMMAND_KEYS),
    KIND("lcd_data", EMU_TRACE_LCD_DATA, LCD_DATA_KEYS),
    KIND("lcd_frame", EMU_TRACE_LCD_FRAME, LCD_FRAME_KEYS),
    KIND("lcd_reset", EMU_TRACE_LCD_RESET, LCD_RESET_KEYS),
    KIND("lcd_select", EMU_TRACE_LCD_SELECT, LCD_SELECT_KEYS),
    KIND("lcd_status", EMU_TRACE_LCD_STATUS, LCD_STATUS_KEYS),
    KIND("lcd_transaction", EMU_TRACE_LCD_TRANSACTION, LCD_TRANSACTION_KEYS),
    KIND("lifecycle", EMU_TRACE_LIFECYCLE, LIFECYCLE_KEYS),
    KIND("mem_read", EMU_TRACE_MEM_READ, MEM_KEYS),
    KIND("mem_write", EMU_TRACE_MEM_WRITE, MEM_KEYS),
    KIND("pec_transfer", EMU_TRACE_PEC_TRANSFER, PEC_KEYS),
    KIND("port_edge", EMU_TRACE_PORT_EDGE, PORT_KEYS),
    KIND("serial_history", EMU_TRACE_SERIAL_HISTORY, SERIAL_HISTORY_KEYS),
    KIND("serial_rx", EMU_TRACE_SERIAL_RX, SERIAL_KEYS),
    KIND("serial_tx", EMU_TRACE_SERIAL_TX, SERIAL_KEYS),
    EMPTY_KIND("sfr_read", EMU_TRACE_SFR_READ),
    EMPTY_KIND("sfr_write", EMU_TRACE_SFR_WRITE),
    KIND("sim_apdu", EMU_TRACE_SIM_APDU, SIM_APDU_KEYS),
    KIND("sim_byte", EMU_TRACE_SIM_BYTE, SIM_BYTE_KEYS),
    KIND("sim_control", EMU_TRACE_SIM_CONTROL, SIM_CONTROL_KEYS),
    KIND("sim_irq", EMU_TRACE_SIM_IRQ, SIM_IRQ_KEYS),
    KIND("ssc0_transfer_complete", EMU_TRACE_SSC0_TRANSFER_COMPLETE,
         SSC_COMPLETE_KEYS),
    KIND("ssc0_transfer_start", EMU_TRACE_SSC0_TRANSFER_START, SSC_START_KEYS),
    KIND("tdma_irq", EMU_TRACE_TDMA_IRQ, TDMA_IRQ_KEYS),
    KIND("tdma_tick", EMU_TRACE_TDMA_TICK, TDMA_TICK_KEYS),
    KIND("twi_event", EMU_TRACE_TWI_EVENT, TWI_EVENT_KEYS),
    EMPTY_KIND("unmapped", EMU_TRACE_UNMAPPED),
    EMPTY_KIND("unmapped_read", EMU_TRACE_UNMAPPED),
    KIND("xbus_access", EMU_TRACE_XBUS_ACCESS, XBUS_ACCESS_KEYS),
    KIND("xbus_audio_packet", EMU_TRACE_XBUS_AUDIO_PACKET, XBUS_AUDIO_KEYS),
    KIND("xbus_mailbox_complete", EMU_TRACE_XBUS_MAILBOX_COMPLETE,
         XBUS_MAILBOX_KEYS),
    KIND("xbus_window", EMU_TRACE_XBUS_WINDOW, XBUS_WINDOW_KEYS),
};

static void set_error(char *error, size_t cap, const char *format,
                      const char *kind, const char *key, unsigned type) {
    if (!error || !cap) return;
    snprintf(error, cap, format, kind ? kind : "", key ? key : "", type);
}

const emu_trace_schema_kind_t *emu_trace_schema_catalog(size_t *count) {
    if (count) *count = sizeof EMU_TRACE_CATALOG / sizeof EMU_TRACE_CATALOG[0];
    return EMU_TRACE_CATALOG;
}

const emu_trace_schema_kind_t *emu_trace_schema_find_kind(const char *kind) {
    if (!kind) return NULL;
    size_t lo = 0, hi = sizeof EMU_TRACE_CATALOG / sizeof EMU_TRACE_CATALOG[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(kind, EMU_TRACE_CATALOG[mid].kind);
        if (!cmp) return &EMU_TRACE_CATALOG[mid];
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

emu_trace_mask_t emu_trace_schema_kind_mask(const char *kind) {
    const emu_trace_schema_kind_t *entry = emu_trace_schema_find_kind(kind);
    return entry ? entry->mask : 0;
}

static int valid_name(const char *name) {
    if (!name || !((*name >= 'a' && *name <= 'z') || *name == '_'))
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_'))
            return 0;
    return 1;
}

int emu_trace_schema_validate_catalog(char *error, size_t cap) {
    emu_trace_mask_t represented = 0;
    size_t count = sizeof EMU_TRACE_CATALOG / sizeof EMU_TRACE_CATALOG[0];
    for (size_t i = 0; i < count; i++) {
        const emu_trace_schema_kind_t *kind = &EMU_TRACE_CATALOG[i];
        if (!valid_name(kind->kind) || !kind->mask ||
            (i && strcmp(EMU_TRACE_CATALOG[i - 1].kind, kind->kind) >= 0)) {
            set_error(error, cap, "invalid trace kind %s", kind->kind, NULL, 0);
            return -1;
        }
        represented |= kind->mask;
        for (size_t j = 0; j < kind->key_n; j++) {
            const emu_trace_schema_key_t *key = &kind->keys[j];
            if (!valid_name(key->name) || !key->types ||
                (key->types & ~(EMU_TRACE_SCHEMA_I64 | EMU_TRACE_SCHEMA_BOOL |
                                EMU_TRACE_SCHEMA_NULL | EMU_TRACE_SCHEMA_STR)) ||
                (j && strcmp(kind->keys[j - 1].name, key->name) >= 0)) {
                set_error(error, cap, "invalid trace field %s.%s",
                          kind->kind, key->name, 0);
                return -1;
            }
        }
    }
    if (represented != EMU_TRACE_MASK_ALL) {
        set_error(error, cap, "trace catalog mask mismatch %s", "", NULL, 0);
        return -1;
    }
    if (error && cap) error[0] = 0;
    return 0;
}

static const emu_trace_schema_key_t *find_key(const emu_trace_schema_kind_t *kind,
                                          const char *name) {
    size_t lo = 0, hi = kind->key_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, kind->keys[mid].name);
        if (!cmp) return &kind->keys[mid];
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

int emu_trace_schema_validate_event(const emu_trace_event_t *event,
                                char *error, size_t cap) {
    const char *kind_name = event && event->kind ? event->kind : "";
    const emu_trace_schema_kind_t *kind = emu_trace_schema_find_kind(kind_name);
    if (!event || !kind || event->info.n < 0 || event->info.n > 32) {
        set_error(error, cap, "undeclared trace kind %s",
                  kind_name, NULL, 0);
        return -1;
    }
    for (int i = 0; i < event->info.n; i++) {
        const emu_trace_field_t *kv = &event->info.kv[i];
        const char *key_name = kv->key ? kv->key : "";
        const emu_trace_schema_key_t *key = find_key(kind, key_name);
        unsigned type = kv->kind <= EMU_TRACE_VALUE_STRING ? 1u << kv->kind : 0;
        if (!key || !type || !(key->types & type)) {
            set_error(error, cap, "undeclared trace field %s.%s type %u",
                      kind_name, key_name, (unsigned)kv->kind);
            return -1;
        }
        for (int j = 0; j < i; j++) {
            const char *prior = event->info.kv[j].key
                              ? event->info.kv[j].key : "";
            if (!strcmp(prior, key_name)) {
                set_error(error, cap, "duplicate trace field %s.%s",
                          kind_name, key_name, 0);
                return -1;
            }
        }
    }
    if (error && cap) error[0] = 0;
    return 0;
}

#undef KI
#undef KB
#undef KS
#undef KIN
#undef KBN
#undef KSN
#undef KEYS
#undef KIND
#undef EMPTY_KIND
