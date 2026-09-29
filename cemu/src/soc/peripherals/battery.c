/* Firmware-visible battery terminal voltage input, routed by device config. */
#include <string.h>
#include "battery.h"
#include "gsm_stub.h"
#include "soc.h"

#define BATTERY_ADC_COMMAND 0xE062u
#define BATTERY_ADC_START   0x0800u

static const addr_range_t BATTERY_RANGES[] = {
    {BATTERY_ADC_COMMAND, BATTERY_ADC_COMMAND + 1u},
};

static const reg_name_t BATTERY_NAMES[] = {
    {BATTERY_ADC_COMMAND, "ANALOG_MEASUREMENT_COMMAND"},
};

static uint16_t level_to_mv(const battery_state_t *st, unsigned level) {
    if (!st->curve || !st->n_curve) return 0;
    if (level <= st->curve[0].level) return st->curve[0].millivolts;
    for (size_t i = 1; i < st->n_curve; i++) {
        const battery_curve_point_t *a = &st->curve[i - 1u];
        const battery_curve_point_t *b = &st->curve[i];
        if (level > b->level) continue;
        unsigned span = (unsigned)b->level - a->level;
        unsigned offset = level - a->level;
        int delta = (int)b->millivolts - a->millivolts;
        return (uint16_t)((int)a->millivolts +
                          (delta * (int)offset + (int)span / 2) / (int)span);
    }
    return st->curve[st->n_curve - 1u].millivolts;
}

static long div_raw_for_voltage(long numerator, long denominator) {
    /* At or below the calibrated high endpoint, choose the first raw code
     * whose firmware conversion cannot round above the requested voltage.
     * C55's percentage table treats its upper threshold as the next bucket. */
    if (numerator >= 0)
        return (numerator + denominator - 1) / denominator;
    return (numerator - denominator / 2) / denominator;
}

static int16_t mv_to_raw(const battery_state_t *st, uint16_t mv) {
    long mv_span = (long)st->adc_mv_high - st->adc_mv_low;
    long raw_span = (long)st->adc_raw_low - st->adc_raw_high;
    long numerator = ((long)st->adc_mv_high - mv) * raw_span;
    long raw = (long)st->adc_raw_high
             + div_raw_for_voltage(numerator, mv_span);
    if (raw < -32768) raw = -32768;
    if (raw > 32767) raw = 32767;
    return (int16_t)raw;
}

static void recompute(battery_state_t *st) {
    st->millivolts = level_to_mv(st, st->level);
    st->adc_raw = mv_to_raw(st, st->millivolts);
}

static void emit_state(soc_t *s, const battery_state_t *st,
                       unsigned old_level, int old_charging,
                       const char *source) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "battery_state"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "battery_state";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_value = 1; ev.value = st->level;
    ev.detail = "external battery input";
    cemu_event_field_i64(&ev.info, "adc_raw", st->adc_raw);
    cemu_event_field_bool(&ev.info, "charging", st->charging);
    cemu_event_field_i64(&ev.info, "level", st->level);
    cemu_event_field_i64(&ev.info, "millivolts", st->millivolts);
    cemu_event_field_bool(&ev.info, "old_charging", old_charging);
    cemu_event_field_i64(&ev.info, "old_level", old_level);
    cemu_event_field_string(&ev.info, "source", source ? source : "api");
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static void emit_sample(soc_t *s, const battery_state_t *st,
                        uint16_t command, uint8_t channel,
                        uint16_t result0, uint16_t result1,
                        uint64_t result_sequence) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "battery_sample"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "battery_sample";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = BATTERY_ADC_COMMAND;
    ev.has_size = 1; ev.size = 2;
    ev.has_value = 1; ev.value = command;
    ev.detail = "battery analog conversion";
    cemu_event_field_i64(&ev.info, "adc_raw", st->adc_raw);
    cemu_event_field_i64(&ev.info, "channel", channel);
    cemu_event_field_bool(&ev.info, "charging", st->charging);
    cemu_event_field_i64(&ev.info, "level", st->level);
    cemu_event_field_i64(&ev.info, "millivolts", st->millivolts);
    cemu_event_field_i64(&ev.info, "result0", result0);
    cemu_event_field_i64(&ev.info, "result1", result1);
    cemu_event_field_i64(&ev.info, "result_word", st->adc_result_word);
    cemu_event_field_i64(&ev.info, "result_sequence", (long)result_sequence);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static int battery_write8(peripheral_t *self, soc_t *s,
                          uint32_t addr, uint8_t value) {
    (void)value;
    battery_state_t *st = (battery_state_t *)self->state;
    if (!st->available || addr != BATTERY_ADC_COMMAND) return 0;
    uint16_t command = (uint16_t)(memory_controller_ram_get(&s->memory, BATTERY_ADC_COMMAND) |
                         ((uint16_t)memory_controller_ram_get(&s->memory, BATTERY_ADC_COMMAND + 1u) << 8));
    if (!(command & BATTERY_ADC_START)) return 1;
    uint8_t channel = (uint8_t)(command & 0xFFu);
    uint16_t sample = channel == st->battery_channel
                    ? (uint16_t)st->adc_raw : 0;
    uint16_t result0 = st->adc_result_word == DEVICE_BATTERY_RESULT0
                     ? sample : 0;
    uint16_t result1 = st->adc_result_word == DEVICE_BATTERY_RESULT1
                     ? sample : 0;
    uint64_t result_sequence = cemu_baseband_result_publish(
        s->gsm_stub_periph, s, BASEBAND_RESULT_ADC, command,
        result0, result1);
    st->samples++;
    emit_sample(s, st, command, channel, result0, result1,
                result_sequence);
    return 1;
}

void cemu_battery_periph_init(peripheral_t *p, battery_state_t *st,
                         const device_battery_config_t *cfg) {
    memset(p, 0, sizeof *p);
    memset(st, 0, sizeof *st);
    p->id = "battery";
    p->state = st;
    if (!cfg || !cfg->available ||
        cfg->adc_raw_low <= cfg->adc_raw_high ||
        cfg->adc_mv_low >= cfg->adc_mv_high ||
        cfg->adc_result_word > DEVICE_BATTERY_RESULT1)
        return;
    st->available = 1;
    st->level = 100;
    st->charging = 0;
    st->battery_channel = cfg->battery_channel;
    st->reference_channel = cfg->reference_channel;
    st->adc_result_word = cfg->adc_result_word;
    st->curve = cfg->curve;
    st->n_curve = cfg->n_curve;
    st->adc_raw_high = cfg->adc_raw_high;
    st->adc_mv_high = cfg->adc_mv_high;
    st->adc_raw_low = cfg->adc_raw_low;
    st->adc_mv_low = cfg->adc_mv_low;
    recompute(st);
    p->byte_ranges = BATTERY_RANGES;
    p->n_byte_ranges = 1;
    PERIPHERAL_REG_NAMES(p, BATTERY_NAMES,
                         (int)(sizeof BATTERY_NAMES / sizeof BATTERY_NAMES[0]));
    p->write8 = battery_write8;
}

int cemu_battery_available(const peripheral_t *p) {
    return p && ((const battery_state_t *)p->state)->available;
}

unsigned cemu_battery_level(const peripheral_t *p) {
    return p ? ((const battery_state_t *)p->state)->level : 0;
}

int cemu_battery_charging(const peripheral_t *p) {
    return p && ((const battery_state_t *)p->state)->charging;
}

uint16_t cemu_battery_millivolts(const peripheral_t *p) {
    return p ? ((const battery_state_t *)p->state)->millivolts : 0;
}

int16_t cemu_battery_adc_raw(const peripheral_t *p) {
    return p ? ((const battery_state_t *)p->state)->adc_raw : 0;
}

int cemu_battery_set_state(peripheral_t *p, soc_t *s, unsigned level,
                      int charging, const char *source) {
    if (!cemu_battery_available(p) || level > 100) return 0;
    battery_state_t *st = (battery_state_t *)p->state;
    unsigned old_level = st->level;
    int old_charging = st->charging;
    charging = charging ? 1 : 0;
    if (old_level == level && old_charging == charging) return 1;
    st->level = (uint8_t)level;
    st->charging = (uint8_t)charging;
    recompute(st);
    st->state_changes++;
    if (s) emit_state(s, st, old_level, old_charging, source);
    return 1;
}

void cemu_battery_restore_state(peripheral_t *p, unsigned level, int charging) {
    if (!cemu_battery_available(p) || level > 100) return;
    battery_state_t *st = (battery_state_t *)p->state;
    st->level = (uint8_t)level;
    st->charging = charging ? 1u : 0u;
    recompute(st);
}
