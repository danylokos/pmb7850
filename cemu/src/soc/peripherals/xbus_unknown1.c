/* Identity-unknown XBUS device — see xbus_unknown1.h. */
#include <stdlib.h>
#include <string.h>
#include "xbus_unknown1.h"
#include "battery.h"
#include "soc.h"
#include "synth.h"

#define UNKNOWN1_ID       0xE800u
#define UNKNOWN1_STATUS   0xE806u
#define UNKNOWN1_LAUNCH   0xEF3Au
#define UNKNOWN1_MBOX     0xEC12u
#define UNKNOWN1_CONTROL  0xEC10u
#define UNKNOWN1_RESULT   0xEC16u
#define UNKNOWN1_COMMAND  0xEC14u
#define UNKNOWN1_REQUEST  0xEC80u
#define UNKNOWN1_INDEX    0xEC82u
#define UNKNOWN1_TRAILER  0xEC84u
#define UNKNOWN1_DONE     0x8000u
#define UNKNOWN1_PENDING  0x1u
#define UNKNOWN1_SYNC_COMPLETE 0x2u
#define UNKNOWN1_COMPLETE 0x4u
#define UNKNOWN1_READY    0x10u
#define UNKNOWN1_IRQ      0xF140u
#define UNKNOWN1_IRQ_DELAY_TICKS 32u
/* Observed 1700 synchronous, 1701 queued, 1704 legacy IRQ modes.
 * Firmware compatibility calibration, not identified physical bit names. */
#define UNKNOWN1_INFERRED_IRQ_MODE_MASK 0x0005u

static const addr_range_t UNKNOWN1_RANGES[] = {
    {0xE800, 0xE807},
    {0xEC12, 0xEC17},
    {0xEF3A, 0xEF3B},
};

static const reg_name_t UNKNOWN1_REG_NAMES[] = {
    { UNKNOWN1_ID, "XBUS_UNKNOWN1_ID" },
    { UNKNOWN1_STATUS, "XBUS_UNKNOWN1_STATUS" },
    { UNKNOWN1_LAUNCH, "XBUS_UNKNOWN1_LAUNCH" },
    { UNKNOWN1_MBOX, "XBUS_UNKNOWN1_MBOX" },
    { UNKNOWN1_RESULT, "XBUS_UNKNOWN1_MBOX_RESULT" },
    { UNKNOWN1_IRQ, "IRQ80IC" },
};

static const ic_node_t UNKNOWN1_IC_NODES[] = {
    { UNKNOWN1_IRQ, 0x50, 0 },
};

static uint16_t ram_word(soc_t *s, uint32_t addr) {
    return (uint16_t)(memory_controller_ram_get(&s->memory, addr) |
                      ((uint16_t)memory_controller_ram_get(&s->memory, addr + 1u) << 8));
}

static void set_ram_word(soc_t *s, uint32_t addr, uint16_t value) {
    memory_controller_ram_set(&s->memory, addr, (uint8_t)value);
    memory_controller_ram_set(&s->memory, addr + 1u, (uint8_t)(value >> 8));
}

/* EB80/E836 audio behavior is delegated to audio/xbus_audio.c. */

static int mailbox_register_query(uint16_t command, uint16_t request,
                                  uint16_t trailer) {
    return command == 2u && request == 0x0162u && trailer == 0x0163u;
}

static int mailbox_charger_query(soc_t *s, uint16_t command,
                                 uint16_t request, uint16_t index,
                                 uint16_t trailer) {
    return cemu_battery_available(s->battery_periph) &&
           mailbox_register_query(command, request, trailer) && index == 2u;
}

static uint8_t mailbox_result(soc_t *s, uint16_t command,
                              uint16_t request, uint16_t index,
                              uint16_t trailer, int charger_query) {
    if (mailbox_register_query(command, request, trailer) && index == 0u)
        return 1u;
    return charger_query
         ? (cemu_battery_charging(s->battery_periph) ? UNKNOWN1_READY : 0u)
         : UNKNOWN1_READY;
}

static void emit_mailbox_complete(soc_t *s, xbus_unknown1_state_t *st,
                                  const char *phase, uint64_t deadline,
                                  uint8_t before, uint8_t status, uint8_t result,
                                  uint16_t irq_before, uint16_t irq_after,
                                  uint16_t command, uint16_t request,
                                  uint16_t index, uint16_t trailer,
                                  int charger_query, int irq_asserted) {
    if (!cemu_event_native_trace_active(&s->instrumentation,
                                       "xbus_mailbox_complete")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "xbus_mailbox_complete";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = UNKNOWN1_MBOX;
    ev.has_size = 1; ev.size = 1;
    ev.has_value = 1; ev.value = status;
    ev.detail = "xbus-unknown-1";
    cemu_event_field_bool(&ev.info, "already_pending",
                    (irq_before & XIC_IR_BIT) != 0);
    cemu_event_field_i64(&ev.info, "battery_level",
                   cemu_battery_available(s->battery_periph)
                       ? (long)cemu_battery_level(s->battery_periph) : -1);
    cemu_event_field_bool(&ev.info, "charger_query", charger_query);
    cemu_event_field_bool(&ev.info, "charging",
                    cemu_battery_charging(s->battery_periph));
    cemu_event_field_i64(&ev.info, "command", command);
    cemu_event_field_i64(&ev.info, "control", st->control);
    cemu_event_field_bool(&ev.info, "async",
                         (st->control & UNKNOWN1_INFERRED_IRQ_MODE_MASK) != 0);
    cemu_event_field_bool(&ev.info, "complete_bit1",
                    (status & UNKNOWN1_SYNC_COMPLETE) != 0);
    cemu_event_field_bool(&ev.info, "complete_bit2",
                    (status & UNKNOWN1_COMPLETE) != 0);
    cemu_event_field_i64(&ev.info, "deadline", (long)deadline);
    cemu_event_field_i64(&ev.info, "index", index);
    cemu_event_field_i64(&ev.info, "irq", UNKNOWN1_IRQ);
    cemu_event_field_bool(&ev.info, "irq_asserted", irq_asserted);
    cemu_event_field_i64(&ev.info, "irq_value", irq_after);
    cemu_event_field_string(&ev.info, "phase", phase);
    cemu_event_field_i64(&ev.info, "request", request);
    cemu_event_field_i64(&ev.info, "result", result);
    cemu_event_field_i64(&ev.info, "status_before", before);
    cemu_event_field_i64(&ev.info, "trailer", trailer);
    cemu_event_field_i64(&ev.info, "transaction_id",
                   (long)st->active_transaction_id);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static void mailbox_emit_phase(soc_t *s, xbus_unknown1_state_t *st,
                               const char *phase, uint64_t deadline,
                               uint8_t before, uint8_t status,
                               uint16_t irq_before, uint16_t irq_after,
                               int irq_asserted) {
    uint16_t command = ram_word(s, UNKNOWN1_COMMAND);
    uint16_t request = ram_word(s, UNKNOWN1_REQUEST);
    uint16_t index = ram_word(s, UNKNOWN1_INDEX);
    uint16_t trailer = ram_word(s, UNKNOWN1_TRAILER);
    int charger_query = mailbox_charger_query(
        s, command, request, index, trailer);
    emit_mailbox_complete(
        s, st, phase, deadline, before, status,
        memory_controller_ram_get(&s->memory, UNKNOWN1_RESULT), irq_before, irq_after,
        command, request, index, trailer, charger_query, irq_asserted);
}

static int unknown1_completed(soc_t *s) {
    return memory_controller_ram_present(&s->memory, UNKNOWN1_LAUNCH) &&
           (memory_controller_ram_get(&s->memory, UNKNOWN1_LAUNCH) & 1u);
}

static int unknown1_read8(peripheral_t *self, soc_t *s, uint32_t addr) {
    xbus_unknown1_state_t *st = (xbus_unknown1_state_t *)self->state;
    if (addr == UNKNOWN1_ID) {
        st->id_reads++;
        return st->id & 0xFF;
    }
    if (addr == UNKNOWN1_ID + 1)
        return (st->id >> 8) & 0xFF;
    if (addr == UNKNOWN1_STATUS) {
        st->status_reads++;
        return memory_controller_ram_present(&s->memory, addr) ? memory_controller_ram_get(&s->memory, addr) : 0;
    }
    if (addr == UNKNOWN1_STATUS + 1) {
        uint8_t stored = memory_controller_ram_present(&s->memory, addr) ? memory_controller_ram_get(&s->memory, addr) : 0;
        return stored | (unknown1_completed(s) ? (UNKNOWN1_DONE >> 8) : 0);
    }
    return -1;
}

static int unknown1_peek8(peripheral_t *self, soc_t *s, uint32_t addr) {
    xbus_unknown1_state_t *st = (xbus_unknown1_state_t *)self->state;
    if (addr == UNKNOWN1_ID) return st->id & 0xFF;
    if (addr == UNKNOWN1_ID + 1) return (st->id >> 8) & 0xFF;
    if (addr == UNKNOWN1_STATUS)
        return memory_controller_ram_present(&s->memory, addr) ? memory_controller_ram_get(&s->memory, addr) : 0;
    if (addr == UNKNOWN1_STATUS + 1) {
        uint8_t stored = memory_controller_ram_present(&s->memory, addr) ? memory_controller_ram_get(&s->memory, addr) : 0;
        return stored | (unknown1_completed(s) ? (UNKNOWN1_DONE >> 8) : 0);
    }
    return -1;
}

static int mailbox_diagnostic_immediate(soc_t *s) {
    unsigned controls = (1u << SYN_XBUS_MAILBOX_COMPLETE_BIT1) |
                        (1u << SYN_XBUS_MAILBOX_NO_COMPLETE_BIT2) |
                        (1u << SYN_XBUS_MAILBOX_NO_IRQ80) |
                        (1u << SYN_XBUS_MAILBOX_IMMEDIATE);
    return (s->synth_mask & controls) != 0;
}

static void mailbox_accept_immediate(soc_t *s, xbus_unknown1_state_t *st,
                                     uint8_t before) {
    int complete_bit1 =
        (s->synth_mask >> SYN_XBUS_MAILBOX_COMPLETE_BIT1) & 1u;
    int complete_bit2 =
        !((s->synth_mask >> SYN_XBUS_MAILBOX_NO_COMPLETE_BIT2) & 1u);
    int assert_irq =
        !((s->synth_mask >> SYN_XBUS_MAILBOX_NO_IRQ80) & 1u);
    uint8_t done = before & (uint8_t)~UNKNOWN1_PENDING;
    if (complete_bit1) done |= UNKNOWN1_SYNC_COMPLETE;
    if (complete_bit2) done |= UNKNOWN1_COMPLETE;
    memory_controller_ram_set(&s->memory, UNKNOWN1_MBOX, done);

    uint16_t command = ram_word(s, UNKNOWN1_COMMAND);
    uint16_t request = ram_word(s, UNKNOWN1_REQUEST);
    uint16_t index = ram_word(s, UNKNOWN1_INDEX);
    uint16_t trailer = ram_word(s, UNKNOWN1_TRAILER);
    int charger_query = mailbox_charger_query(
        s, command, request, index, trailer);
    uint8_t result = mailbox_result(
        s, command, request, index, trailer, charger_query);
    memory_controller_ram_set(&s->memory, UNKNOWN1_RESULT, result);

    uint16_t irq_before = memory_controller_sfr_get(&s->memory, UNKNOWN1_IRQ);
    uint16_t irq_after = assert_irq
                       ? (irq_before | XIC_IR_BIT) : irq_before;
    if (assert_irq) cemu_memory_controller_sfr_put(&s->memory, UNKNOWN1_IRQ, irq_after);
    st->phase = XBUS_MAILBOX_IDLE;
    st->deadline = 0;
    emit_mailbox_complete(
        s, st, "immediate", 0, before, done, result,
        irq_before, irq_after, command, request, index, trailer,
        charger_query, assert_irq);
}

static void mailbox_accept_staged(soc_t *s, xbus_unknown1_state_t *st,
                                  uint8_t before) {
    uint8_t ready = (before & (uint8_t)~(UNKNOWN1_PENDING |
                                         UNKNOWN1_COMPLETE)) |
                    UNKNOWN1_SYNC_COMPLETE;
    memory_controller_ram_set(&s->memory, UNKNOWN1_MBOX, ready);
    uint16_t command = ram_word(s, UNKNOWN1_COMMAND);
    uint16_t request = ram_word(s, UNKNOWN1_REQUEST);
    uint16_t index = ram_word(s, UNKNOWN1_INDEX);
    uint16_t trailer = ram_word(s, UNKNOWN1_TRAILER);
    int charger_query = mailbox_charger_query(
        s, command, request, index, trailer);
    uint8_t result = mailbox_result(
        s, command, request, index, trailer, charger_query);
    memory_controller_ram_set(&s->memory, UNKNOWN1_RESULT, result);

    /* Elapsed time cannot turn a preempted synchronous caller into a worker. */
    int irq_mode = (st->control & UNKNOWN1_INFERRED_IRQ_MODE_MASK) != 0;
    st->phase = irq_mode ? XBUS_MAILBOX_GRACE : XBUS_MAILBOX_SYNC;
    st->deadline = irq_mode ? s->ticks + UNKNOWN1_IRQ_DELAY_TICKS : 0;
    uint16_t irq = memory_controller_sfr_get(&s->memory, UNKNOWN1_IRQ);
    emit_mailbox_complete(
        s, st, "sync-ready", st->deadline, before, ready, result,
        irq, irq, command, request, index, trailer, charger_query, 0);
}

static int unknown1_write8(peripheral_t *self, soc_t *s,
                           uint32_t addr, uint8_t value) {
    (void)value;
    xbus_unknown1_state_t *st = (xbus_unknown1_state_t *)self->state;
    if (cemu_xbus_audio_handles(&st->audio, addr))
        return cemu_xbus_audio_write8(&st->audio, s, addr);
    if (addr == UNKNOWN1_STATUS || addr == UNKNOWN1_STATUS + 1) {
        uint8_t launch = memory_controller_ram_present(&s->memory, UNKNOWN1_LAUNCH)
                       ? memory_controller_ram_get(&s->memory, UNKNOWN1_LAUNCH) : 0;
        memory_controller_ram_set(&s->memory, UNKNOWN1_LAUNCH, launch & (uint8_t)~1u);
        return 1;
    }
    if (addr == UNKNOWN1_LAUNCH &&
        memory_controller_ram_present(&s->memory, UNKNOWN1_LAUNCH) &&
        (memory_controller_ram_get(&s->memory, UNKNOWN1_LAUNCH) & 1u))
        return 1;
    if (addr != UNKNOWN1_MBOX) return 0;

    uint8_t v = memory_controller_ram_get(&s->memory, UNKNOWN1_MBOX);
    if (st->phase != XBUS_MAILBOX_IDLE &&
        !(v & UNKNOWN1_SYNC_COMPLETE)) {
        uint64_t deadline = st->deadline;
        uint16_t irq = memory_controller_sfr_get(&s->memory, UNKNOWN1_IRQ);
        mailbox_emit_phase(s, st, "sync-consumed", deadline,
                           UNKNOWN1_SYNC_COMPLETE, v, irq, irq, 0);
        st->phase = XBUS_MAILBOX_IDLE;
        st->deadline = 0;
    }

    if (!(v & UNKNOWN1_PENDING)) return 0;
    if (st->phase != XBUS_MAILBOX_IDLE) {
        /* A second doorbell cannot replace an in-flight transaction. */
        memory_controller_ram_set(&s->memory, UNKNOWN1_MBOX, v & (uint8_t)~UNKNOWN1_PENDING);
        return 1;
    }

    st->doorbell_rings++;
    st->transaction_seq++;
    st->active_transaction_id = st->transaction_seq;
    st->control = ram_word(s, UNKNOWN1_CONTROL);
    if (mailbox_diagnostic_immediate(s))
        mailbox_accept_immediate(s, st, v);
    else
        mailbox_accept_staged(s, st, v);
    return 1;
}

static void unknown1_tick(peripheral_t *self, soc_t *s, int n) {
    (void)n;
    xbus_unknown1_state_t *st = (xbus_unknown1_state_t *)self->state;
    if (st->phase == XBUS_MAILBOX_GRACE && s->ticks >= st->deadline) {
        uint64_t deadline = st->deadline;
        uint8_t before = memory_controller_ram_get(&s->memory, UNKNOWN1_MBOX);
        uint8_t promoted = (before & (uint8_t)~(UNKNOWN1_PENDING |
                                                UNKNOWN1_SYNC_COMPLETE)) |
                           UNKNOWN1_COMPLETE;
        memory_controller_ram_set(&s->memory, UNKNOWN1_MBOX, promoted);
        uint16_t irq_before = memory_controller_sfr_get(&s->memory, UNKNOWN1_IRQ);
        uint16_t irq_after = irq_before | XIC_IR_BIT;
        cemu_memory_controller_sfr_put(&s->memory, UNKNOWN1_IRQ, irq_after);
        mailbox_emit_phase(s, st, "irq-promoted", deadline, before, promoted,
                           irq_before, irq_after, 1);
        st->phase = XBUS_MAILBOX_IDLE;
        st->deadline = 0;
    }
    cemu_xbus_audio_tick(&st->audio, s);
}

static uint64_t unknown1_next_event(peripheral_t *self, soc_t *s) {
    xbus_unknown1_state_t *st = (xbus_unknown1_state_t *)self->state;
    uint64_t next = UINT64_MAX;
    if (st->phase == XBUS_MAILBOX_GRACE)
        next = st->deadline > s->ticks ? st->deadline - s->ticks : 1u;
    uint64_t audio = cemu_xbus_audio_next_event(&st->audio, s);
    if (audio < next) next = audio;
    return next;
}

static void unknown1_advance_quiet(peripheral_t *self, soc_t *s,
                                   uint64_t ticks) {
    (void)self; (void)s; (void)ticks;
    /* The deadline is absolute, so event-free spans require no private update. */
}

static int unknown1_ic_will_fire(peripheral_t *self, soc_t *s,
                                 uint32_t ic_addr) {
    (void)s;
    xbus_unknown1_state_t *st = (xbus_unknown1_state_t *)self->state;
    return ic_addr == UNKNOWN1_IRQ && st->phase == XBUS_MAILBOX_GRACE;
}

void cemu_xbus_unknown1_periph_init(peripheral_t *p, xbus_unknown1_state_t *st,
                                    uint16_t id, int audio_enabled) {
    memset(st, 0, sizeof *st);
    st->id = id;
    cemu_xbus_audio_init(&st->audio, audio_enabled);
    memcpy(st->ranges, UNKNOWN1_RANGES, sizeof UNKNOWN1_RANGES);
    int range_count = 3;
    if (st->audio.enabled) {
        st->ranges[range_count++] = (addr_range_t){
            XBUS_AUDIO_STREAM_BASE, XBUS_AUDIO_STREAM_END,
        };
        st->ranges[range_count++] = (addr_range_t){
            XBUS_AUDIO_COMMAND, XBUS_AUDIO_COMMAND + 5u,
        };
    }
    p->id = "xbus-unknown-1";
    p->state = st;
    p->sfr_words = NULL;   p->n_sfr_words = 0;
    p->byte_ranges = st->ranges; p->n_byte_ranges = range_count;
    p->ic_nodes = UNKNOWN1_IC_NODES; p->n_ic_nodes = 1;
    PERIPHERAL_REG_NAMES(p, UNKNOWN1_REG_NAMES,
                         (int)(sizeof UNKNOWN1_REG_NAMES /
                               sizeof UNKNOWN1_REG_NAMES[0]));
    p->read8 = unknown1_read8;
    p->peek8 = unknown1_peek8;
    p->write8 = unknown1_write8;
    p->read_sfr_word = NULL; p->on_sfr_poll = NULL; p->on_sfr_write = NULL;
    p->tick = unknown1_tick;
    p->next_event_ticks = unknown1_next_event;
    p->advance_quiet = unknown1_advance_quiet;
    p->timer_running = NULL;
    p->ic_will_fire = unknown1_ic_will_fire;
}

void cemu_xbus_unknown1_audio_reset(peripheral_t *p, soc_t *soc) {
    if (!p || !p->state) return;
    xbus_unknown1_state_t *st = p->state;
    cemu_xbus_audio_reset(&st->audio, soc, 0);
}
