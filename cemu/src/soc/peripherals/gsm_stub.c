/* Experimental PMB7850 baseband command/result transport.
 *
 * C55 v24 exposes the current DSP command at shared word E17C after preparing
 * each expected response class. IRQ45 records an unsuccessful result; XP2
 * records a synchronized result whose carrier tag is supplied through the
 * shared DSP response words below. The proprietary OAK/L1 message transport
 * remains unknown.  The temporary C55 decoded-network call experiment lives
 * in gsm_legacy_adapter.c and is deliberately outside this transport.
 */
#include <string.h>
#include "gsm_stub.h"
#include "soc.h"

#define GSM_COMMAND_ACCUMULATE 0x00ABu /* expected descriptor type 0x0F */
#define GSM_COMMAND_MEASURE    0x00A9u /* expected descriptor type 0x03 */
#define GSM_RESPONSE_TICKS     120000u /* one configured C55 TDMA frame */

static const addr_range_t GSM_RANGES[] = {
    {GSM_DSP_COMMAND_MIRROR, GSM_DSP_COMMAND_MIRROR + 1u},
    {GSM_DSP_COMMAND, GSM_DSP_COMMAND + 1u},
};
static const ic_node_t GSM_IC[] = {
    {GSM_IRQ45_IC, 0x4D, 0},
    {GSM_XP2_IC, 0x42, 0},
};
static const reg_name_t GSM_NAMES[] = {
    {GSM_DSP_COMMAND_MIRROR, "GSM_DSP_COMMAND_MIRROR"},
    {GSM_DSP_COMMAND, "GSM_DSP_COMMAND"},
    {GSM_IRQ45_IC, "IRQ45IC"},
    {GSM_XP2_IC, "XP2IC"},
};

static uint16_t ram_word(soc_t *s, uint32_t addr) {
    return (uint16_t)(memory_controller_ram_get(&s->memory, addr) |
                      ((uint16_t)memory_controller_ram_get(&s->memory, addr + 1u) << 8));
}

static void put_word(soc_t *s, uint32_t addr, uint16_t value) {
    /* DSP writes must follow the CPU-visible mapping: C55 places the shared
     * response control block in remapped local memory after EINIT. */
    cemu_memory_controller_poke16(&s->memory, addr, value);
}

uint64_t cemu_baseband_result_publish(peripheral_t *p, soc_t *s,
                                 baseband_result_source_t source,
                                 uint16_t command,
                                 uint16_t word0, uint16_t word1) {
    gsm_stub_state_t *st = p ? p->state : NULL;
    if (!st || !s || source == BASEBAND_RESULT_NONE) return 0;
    put_word(s, BASEBAND_RESULT_WORD0, word0);
    put_word(s, BASEBAND_RESULT_WORD1, word1);
    st->result_sequence++;
    st->last_result_source = source;
    st->last_result_command = command;
    st->last_result_word0 = word0;
    st->last_result_word1 = word1;
    return st->result_sequence;
}

static void emit_request(soc_t *s, gsm_stub_state_t *st, int accepted) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "gsm_request")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "gsm_request";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = GSM_DSP_COMMAND;
    ev.has_size = 1; ev.size = 2;
    ev.has_value = 1; ev.value = st->command;
    ev.detail = "PMB7850 DSP command";
    cemu_event_field_bool(&ev.info, "accepted", accepted);
    cemu_event_field_i64(&ev.info, "completion_tick", (long)st->due_tick);
    cemu_event_field_i64(&ev.info, "sequence", (long)st->request_sequence);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static void emit_response(soc_t *s, gsm_stub_state_t *st, uint32_t irq,
                          int synchronized, uint16_t word0, uint16_t word1) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "gsm_response")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "gsm_response";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = irq;
    ev.has_size = 1; ev.size = 2;
    ev.has_value = 1; ev.value = memory_controller_sfr_get(&s->memory, irq);
    ev.detail = synchronized ? "synchronized DSP response" : "unsynchronized DSP response";
    cemu_event_field_i64(&ev.info, "arfcn",
                   synchronized ? (long)st->cell.arfcn : -1L);
    cemu_event_field_i64(&ev.info, "command", st->pending_command);
    cemu_event_field_i64(&ev.info, "sequence", (long)st->pending_sequence);
    cemu_event_field_i64(&ev.info, "result_sequence", (long)st->result_sequence);
    cemu_event_field_bool(&ev.info, "synchronized", synchronized);
    cemu_event_field_i64(&ev.info, "word0", word0);
    cemu_event_field_i64(&ev.info, "word1", word1);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static void emit_cell_state(soc_t *s, gsm_stub_state_t *st, uint32_t irq) {
    if (!cemu_event_native_trace_active(&s->instrumentation,
                                        "gsm_cell_state")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "gsm_cell_state";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = st->response_carrier_addr;
    ev.has_size = 1; ev.size = 4;
    ev.has_value = 1; ev.value = st->cell.arfcn;
    ev.detail = "virtual cell synchronized";
    cemu_event_field_bool(&ev.info, "available", st->cell.available);
    cemu_event_field_i64(&ev.info, "command", st->pending_command);
    cemu_event_field_i64(&ev.info, "interrupt", irq);
    cemu_event_field_i64(&ev.info, "level", st->cell.level);
    cemu_event_field_i64(&ev.info, "measurement_index",
                   st->cell.measurement_index);
    cemu_event_field_i64(&ev.info, "plmn_bcd",
                   st->cell.plmn[0] | ((long)st->cell.plmn[1] << 8) |
                   ((long)st->cell.plmn[2] << 16));
    cemu_event_field_i64(&ev.info, "quality", st->cell.quality);
    cemu_event_field_i64(&ev.info, "sequence", (long)st->pending_sequence);
    cemu_event_field_bool(&ev.info, "service_allowed", st->cell.service_allowed);
    cemu_event_field_bool(&ev.info, "synchronized", st->cell.synchronized);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static int gsm_read8(peripheral_t *self, soc_t *s, uint32_t addr) {
    (void)self;
    return memory_controller_ram_present(&s->memory, addr) ? memory_controller_ram_get(&s->memory, addr) : 0;
}

static int gsm_peek8(peripheral_t *self, soc_t *s, uint32_t addr) {
    return gsm_read8(self, s, addr);
}

static int gsm_write8(peripheral_t *self, soc_t *s,
                      uint32_t addr, uint8_t value) {
    (void)value;
    gsm_stub_state_t *st = (gsm_stub_state_t *)self->state;
    if (!st->enabled || addr != GSM_DSP_COMMAND) return 0;
    st->command = ram_word(s, GSM_DSP_COMMAND);
    st->request_sequence++;
    int recognized = st->command == GSM_COMMAND_ACCUMULATE ||
                     st->command == GSM_COMMAND_MEASURE;
    int accepted = recognized && !st->pending;
    if (accepted) {
        st->pending = 1;
        st->pending_command = st->command;
        st->pending_sequence = st->request_sequence;
        st->due_tick = s->ticks + GSM_RESPONSE_TICKS;
    }
    /* E17C is shared memory, not a write-only doorbell.  The firmware's bulk
     * copier can rewrite it just before completion.  Once work is pending,
     * later copies are observable but cannot postpone or replace that work. */
    emit_request(s, st, accepted);
    return 1;
}

static void raise_response(soc_t *s, gsm_stub_state_t *st) {
    int was_synchronized = st->cell.synchronized;
    virtual_cell_result_t result =
        cemu_virtual_cell_complete_command(&st->cell, st->pending_command);
    uint32_t irq = GSM_IRQ45_IC;
    cemu_baseband_result_publish(s->gsm_stub_periph, s, BASEBAND_RESULT_GSM,
                            st->pending_command, result.word0,
                            result.word1);
    if (result.synchronized) {
        /* The DSP shares this response block with the C166 low-level driver.
         * Bit 0 selects the low carrier byte for the pending XP2 response. */
        put_word(s, st->response_carrier_addr, result.arfcn);
        put_word(s, st->response_flags_addr, 1);
        irq = GSM_XP2_IC;
        st->synchronized_responses++;
    }
    uint16_t value = (uint16_t)(memory_controller_sfr_get(&s->memory, irq) | XIC_IR_BIT);
    cemu_memory_controller_sfr_put(&s->memory, irq, value);
    st->pending = 0;
    emit_response(s, st, irq, result.synchronized,
                  result.word0, result.word1);
    if (!was_synchronized && st->cell.synchronized)
        emit_cell_state(s, st, irq);
}

static void gsm_tick(peripheral_t *self, soc_t *s, int n) {
    (void)n;
    gsm_stub_state_t *st = (gsm_stub_state_t *)self->state;
    if (st->enabled && st->pending && s->ticks >= st->due_tick)
        raise_response(s, st);
}

static uint64_t gsm_next_event(peripheral_t *self, soc_t *s) {
    gsm_stub_state_t *st = (gsm_stub_state_t *)self->state;
    if (!st->enabled || !st->pending) return UINT64_MAX;
    return st->due_tick <= s->ticks ? 1 : st->due_tick - s->ticks;
}

static void gsm_advance_quiet(peripheral_t *self, soc_t *s, uint64_t ticks) {
    (void)self; (void)s; (void)ticks;
}

static int gsm_ic_will_fire(peripheral_t *self, soc_t *s, uint32_t ic_addr) {
    (void)s;
    gsm_stub_state_t *st = (gsm_stub_state_t *)self->state;
    return st->enabled && st->pending &&
           (ic_addr == GSM_IRQ45_IC || ic_addr == GSM_XP2_IC);
}

void cemu_gsm_stub_periph_init(peripheral_t *p, gsm_stub_state_t *st,
                          const device_baseband_profile_t *profile) {
    memset(p, 0, sizeof *p);
    memset(st, 0, sizeof *st);
    if (profile) {
        st->response_carrier_addr = profile->response_carrier_addr;
        st->response_flags_addr = profile->response_flags_addr;
    }
    cemu_virtual_cell_init(&st->cell);
    p->id = "gsm-stub";
    p->state = st;
    p->byte_ranges = GSM_RANGES;
    p->n_byte_ranges = (int)(sizeof GSM_RANGES / sizeof GSM_RANGES[0]);
    p->ic_nodes = GSM_IC;
    p->n_ic_nodes = (int)(sizeof GSM_IC / sizeof GSM_IC[0]);
    PERIPHERAL_REG_NAMES(p, GSM_NAMES,
                         (int)(sizeof GSM_NAMES / sizeof GSM_NAMES[0]));
    p->read8 = gsm_read8;
    p->peek8 = gsm_peek8;
    p->write8 = gsm_write8;
    p->tick = gsm_tick;
    p->next_event_ticks = gsm_next_event;
    p->advance_quiet = gsm_advance_quiet;
    p->ic_will_fire = gsm_ic_will_fire;
}

int cemu_gsm_stub_available(const peripheral_t *p) {
    const gsm_stub_state_t *st = p ? p->state : NULL;
    return st && st->response_carrier_addr && st->response_flags_addr;
}

void cemu_gsm_stub_set_enabled(peripheral_t *p, soc_t *s, int enabled) {
    gsm_stub_state_t *st = (gsm_stub_state_t *)p->state;
    if (st->restored) {
        st->restored = 0;
        st->enabled = enabled != 0 && st->response_carrier_addr != 0 &&
                      st->response_flags_addr != 0;
        return;
    }
    uint32_t response_carrier_addr = st->response_carrier_addr;
    uint32_t response_flags_addr = st->response_flags_addr;
    memset(st, 0, sizeof *st);
    st->response_carrier_addr = response_carrier_addr;
    st->response_flags_addr = response_flags_addr;
    cemu_virtual_cell_init(&st->cell);
    st->enabled = enabled != 0 && response_carrier_addr != 0 &&
                  response_flags_addr != 0;
    if (!st->enabled || !memory_controller_ram_present(&s->memory, GSM_DSP_COMMAND)) return;
    st->command = ram_word(s, GSM_DSP_COMMAND);
    if (st->command == GSM_COMMAND_ACCUMULATE ||
        st->command == GSM_COMMAND_MEASURE) {
        st->request_sequence = 1;
        st->pending = 1;
        st->pending_command = st->command;
        st->pending_sequence = st->request_sequence;
        st->due_tick = s->ticks + GSM_RESPONSE_TICKS;
    }
}
