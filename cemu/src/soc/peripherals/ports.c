/* P3/P6/P7/P8 data/direction port surface. */
#include <stdio.h>
#include <string.h>
#include "ports.h"
#include "soc.h"

#define P3   0xFFC4u
#define DP3  0xFFC6u
#define P6   0xFFCCu
#define DP6  0xFFCEu
#define P7   0xFFD0u
#define DP7  0xFFD2u
#define P8   0xFFD4u
#define DP8  0xFFD6u

/* Direction words need no behavioral hook: their canonical SFR storage is
 * consumed when the matching data word is read. Leaving DP3 unclaimed also
 * preserves the keypad peripheral's matrix-column write callback. */
static const uint32_t PORT_SFR_WORDS[] = { P3, P6, P7, P8 };

static const reg_name_t PORT_REG_NAMES[] = {
    { P3, "P3" }, { DP3, "DP3" }, { P6, "P6" }, { DP6, "DP6" },
    { P7, "P7" }, { DP7, "DP7" }, { P8, "P8" }, { DP8, "DP8" },
};

static int port_index(int port) {
    switch (port) {
    case SOC_PORT_P3: return 0;
    case SOC_PORT_P6: return 1;
    case SOC_PORT_P7: return 2;
    case SOC_PORT_P8: return 3;
    default: return -1;
    }
}

static uint16_t *input_level_ref(ports_state_t *st, int port) {
    int index = port_index(port);
    return index >= 0 ? &st->input_level[index] : NULL;
}

static uint16_t *input_valid_ref(ports_state_t *st, int port) {
    int index = port_index(port);
    return index >= 0 ? &st->input_valid[index] : NULL;
}

static uint32_t port_addr(int port) {
    switch (port) {
    case SOC_PORT_P3: return P3;
    case SOC_PORT_P6: return P6;
    case SOC_PORT_P7: return P7;
    case SOC_PORT_P8: return P8;
    default: return 0;
    }
}

static uint32_t direction_addr(int port) {
    switch (port) {
    case SOC_PORT_P3: return DP3;
    case SOC_PORT_P6: return DP6;
    case SOC_PORT_P7: return DP7;
    case SOC_PORT_P8: return DP8;
    default: return 0;
    }
}

static const char *port_name(int port) {
    switch (port) {
    case SOC_PORT_P3: return "P3";
    case SOC_PORT_P6: return "P6";
    case SOC_PORT_P7: return "P7";
    case SOC_PORT_P8: return "P8";
    default: return "P?";
    }
}

static uint16_t visible_port_word(ports_state_t *st, soc_t *s, int port, uint16_t stored) {
    uint16_t *level = input_level_ref(st, port);
    uint16_t *valid = input_valid_ref(st, port);
    if (!level || !valid) return stored;

    uint16_t dp = memory_controller_sfr_get(&s->memory, direction_addr(port));
    uint16_t external_input_mask = (uint16_t)(*valid & ~dp);
    return (uint16_t)((stored & ~external_input_mask) | (*level & external_input_mask));
}

static void trace_port_edge(soc_t *s, int port, int bit, int level, int edge) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "port_edge")) return;

    char detail[32];
    snprintf(detail, sizeof detail, "%s.%d %s", port_name(port), bit,
             edge > 0 ? "rising" : edge < 0 ? "falling" : "level");

    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "port_edge";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1;
    ev.addr = port_addr(port);
    ev.has_size = 1;
    ev.size = 1;
    ev.has_value = 1;
    ev.value = (uint32_t)level;
    ev.detail = detail;
    cemu_event_field_i64(&ev.info, "bit", bit);
    cemu_event_field_string(&ev.info, "edge", edge > 0 ? "rising" : edge < 0 ? "falling" : "none");
    cemu_event_field_i64(&ev.info, "level", level ? 1 : 0);
    cemu_event_field_string(&ev.info, "port", port_name(port));
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static uint16_t ports_read_sfr_word(peripheral_t *self, soc_t *s, uint32_t word_addr, uint16_t stored) {
    ports_state_t *st = (ports_state_t *)self->state;
    if (word_addr == P3) return visible_port_word(st, s, SOC_PORT_P3, stored);
    if (word_addr == P6) return visible_port_word(st, s, SOC_PORT_P6, stored);
    if (word_addr == P7) return visible_port_word(st, s, SOC_PORT_P7, stored);
    if (word_addr == P8) return visible_port_word(st, s, SOC_PORT_P8, stored);
    return stored;
}

static void ports_on_sfr_poll(peripheral_t *self, soc_t *s, uint32_t word_addr) {
    (void)self;
    if (s->serial_autobaud_bypass && word_addr == P7)
        cemu_memory_controller_sfr_put(&s->memory, P7, (uint16_t)(memory_controller_sfr_get(&s->memory, P7) ^ 0x0008u));
}

void cemu_ports_periph_init(peripheral_t *p, ports_state_t *st) {
    memset(st, 0, sizeof(*st));
    p->id = "ports";
    p->state = st;
    p->sfr_words = PORT_SFR_WORDS;  p->n_sfr_words = 4;
    p->byte_ranges = NULL;          p->n_byte_ranges = 0;
    p->ic_nodes = NULL;             p->n_ic_nodes = 0;
    PERIPHERAL_REG_NAMES(p, PORT_REG_NAMES, 8);
    p->read8 = NULL;  p->peek8 = NULL;  p->write8 = NULL;
    p->read_sfr_word = ports_read_sfr_word;
    p->on_sfr_poll = ports_on_sfr_poll;
    p->on_sfr_write = NULL;
    p->tick = NULL;   p->timer_running = NULL;  p->ic_will_fire = NULL;
}

void cemu_ports_input_release(ports_state_t *st, int port, int bit) {
    uint16_t *valid = input_valid_ref(st, port);
    if (!valid || bit < 0 || bit > 15) return;
    *valid &= (uint16_t)~(1u << bit);
}

void cemu_ports_input_restore_level(ports_state_t *st, int port, int bit, int level) {
    if (port_index(port) < 0 || bit < 0 || bit > 15) return;

    uint16_t *input_level = input_level_ref(st, port);
    uint16_t *input_valid = input_valid_ref(st, port);
    uint16_t mask = (uint16_t)(1u << bit);
    *input_valid |= mask;
    if (level) *input_level |= mask;
    else       *input_level &= (uint16_t)~mask;
}

int cemu_ports_input_level(ports_state_t *st, soc_t *s, int port, int bit, int level) {
    if (port_index(port) < 0 || bit < 0 || bit > 15)
        return SOC_PORT_EDGE_INVALID;

    uint16_t *input_level = input_level_ref(st, port);
    uint16_t *input_valid = input_valid_ref(st, port);
    uint16_t mask = (uint16_t)(1u << bit);
    int old = (*input_level & mask) != 0;
    int now = level ? 1 : 0;

    *input_valid |= mask;
    if (now) *input_level |= mask;
    else     *input_level &= (uint16_t)~mask;

    int edge = (old == now) ? SOC_PORT_EDGE_NONE :
               now ? SOC_PORT_EDGE_RISING : SOC_PORT_EDGE_FALLING;
    trace_port_edge(s, port, bit, now, edge);
    return edge;
}
