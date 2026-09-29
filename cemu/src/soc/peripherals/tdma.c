/* PMB7850 GSM TDMA frame timer, bounded to the firmware-observed register use. */
#include <string.h>
#include "tdma.h"
#include "soc.h"

#define TDMA_E020          0xE020u
#define TDMA_FRAME_TOP     0xE022u
#define TDMA_IRQ36_COMPARE 0xE024u
#define TDMA_IRQ37_COMPARE 0xE026u
#define TDMA_RESET         0xE030u
#define TDMA_E032          0xE032u
#define TDMA_COUNTER       0xE034u
#define TDMA_CONTROL       0xE060u
#define TDMA_ENABLE        (1u << 1)
#define IRQ36IC            0xF186u
#define IRQ37IC            0xF18Eu
#define TDMA_DIVISOR       12u

static const addr_range_t TDMA_RANGES[] = {
    { TDMA_E020, TDMA_IRQ37_COMPARE + 1u },
    { TDMA_RESET, TDMA_COUNTER + 1u },
    { TDMA_CONTROL, TDMA_CONTROL + 1u },
};

static const ic_node_t TDMA_IC[] = {
    { IRQ36IC, 0x40, 1 },
    { IRQ37IC, 0x41, 1 },
};

static const reg_name_t TDMA_REG_NAMES[] = {
    { TDMA_E020, "TDMA_E020" },
    { TDMA_FRAME_TOP, "TDMA_FRAME_TOP" },
    { TDMA_IRQ36_COMPARE, "TDMA_IRQ36_COMPARE" },
    { TDMA_IRQ37_COMPARE, "TDMA_IRQ37_COMPARE" },
    { TDMA_RESET, "TDMA_COUNTER_RESET" },
    { TDMA_E032, "TDMA_E032" },
    { TDMA_COUNTER, "TDMA_COUNTER" },
    { TDMA_CONTROL, "TDMA_CONTROL" },
    /* XP0/XP1 are the established firmware-facing aliases for IRQ36/IRQ37. */
    { IRQ36IC, "XP0IC" },
    { IRQ37IC, "XP1IC" },
};

static uint16_t ram_word(soc_t *s, uint32_t addr) {
    return (uint16_t)(memory_controller_ram_get(&s->memory, addr) |
                      ((uint16_t)memory_controller_ram_get(&s->memory, addr + 1u) << 8));
}

static void put_ram_word(soc_t *s, uint32_t addr, uint16_t value) {
    memory_controller_ram_set(&s->memory, addr, (uint8_t)value);
    memory_controller_ram_set(&s->memory, addr + 1u, (uint8_t)(value >> 8));
}

static int tdma_read8(peripheral_t *self, soc_t *s, uint32_t addr) {
    (void)self;
    return memory_controller_ram_present(&s->memory, addr) ? memory_controller_ram_get(&s->memory, addr) : 0;
}

static int tdma_peek8(peripheral_t *self, soc_t *s, uint32_t addr) {
    return tdma_read8(self, s, addr);
}

static int tdma_write8(peripheral_t *self, soc_t *s,
                       uint32_t addr, uint8_t value) {
    (void)self;
    (void)value;
    if (addr == TDMA_RESET && ram_word(s, TDMA_RESET) == 0)
        put_ram_word(s, TDMA_COUNTER, 0);
    return 1;
}

static void tdma_emit_tick(soc_t *s, uint16_t before, uint16_t after,
                           uint16_t top) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "tdma_tick"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "tdma_tick";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = TDMA_COUNTER;
    ev.has_size = 1; ev.size = 2;
    ev.has_value = 1; ev.value = after;
    ev.detail = "TDMA frame counter";
    cemu_event_field_i64(&ev.info, "count_before", before);
    cemu_event_field_i64(&ev.info, "top", top);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void tdma_raise_irq(soc_t *s, uint32_t ic_addr, int trap,
                           uint16_t compare, uint16_t counter,
                           const char *alias) {
    uint16_t old = memory_controller_sfr_get(&s->memory, ic_addr);
    uint16_t value = (uint16_t)(old | XIC_IR_BIT);
    cemu_memory_controller_sfr_put(&s->memory, ic_addr, value);
    if (!cemu_event_native_trace_active(&s->instrumentation, "tdma_irq"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "tdma_irq";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = ic_addr;
    ev.has_size = 1; ev.size = 2;
    ev.has_value = 1; ev.value = value;
    ev.detail = alias;
    cemu_event_field_bool(&ev.info, "already_pending", (old & XIC_IR_BIT) != 0);
    cemu_event_field_i64(&ev.info, "compare", compare);
    cemu_event_field_i64(&ev.info, "counter", counter);
    cemu_event_field_i64(&ev.info, "trap", trap);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void tdma_tick(peripheral_t *self, soc_t *s, int n) {
    (void)self;
    (void)n;
    if (!(ram_word(s, TDMA_CONTROL) & TDMA_ENABLE) ||
        s->ticks % TDMA_DIVISOR != 0)
        return;

    uint16_t top = ram_word(s, TDMA_FRAME_TOP);
    uint16_t before = ram_word(s, TDMA_COUNTER);
    uint16_t after = before >= top ? 0 : (uint16_t)(before + 1u);
    put_ram_word(s, TDMA_COUNTER, after);
    tdma_emit_tick(s, before, after, top);

    uint16_t irq37_compare = ram_word(s, TDMA_IRQ37_COMPARE);
    uint16_t irq36_compare = ram_word(s, TDMA_IRQ36_COMPARE);
    if (after == irq37_compare)
        tdma_raise_irq(s, IRQ37IC, 0x41, irq37_compare, after, "IRQ37/XP1");
    if (after == irq36_compare)
        tdma_raise_irq(s, IRQ36IC, 0x40, irq36_compare, after, "IRQ36/XP0");
}

static uint64_t tdma_next_event(peripheral_t *self, soc_t *s) {
    (void)self;
    if (!(ram_word(s, TDMA_CONTROL) & TDMA_ENABLE)) return UINT64_MAX;
    uint64_t phase = s->ticks % TDMA_DIVISOR;
    return phase ? TDMA_DIVISOR - phase : TDMA_DIVISOR;
}
static void tdma_advance_quiet(peripheral_t *self, soc_t *s, uint64_t ticks) {
    (void)self; (void)s; (void)ticks;
}

static int tdma_ic_will_fire(peripheral_t *self, soc_t *s,
                             uint32_t ic_addr) {
    (void)self;
    if (!(ram_word(s, TDMA_CONTROL) & TDMA_ENABLE)) return 0;
    uint16_t top = ram_word(s, TDMA_FRAME_TOP);
    if (ic_addr == IRQ36IC)
        return ram_word(s, TDMA_IRQ36_COMPARE) <= top;
    if (ic_addr == IRQ37IC)
        return ram_word(s, TDMA_IRQ37_COMPARE) <= top;
    return 0;
}

void cemu_tdma_periph_init(peripheral_t *p) {
    memset(p, 0, sizeof *p);
    p->id = "tdma";
    p->byte_ranges = TDMA_RANGES;
    p->n_byte_ranges = (int)(sizeof TDMA_RANGES / sizeof TDMA_RANGES[0]);
    p->ic_nodes = TDMA_IC;
    p->n_ic_nodes = (int)(sizeof TDMA_IC / sizeof TDMA_IC[0]);
    PERIPHERAL_REG_NAMES(p, TDMA_REG_NAMES, (int)(sizeof TDMA_REG_NAMES / sizeof TDMA_REG_NAMES[0]));
    p->read8 = tdma_read8;
    p->peek8 = tdma_peek8;
    p->write8 = tdma_write8;
    p->tick = tdma_tick;
    p->next_event_ticks = tdma_next_event;
    p->advance_quiet = tdma_advance_quiet;
    p->timer_running = tdma_ic_will_fire;
    p->ic_will_fire = tdma_ic_will_fire;
}
