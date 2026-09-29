#include <string.h>
#include "capcom_common.h"
#include "base.h"
#include "soc.h"

enum {
    CAPCOM_MODE_DISABLED = 0,
    CAPCOM_MODE_CAPTURE_RISING = 1,
    CAPCOM_MODE_CAPTURE_FALLING = 2,
    CAPCOM_MODE_CAPTURE_BOTH = 3,
    CAPCOM_MODE_COMPARE0 = 4,
    CAPCOM_MODE_COMPARE1 = 5,
    CAPCOM_MODE_COMPARE2 = 6,
    CAPCOM_MODE_COMPARE3 = 7,
};

typedef struct {
    int timer_idx;
    int mode;
} capcom_channel_cfg_t;

typedef struct {
    int run;
    int counter_mode;
    int input_sel;
} capcom_timer_cfg_t;

static capcom_timer_cfg_t timer_cfg(soc_t *s, const capcom_unit_desc_t *desc, int timer_idx) {
    uint16_t con = memory_controller_sfr_get(&s->memory, desc->con_addr);
    int shift = timer_idx ? 8 : 0;
    capcom_timer_cfg_t cfg;
    cfg.run = (con >> (shift + 6)) & 1;
    cfg.counter_mode = (con >> (shift + 3)) & 1;
    cfg.input_sel = (con >> shift) & 0x7;
    return cfg;
}

static capcom_channel_cfg_t channel_cfg(soc_t *s, const capcom_unit_desc_t *desc, int local_channel) {
    uint16_t ccm = memory_controller_sfr_get(&s->memory, desc->ccm_addr[local_channel >> 2]);
    uint8_t field = (uint8_t)((ccm >> ((local_channel & 3) * 4)) & 0xF);
    capcom_channel_cfg_t cfg;
    cfg.timer_idx = (field >> 3) & 1;
    cfg.mode = field & 0x7;
    return cfg;
}

static int channel_has_output(const capcom_unit_desc_t *desc, int local_channel) {
    int global = desc->base_channel + local_channel;
    if (desc->no_output_lo < 0) return 1;
    return global < desc->no_output_lo || global > desc->no_output_hi;
}

static int channel_irq_output_enabled(const capcom_unit_state_t *st,
                                      int local_channel) {
    return (st->irq_output_mask & (uint16_t)(1u << local_channel)) != 0;
}

static void set_ir(soc_t *s, uint32_t ic_addr) {
    cemu_memory_controller_sfr_put(&s->memory, ic_addr, memory_controller_sfr_get(&s->memory, ic_addr) | XIC_IR_BIT);
}

static void set_output_level(capcom_unit_state_t *st, soc_t *s,
                             const capcom_unit_desc_t *desc, int local_channel,
                             int source_channel, int timer_idx,
                             uint16_t timer_value, uint8_t level,
                             const char *reason) {
    capcom_channel_state_t *channel = &st->channels[local_channel];
    level = level ? 1u : 0u;
    if (channel->output_level == level) return;
    channel->output_level = level;

    cemu_soc_audio_capcom_edge(
        s, desc->base_channel + local_channel, level);

    if (!cemu_event_native_trace_active(&s->instrumentation, "capcom_output"))
        return;

    capcom_channel_cfg_t cfg = channel_cfg(s, desc, local_channel);
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "capcom_output";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1;
    ev.addr = desc->cc_addr[local_channel];
    ev.has_size = 1;
    ev.size = 1;
    ev.has_value = 1;
    ev.value = level;
    ev.detail = "CAPCOM output edge";
    cemu_event_field_i64(&ev.info, "channel", desc->base_channel + local_channel);
    cemu_event_field_string(&ev.info, "edge", level ? "rising" : "falling");
    cemu_event_field_i64(&ev.info, "mode", cfg.mode);
    cemu_event_field_string(&ev.info, "reason", reason);
    cemu_event_field_i64(&ev.info, "source_channel",
                   desc->base_channel + source_channel);
    cemu_event_field_string(&ev.info, "timer", desc->timer_name[timer_idx]);
    cemu_event_field_i64(&ev.info, "timer_value", timer_value);
    cemu_event_field_string(&ev.info, "unit", desc->id);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static void reset_mode23_period(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                                int timer_idx, uint16_t reload) {
    for (int i = 0; i < desc->channel_count; i++) {
        capcom_channel_cfg_t cfg = channel_cfg(s, desc, i);
        if (cfg.timer_idx != timer_idx) continue;
        if (cfg.mode == CAPCOM_MODE_COMPARE2 || cfg.mode == CAPCOM_MODE_COMPARE3)
            st->channels[i].period_blocked = 0;
        if (cfg.mode != CAPCOM_MODE_COMPARE3 || !channel_has_output(desc, i)) continue;
        if (memory_controller_sfr_get(&s->memory, desc->cc_addr[i]) == reload) continue;
        set_output_level(st, s, desc, i, i, timer_idx, reload, 0,
                         "timer_reload");
    }
}

static void evaluate_compare(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                             int timer_idx, uint16_t timer_value, int overflowed, uint16_t reload) {
    uint8_t raise_ir[16];
    uint8_t toggle_output[16];
    uint8_t set_output[16];
    int8_t output_source[16];
    memset(raise_ir, 0, sizeof raise_ir);
    memset(toggle_output, 0, sizeof toggle_output);
    memset(set_output, 0, sizeof set_output);
    memset(output_source, -1, sizeof output_source);

    for (int i = 0; i < desc->channel_count; i++) {
        capcom_channel_cfg_t cfg = channel_cfg(s, desc, i);
        if (cfg.timer_idx != timer_idx) continue;
        if (cfg.mode < CAPCOM_MODE_COMPARE0) continue;
        if (!st->timers[timer_idx].compare_ready) continue;
        if ((cfg.mode == CAPCOM_MODE_COMPARE2 || cfg.mode == CAPCOM_MODE_COMPARE3) &&
            st->channels[i].period_blocked)
            continue;
        if (memory_controller_sfr_get(&s->memory, desc->cc_addr[i]) != timer_value) continue;

        raise_ir[i] = 1;
        switch (cfg.mode) {
            case CAPCOM_MODE_COMPARE0:
                if (i >= 8) {
                    capcom_channel_cfg_t bank1 = channel_cfg(s, desc, i - 8);
                    if (bank1.mode == CAPCOM_MODE_COMPARE1 && channel_has_output(desc, i - 8)) {
                        toggle_output[i - 8] = 1;
                        output_source[i - 8] = (int8_t)i;
                    }
                }
                break;
            case CAPCOM_MODE_COMPARE1:
                if (channel_has_output(desc, i)) {
                    toggle_output[i] = 1;
                    output_source[i] = (int8_t)i;
                }
                break;
            case CAPCOM_MODE_COMPARE2:
                st->channels[i].period_blocked = 1;
                break;
            case CAPCOM_MODE_COMPARE3:
                st->channels[i].period_blocked = 1;
                if (channel_has_output(desc, i) && !(overflowed && timer_value == reload)) {
                    set_output[i] = 1;
                    output_source[i] = (int8_t)i;
                }
                break;
            default:
                break;
        }
    }

    for (int i = 0; i < desc->channel_count; i++) {
        if (raise_ir[i] && channel_irq_output_enabled(st, i))
            set_ir(s, desc->cc_ic_addr[i]);
        if (toggle_output[i])
            set_output_level(st, s, desc, i, output_source[i], timer_idx,
                             timer_value,
                             (uint8_t)!st->channels[i].output_level,
                             "compare_match");
        if (set_output[i])
            set_output_level(st, s, desc, i, output_source[i], timer_idx,
                             timer_value, 1, "compare_match");
    }
}

static int timer_advances_autonomously(soc_t *s, const capcom_unit_desc_t *desc, int timer_idx) {
    capcom_timer_cfg_t cfg = timer_cfg(s, desc, timer_idx);
    if (!cfg.run) return 0;
    if (!cfg.counter_mode) return 1;
    if (cfg.input_sel != 0) return 0;
    return cemu_gpt_running(s, 0xFF48u);
}

static int compare_match_reachable(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                                   int local_channel) {
    capcom_channel_cfg_t cfg = channel_cfg(s, desc, local_channel);
    uint16_t compare;
    uint16_t current;
    uint16_t reload;

    if (cfg.mode < CAPCOM_MODE_COMPARE0) return 0;
    if (!timer_advances_autonomously(s, desc, cfg.timer_idx)) return 0;

    compare = memory_controller_sfr_get(&s->memory, desc->cc_addr[local_channel]);
    current = memory_controller_sfr_get(&s->memory, desc->timer_addr[cfg.timer_idx]);
    reload = memory_controller_sfr_get(&s->memory, desc->reload_addr[cfg.timer_idx]);

    if ((cfg.mode == CAPCOM_MODE_COMPARE2 || cfg.mode == CAPCOM_MODE_COMPARE3) &&
        st->channels[local_channel].period_blocked)
        return compare >= reload;

    return compare > current || compare >= reload;
}

static void step_timer_once(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                            int timer_idx) {
    uint16_t value = memory_controller_sfr_get(&s->memory, desc->timer_addr[timer_idx]);
    uint16_t reload = memory_controller_sfr_get(&s->memory, desc->reload_addr[timer_idx]);
    int overflowed = value == 0xFFFFu;

    value = overflowed ? reload : (uint16_t)(value + 1u);
    if (overflowed) {
        set_ir(s, desc->timer_ic_addr[timer_idx]);
        reset_mode23_period(st, s, desc, timer_idx, reload);
    }

    cemu_memory_controller_sfr_put(&s->memory, desc->timer_addr[timer_idx], value);
    st->timers[timer_idx].compare_ready = 1;
    evaluate_compare(st, s, desc, timer_idx, value, overflowed, reload);
}

static int timer_tick_source(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                             int timer_idx) {
    capcom_timer_cfg_t cfg = timer_cfg(s, desc, timer_idx);
    if (!cfg.run) return 0;

    if (!cfg.counter_mode) {
        uint32_t div = 1u << (cfg.input_sel + 3);
        st->timers[timer_idx].prescaler += 1;
        if (st->timers[timer_idx].prescaler < div) return 0;
        st->timers[timer_idx].prescaler %= div;
        return 1;
    }

    if (cfg.input_sel == 0) return s->capcom_t6_pulse != 0;
    if (timer_idx == 1) return 0;
    return 0;
}

static int capture_edge_match(int mode, int rising) {
    if (mode == CAPCOM_MODE_CAPTURE_BOTH) return 1;
    if (mode == CAPCOM_MODE_CAPTURE_RISING) return rising != 0;
    if (mode == CAPCOM_MODE_CAPTURE_FALLING) return rising == 0;
    return 0;
}

static int counter_edge_match(int input_sel, int rising) {
    if (input_sel == 3) return 1;
    if (input_sel == 1) return rising != 0;
    if (input_sel == 2) return rising == 0;
    return 0;
}

void cemu_capcom_unit_reset_state(capcom_unit_state_t *st) {
    memset(st, 0, sizeof(*st));
    st->irq_output_mask = UINT16_MAX;
}

void cemu_capcom_unit_set_channel_irq_output(capcom_unit_state_t *st,
                                        int local_channel, int enabled) {
    if (local_channel < 0 || local_channel >= 16) return;
    uint16_t bit = (uint16_t)(1u << local_channel);
    if (enabled) st->irq_output_mask |= bit;
    else st->irq_output_mask &= (uint16_t)~bit;
}

void cemu_capcom_unit_tick(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc, int n) {
    for (int i = 0; i < n; i++) {
        for (int timer_idx = 0; timer_idx < 2; timer_idx++) {
            if (timer_tick_source(st, s, desc, timer_idx))
                step_timer_once(st, s, desc, timer_idx);
        }
    }
}

void cemu_capcom_unit_on_sfr_write(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                              uint32_t word_addr, uint16_t stored) {
    (void)stored;
    for (int i = 0; i < 2; i++) {
        if (word_addr == desc->timer_addr[i]) {
            st->timers[i].compare_ready = 1;
            return;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (word_addr != desc->ccm_addr[i]) continue;
        for (int ch = i * 4; ch < i * 4 + 4; ch++) {
            int mode = channel_cfg(s, desc, ch).mode;
            if (mode != CAPCOM_MODE_COMPARE2 && mode != CAPCOM_MODE_COMPARE3)
                st->channels[ch].period_blocked = 0;
        }
        return;
    }
}

int cemu_capcom_unit_timer_running(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                              uint32_t ic_addr) {
    (void)st;
    for (int i = 0; i < 2; i++) {
        if (ic_addr != desc->timer_ic_addr[i]) continue;
        return timer_advances_autonomously(s, desc, i);
    }
    return 0;
}

int cemu_capcom_unit_ic_will_fire(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                             uint32_t ic_addr) {
    for (int i = 0; i < 2; i++)
        if (ic_addr == desc->timer_ic_addr[i])
            return timer_advances_autonomously(s, desc, i);
    for (int i = 0; i < desc->channel_count; i++)
        if (ic_addr == desc->cc_ic_addr[i])
            return channel_irq_output_enabled(st, i) &&
                   compare_match_reachable(st, s, desc, i);
    return 0;
}

uint64_t cemu_capcom_unit_next_event(capcom_unit_state_t *st, soc_t *s,
                                const capcom_unit_desc_t *desc) {
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < 2; i++) {
        capcom_timer_cfg_t cfg = timer_cfg(s, desc, i);
        if (!cfg.run || cfg.counter_mode) continue;
        uint32_t div = 1u << (cfg.input_sel + 3);
        uint64_t candidate = st->timers[i].prescaler >= div ? 1 : div - st->timers[i].prescaler;
        if (candidate < next) next = candidate;
    }
    return next;
}

void cemu_capcom_unit_advance_quiet(capcom_unit_state_t *st, soc_t *s,
                               const capcom_unit_desc_t *desc, uint64_t ticks) {
    for (int i = 0; i < 2; i++) {
        capcom_timer_cfg_t cfg = timer_cfg(s, desc, i);
        if (!cfg.run || cfg.counter_mode) continue;
        uint32_t div = 1u << (cfg.input_sel + 3);
        uint64_t next = st->timers[i].prescaler >= div ? 1 : div - st->timers[i].prescaler;
        if (ticks < next) st->timers[i].prescaler += (uint32_t)ticks;
    }
}

void cemu_capcom_unit_inject_timer_edge(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                                   int timer_idx, int rising) {
    capcom_timer_cfg_t cfg;
    if (timer_idx < 0 || timer_idx > 1) return;
    cfg = timer_cfg(s, desc, timer_idx);
    st->timers[timer_idx].input_level = rising ? 1u : 0u;
    if (!cfg.run || !cfg.counter_mode) return;
    if (timer_idx == 1) return;
    if (counter_edge_match(cfg.input_sel, rising))
        step_timer_once(st, s, desc, timer_idx);
}

void cemu_capcom_unit_inject_channel_edge(capcom_unit_state_t *st, soc_t *s, const capcom_unit_desc_t *desc,
                                     int local_channel, int rising) {
    capcom_channel_cfg_t cfg;
    int timer_idx;
    if (local_channel < 0 || local_channel >= desc->channel_count) return;
    st->channels[local_channel].input_level = rising ? 1u : 0u;
    cfg = channel_cfg(s, desc, local_channel);
    if (!capture_edge_match(cfg.mode, rising)) return;
    timer_idx = cfg.timer_idx;
    cemu_memory_controller_sfr_put(&s->memory, desc->cc_addr[local_channel], memory_controller_sfr_get(&s->memory, desc->timer_addr[timer_idx]));
    if (channel_irq_output_enabled(st, local_channel))
        set_ir(s, desc->cc_ic_addr[local_channel]);
}

int cemu_capcom_unit_output_level(const capcom_unit_state_t *st, const capcom_unit_desc_t *desc,
                             int local_channel) {
    if (local_channel < 0 || local_channel >= desc->channel_count) return 0;
    if (!channel_has_output(desc, local_channel)) return 0;
    return st->channels[local_channel].output_level != 0;
}

int cemu_capcom_unit_output_driven(soc_t *s, const capcom_unit_desc_t *desc,
                              int local_channel) {
    if (local_channel < 0 || local_channel >= desc->channel_count) return 0;
    if (!channel_has_output(desc, local_channel)) return 0;
    int mode = channel_cfg(s, desc, local_channel).mode;
    return mode == CAPCOM_MODE_COMPARE1 || mode == CAPCOM_MODE_COMPARE3;
}
