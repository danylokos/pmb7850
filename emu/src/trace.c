/* Trace sink dispatch and selector handling. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "emu_trace.h"
#include "emu_trace_schema.h"

typedef struct {
    emu_trace_event_callback_t callback;
    void *ctx;
} callback_impl_t;

static void callback_emit(emu_trace_sink_t *sink, const emu_trace_event_t *event) {
    callback_impl_t *impl = sink->impl;
    impl->callback(impl->ctx, event, sink->count++);
}

static int callback_close(emu_trace_sink_t *sink) {
    int failed = sink->failed;
    free(sink->impl);
    free(sink);
    return failed ? -1 : 0;
}

emu_trace_sink_t *emu_trace_open_discard(void) {
    emu_trace_sink_t *sink = calloc(1, sizeof *sink);
    if (sink) {
        sink->mask = EMU_TRACE_MASK_ALL;
        sink->discard = 1;
    }
    return sink;
}

emu_trace_sink_t *emu_trace_open_callback(emu_trace_mask_t mask,
                                  emu_trace_event_callback_t callback, void *ctx) {
    if (!callback) return NULL;
    emu_trace_sink_t *sink = calloc(1, sizeof *sink);
    callback_impl_t *impl = calloc(1, sizeof *impl);
    if (!sink || !impl) {
        free(sink);
        free(impl);
        return NULL;
    }
    impl->callback = callback;
    impl->ctx = ctx;
    sink->mask = mask;
    sink->impl = impl;
    sink->backend_emit = callback_emit;
    sink->backend_close = callback_close;
    return sink;
}

void emu_trace_emit(emu_trace_sink_t *sink, const emu_trace_event_t *event) {
    if (!sink || sink->discard || sink->failed || !sink->backend_emit) return;
    sink->backend_emit(sink, event);
}

int emu_trace_close(emu_trace_sink_t *sink) {
    if (!sink) return 0;
    if (sink->discard) { free(sink); return 0; }
    if (!sink->backend_close) { free(sink); return -1; }
    return sink->backend_close(sink);
}

static const char *SELECTOR_LIST_TEXT =
"TRACE selectors (--trace[=SELECTORS]; comma-separated groups or exact event names):\n"
"  all, exec,\n"
"  mem (= mem_read, mem_write, unmapped, unmapped_read),\n"
"  sfr (= sfr_read, sfr_write), serial (= serial_tx, serial_rx), serial_history,\n"
"  xbus (= xbus_window, xbus_access, bus_window, xbus_mailbox_complete,\n"
"          xbus_audio_packet),\n"
"  port (= port_edge), pec (= pec_transfer),\n"
"  lcd (= lcd_select, lcd_command, lcd_data, lcd_reset, lcd_frame, lcd_status,\n"
"         lcd_transaction),\n"
"  keypad (= keypad_command, keypad_result, keypad_startup_release, keypad_input,\n"
"            keypad_deferred_release, input_owner), ef_mailbox (keypad alias),\n"
"  tdma (= tdma_tick, tdma_irq),\n"
"  ssc0 (= ssc0_transfer_start, ssc0_transfer_complete),\n"
"  sim (= sim_control, sim_irq, sim_byte, sim_apdu),\n"
"  gsm (= gsm_request, gsm_response, gsm_cell_state, gsm_registration,\n"
"         gsm_signal, gsm_l1_firmware),\n"
"  patch (= firmware_patch),\n"
"  battery (= battery_state, battery_sample), capcom (= capcom_output),\n"
"  twi (= twi_event),\n"
"  eeprom (= eeprom_access, eeprom_map),\n"
"  flash (= flash_command, flash_status, flash_erase_start,\n"
"           flash_erase_suspend, flash_erase_resume, flash_erase_timer,\n"
"           flash_erase_complete, flash_program_start,\n"
"           flash_program_complete, flash_program_suspend,\n"
"           flash_program_resume, flash_lock_change,\n"
"           flash_protection_program, flash_protection_complete,\n"
"           flash_rejected_command, flash_write_absorbed), lifecycle,\n"
"  all exact event names above plus ef_mailbox_command and\n"
"  ef_mailbox_response compatibility aliases\n";

typedef struct { const char *name; emu_trace_mask_t mask; } selector_t;
#define S(n, m) {n, m}
static const selector_t SELECTORS[] = {
    S("all", EMU_TRACE_MASK_ALL), S("mem", EMU_TRACE_MASK_MEM),
    S("sfr", EMU_TRACE_MASK_SFR), S("serial", EMU_TRACE_MASK_SERIAL), S("xbus", EMU_TRACE_MASK_XBUS),
    S("port", EMU_TRACE_MASK_PORT), S("pec", EMU_TRACE_MASK_PEC), S("lcd", EMU_TRACE_MASK_LCD),
    S("keypad", EMU_TRACE_MASK_KEYPAD), S("ef_mailbox", EMU_TRACE_MASK_KEYPAD),
    S("tdma", EMU_TRACE_MASK_TDMA), S("ssc0", EMU_TRACE_MASK_SSC0), S("sim", EMU_TRACE_MASK_SIM),
    S("gsm", EMU_TRACE_MASK_GSM), S("battery", EMU_TRACE_MASK_BATTERY),
    S("patch", EMU_TRACE_MASK_FIRMWARE_PATCH),
    S("capcom", EMU_TRACE_MASK_CAPCOM), S("lifecycle", EMU_TRACE_LIFECYCLE),
    S("twi", EMU_TRACE_MASK_TWI),
    S("eeprom", EMU_TRACE_MASK_EEPROM),
    S("flash", EMU_TRACE_MASK_FLASH),
};
#undef S
static emu_trace_mask_t selector_mask(const char *name) {
    for (size_t i = 0; i < sizeof SELECTORS / sizeof SELECTORS[0]; i++)
        if (!strcmp(name, SELECTORS[i].name)) return SELECTORS[i].mask;
    return emu_trace_schema_kind_mask(name);
}
int emu_trace_parse_selectors(const char *spec, emu_trace_mask_t *out_mask,
                          char *bad, size_t bad_cap) {
    emu_trace_mask_t mask = 0;
    const char *p = spec;
    if (!p || !*p) goto bad_token;
    while (*p) {
        char tok[64]; size_t n = 0;
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        while (*p && *p != ',') { if (n + 1 < sizeof tok) tok[n++] = *p; p++; }
        while (n && isspace((unsigned char)tok[n - 1])) n--;
        tok[n] = 0;
        emu_trace_mask_t bit = selector_mask(tok);
        if (!n || !bit) {
            if (bad && bad_cap) snprintf(bad, bad_cap, "%s", tok);
            return -1;
        }
        mask |= bit;
    }
    if (out_mask) *out_mask = mask;
    if (bad && bad_cap) bad[0] = 0;
    return 0;
bad_token:
    if (bad && bad_cap) snprintf(bad, bad_cap, "%s", spec ? spec : "");
    return -1;
}
const char *emu_trace_selector_list_text(void) { return SELECTOR_LIST_TEXT; }
int emu_trace_mask_accepts(emu_trace_mask_t mask, const char *kind) {
    emu_trace_mask_t bit = emu_trace_schema_kind_mask(kind);
    return bit && (mask & bit);
}
int emu_trace_sink_accepts(const emu_trace_sink_t *sink, const char *kind) {
    if (!sink) return 0;
    return sink->discard || emu_trace_mask_accepts(sink->mask, kind);
}
