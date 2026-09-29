/* Engine-neutral normalized trace sink.
 * The sink is opt-in and NULL by default: when no sink is attached the SoC/CPU
 * skip event construction entirely, so a plain benchmark run pays nothing. */
#ifndef EMU_TRACE_H
#define EMU_TRACE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* One typed key in an event's optional info map. */
typedef enum { EMU_TRACE_VALUE_I64, EMU_TRACE_VALUE_BOOL, EMU_TRACE_VALUE_NULL, EMU_TRACE_VALUE_STRING } emu_trace_value_kind_t;
typedef struct {
    const char *key;
    emu_trace_value_kind_t kind;
    long ival;
    const char *sval;
} emu_trace_field_t;
typedef struct { int n; emu_trace_field_t kv[32]; } emu_trace_info_t;

typedef struct emu_trace_event {
    const char *kind;
    uint64_t icount;
    uint32_t pc;
    int      has_addr;  uint32_t addr;
    int      has_size;  int size;
    int      has_value; uint32_t value;
    const char *detail;   /* "" if none */
    emu_trace_info_t info;
} emu_trace_event_t;

typedef uint64_t emu_trace_mask_t;
enum {
    EMU_TRACE_EXEC         = UINT64_C(1) << 0,
    EMU_TRACE_MEM_READ     = UINT64_C(1) << 1,
    EMU_TRACE_MEM_WRITE    = UINT64_C(1) << 2,
    EMU_TRACE_UNMAPPED     = UINT64_C(1) << 3,
    EMU_TRACE_SFR_READ     = UINT64_C(1) << 4,
    EMU_TRACE_SFR_WRITE    = UINT64_C(1) << 5,
    EMU_TRACE_SERIAL_TX    = UINT64_C(1) << 6,
    EMU_TRACE_SERIAL_RX    = UINT64_C(1) << 7,
    EMU_TRACE_XBUS_WINDOW  = UINT64_C(1) << 8,
    EMU_TRACE_XBUS_ACCESS  = UINT64_C(1) << 9,
    EMU_TRACE_PORT_EDGE    = UINT64_C(1) << 10,
    EMU_TRACE_BUS_WINDOW   = UINT64_C(1) << 11,
    EMU_TRACE_PEC_TRANSFER = UINT64_C(1) << 12,
    EMU_TRACE_LCD_SELECT   = UINT64_C(1) << 13,
    EMU_TRACE_LCD_COMMAND  = UINT64_C(1) << 14,
    EMU_TRACE_LCD_DATA     = UINT64_C(1) << 15,
    EMU_TRACE_LCD_RESET    = UINT64_C(1) << 16,
    EMU_TRACE_LCD_FRAME    = UINT64_C(1) << 17,
    EMU_TRACE_KEYPAD_COMMAND = UINT64_C(1) << 18,
    EMU_TRACE_KEYPAD_RESULT  = UINT64_C(1) << 19,
    EMU_TRACE_TDMA_TICK    = UINT64_C(1) << 20,
    EMU_TRACE_TDMA_IRQ     = UINT64_C(1) << 21,
    EMU_TRACE_SSC0_TRANSFER_START    = UINT64_C(1) << 22,
    EMU_TRACE_SSC0_TRANSFER_COMPLETE = UINT64_C(1) << 23,
    EMU_TRACE_LCD_STATUS     = UINT64_C(1) << 24,
    EMU_TRACE_SIM_CONTROL    = UINT64_C(1) << 25,
    EMU_TRACE_SIM_IRQ        = UINT64_C(1) << 26,
    EMU_TRACE_SIM_BYTE       = UINT64_C(1) << 27,
    EMU_TRACE_SIM_APDU       = UINT64_C(1) << 28,
    EMU_TRACE_KEYPAD_INPUT   = UINT64_C(1) << 29,
    EMU_TRACE_LIFECYCLE      = UINT64_C(1) << 30,
    EMU_TRACE_XBUS_MAILBOX_COMPLETE = UINT64_C(1) << 31,
    EMU_TRACE_FLASH          = UINT64_C(1) << 32,
    EMU_TRACE_KEYPAD_DEFERRED_RELEASE = UINT64_C(1) << 33,
    EMU_TRACE_GSM_REQUEST    = UINT64_C(1) << 34,
    EMU_TRACE_GSM_RESPONSE   = UINT64_C(1) << 35,
    EMU_TRACE_GSM_REGISTRATION = UINT64_C(1) << 36,
    EMU_TRACE_GSM_SIGNAL     = UINT64_C(1) << 37,
    EMU_TRACE_BATTERY_STATE  = UINT64_C(1) << 38,
    EMU_TRACE_BATTERY_SAMPLE = UINT64_C(1) << 39,
    EMU_TRACE_CAPCOM_OUTPUT   = UINT64_C(1) << 40,
    EMU_TRACE_TWI_EVENT       = UINT64_C(1) << 41,
    EMU_TRACE_FIRMWARE_PATCH = UINT64_C(1) << 42,
    EMU_TRACE_GSM_CELL_STATE = UINT64_C(1) << 43,
    EMU_TRACE_GSM_L1_FIRMWARE = UINT64_C(1) << 44,
    EMU_TRACE_LCD_TRANSACTION = UINT64_C(1) << 45,
    EMU_TRACE_EEPROM_ACCESS = UINT64_C(1) << 46,
    EMU_TRACE_EEPROM_MAP    = UINT64_C(1) << 47,
    EMU_TRACE_XBUS_AUDIO_PACKET = UINT64_C(1) << 48,
    EMU_TRACE_SERIAL_HISTORY = UINT64_C(1) << 50,
    EMU_TRACE_INPUT_OWNER = UINT64_C(1) << 49,
    EMU_TRACE_EEPROM        = EMU_TRACE_EEPROM_ACCESS | EMU_TRACE_EEPROM_MAP,
    EMU_TRACE_MASK_MEM     = EMU_TRACE_MEM_READ | EMU_TRACE_MEM_WRITE | EMU_TRACE_UNMAPPED,
    EMU_TRACE_MASK_SFR     = EMU_TRACE_SFR_READ | EMU_TRACE_SFR_WRITE,
    EMU_TRACE_MASK_SERIAL  = EMU_TRACE_SERIAL_TX | EMU_TRACE_SERIAL_RX,
    EMU_TRACE_MASK_XBUS    = EMU_TRACE_XBUS_WINDOW | EMU_TRACE_XBUS_ACCESS | EMU_TRACE_BUS_WINDOW |
                         EMU_TRACE_XBUS_MAILBOX_COMPLETE |
                         EMU_TRACE_XBUS_AUDIO_PACKET,
    EMU_TRACE_MASK_PORT    = EMU_TRACE_PORT_EDGE,
    EMU_TRACE_MASK_PEC     = EMU_TRACE_PEC_TRANSFER,
    EMU_TRACE_MASK_LCD     = EMU_TRACE_LCD_SELECT | EMU_TRACE_LCD_COMMAND | EMU_TRACE_LCD_DATA |
                         EMU_TRACE_LCD_RESET | EMU_TRACE_LCD_FRAME | EMU_TRACE_LCD_STATUS |
                         EMU_TRACE_LCD_TRANSACTION,
    EMU_TRACE_MASK_KEYPAD = EMU_TRACE_KEYPAD_COMMAND | EMU_TRACE_KEYPAD_RESULT |
                        EMU_TRACE_KEYPAD_INPUT | EMU_TRACE_KEYPAD_DEFERRED_RELEASE | EMU_TRACE_INPUT_OWNER,
    EMU_TRACE_MASK_EF_MAILBOX = EMU_TRACE_MASK_KEYPAD,
    EMU_TRACE_MASK_TDMA    = EMU_TRACE_TDMA_TICK | EMU_TRACE_TDMA_IRQ,
    EMU_TRACE_MASK_SSC0    = EMU_TRACE_SSC0_TRANSFER_START |
                         EMU_TRACE_SSC0_TRANSFER_COMPLETE,
    EMU_TRACE_MASK_SIM     = EMU_TRACE_SIM_CONTROL | EMU_TRACE_SIM_IRQ |
                         EMU_TRACE_SIM_BYTE | EMU_TRACE_SIM_APDU,
    EMU_TRACE_MASK_GSM     = EMU_TRACE_GSM_REQUEST | EMU_TRACE_GSM_RESPONSE |
                         EMU_TRACE_GSM_REGISTRATION | EMU_TRACE_GSM_SIGNAL |
                         EMU_TRACE_GSM_CELL_STATE | EMU_TRACE_GSM_L1_FIRMWARE,
    EMU_TRACE_MASK_BATTERY = EMU_TRACE_BATTERY_STATE | EMU_TRACE_BATTERY_SAMPLE,
    EMU_TRACE_MASK_CAPCOM  = EMU_TRACE_CAPCOM_OUTPUT,
    EMU_TRACE_MASK_TWI     = EMU_TRACE_TWI_EVENT,
    EMU_TRACE_MASK_LIFECYCLE = EMU_TRACE_LIFECYCLE,
    EMU_TRACE_MASK_FLASH   = EMU_TRACE_FLASH,
    EMU_TRACE_MASK_EEPROM = EMU_TRACE_EEPROM,
    EMU_TRACE_MASK_FIRMWARE_PATCH = EMU_TRACE_FIRMWARE_PATCH,
    EMU_TRACE_MASK_ALL     = EMU_TRACE_EXEC | EMU_TRACE_MASK_MEM | EMU_TRACE_MASK_SFR |
                         EMU_TRACE_MASK_SERIAL | EMU_TRACE_MASK_XBUS | EMU_TRACE_MASK_PORT |
                         EMU_TRACE_MASK_PEC | EMU_TRACE_MASK_LCD | EMU_TRACE_MASK_EF_MAILBOX |
                         EMU_TRACE_MASK_TDMA | EMU_TRACE_MASK_SSC0 | EMU_TRACE_MASK_SIM |
                         EMU_TRACE_MASK_GSM | EMU_TRACE_MASK_BATTERY |
                         EMU_TRACE_MASK_CAPCOM | EMU_TRACE_MASK_TWI |
                         EMU_TRACE_MASK_LIFECYCLE |
                         EMU_TRACE_MASK_FLASH | EMU_TRACE_MASK_EEPROM |
                         EMU_TRACE_MASK_FIRMWARE_PATCH | EMU_TRACE_SERIAL_HISTORY,
};

#define EMU_TRACE_BATCH_EVENTS 65536u

struct emu_trace_sink;
typedef void (*emu_trace_backend_emit_fn)(struct emu_trace_sink *, const emu_trace_event_t *);
typedef int (*emu_trace_backend_close_fn)(struct emu_trace_sink *);
typedef void (*emu_trace_event_callback_t)(void *, const emu_trace_event_t *, uint64_t);

/* Public fields are intentionally limited to hot-path selector state and
 * counters. Backend state is private to the sink implementation. */
typedef struct emu_trace_sink {
    uint64_t count;
    emu_trace_mask_t mask;
    int discard;
    int failed;
    void *impl;
    emu_trace_backend_emit_fn backend_emit;
    emu_trace_backend_close_fn backend_close;
} emu_trace_sink_t;

/* Open a Hive-partitioned Parquet dataset sink. */
emu_trace_sink_t *emu_trace_open(const char *path, emu_trace_mask_t mask);

/* A no-file "discard" sink: attaching it makes soc->trace non-NULL so every
 * access flows through emit() (and thus fires soc->watch_hook), but nothing is
 * formatted or written. This is how the debugger taps accesses live without
 * recording a trace — the C mirror of Python setting emu_trace_enabled=True while
 * the watch_hook does the real work. Never NULL (allocation is trivial). */
emu_trace_sink_t *emu_trace_open_discard(void);
/* Synchronous callback sink used by unit tests and embedders. */
emu_trace_sink_t *emu_trace_open_callback(emu_trace_mask_t mask, emu_trace_event_callback_t callback,
                                  void *ctx);
/* Finalize the backend; always frees the sink. */
int emu_trace_close(emu_trace_sink_t *sink);

/* Append one typed event. Errors are reported by emu_trace_close(). */
void emu_trace_emit(emu_trace_sink_t *sink, const emu_trace_event_t *ev);

/* Parse a comma-separated trace selector list into a mask. Accepts grouped
 * aliases (`mem`, `sfr`, `serial`, `xbus`) plus exact event selectors. Returns
 * 0 on success, -1 on an unknown/empty token (copied to `bad`). */
int emu_trace_parse_selectors(const char *spec, emu_trace_mask_t *out_mask,
                          char *bad, size_t bad_cap);

/* Standalone listing text enumerating every accepted selector token. */
const char *emu_trace_selector_list_text(void);

/* True if `mask` or `sink->mask` includes the named event kind. Unknown kinds
 * are treated as disabled. The discard sink is treated as accepting all kinds
 * so debugger watchpoints keep their current behavior. */
int emu_trace_mask_accepts(emu_trace_mask_t mask, const char *kind);
int emu_trace_sink_accepts(const emu_trace_sink_t *sink, const char *kind);

/* Helpers to build an info dict in sorted key order. */
static inline void emu_trace_info_int(emu_trace_info_t *in, const char *key, long v) {
    in->kv[in->n].key = key; in->kv[in->n].kind = EMU_TRACE_VALUE_I64; in->kv[in->n].ival = v; in->n++;
}
static inline void emu_trace_info_bool(emu_trace_info_t *in, const char *key, int v) {
    in->kv[in->n].key = key; in->kv[in->n].kind = EMU_TRACE_VALUE_BOOL; in->kv[in->n].ival = v ? 1 : 0; in->n++;
}
static inline void emu_trace_info_null(emu_trace_info_t *in, const char *key) {
    in->kv[in->n].key = key; in->kv[in->n].kind = EMU_TRACE_VALUE_NULL; in->kv[in->n].ival = 0; in->n++;
}
static inline void emu_trace_info_str(emu_trace_info_t *in, const char *key, const char *v) {
    in->kv[in->n].key = key; in->kv[in->n].kind = EMU_TRACE_VALUE_STRING; in->kv[in->n].sval = v; in->n++;
}

#endif /* EMU_TRACE_H */
