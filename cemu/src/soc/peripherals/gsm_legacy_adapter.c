#include <string.h>
#include "gsm_legacy_adapter.h"
#include "gsm_stub.h"
#include "soc.h"

#define GSM_REGISTERED_SELECTOR 4u
#define GSM_SEARCH_STATE        7u

static gsm_stub_state_t *transport(soc_t *s) {
    return s->gsm_stub_periph
         ? (gsm_stub_state_t *)s->gsm_stub_periph->state : NULL;
}

static int registration_inputs_ready(soc_t *s,
                                     gsm_legacy_adapter_state_t *st) {
    gsm_stub_state_t *bb = transport(s);
    if (!st->enabled || !bb || !bb->cell.available ||
        !bb->cell.synchronized || !bb->cell.service_allowed ||
        st->registration_events || !st->adapter_pc || !st->home_plmn_addr ||
        !st->network_state_addr || !st->candidate_ptr_addr ||
        !st->candidate_seg_addr || !s->cpu || !s->cpu->idle)
        return 0;
    if (s->cpu->icount < st->min_icount) return 0;
    if (cemu_memory_controller_peek16(&s->memory, st->network_state_addr) !=
        GSM_SEARCH_STATE)
        return 0;
    uint16_t off = cemu_memory_controller_peek16(&s->memory,
                                             st->candidate_ptr_addr);
    uint16_t seg = cemu_memory_controller_peek16(&s->memory,
                                             st->candidate_seg_addr);
    if ((off == 0 && seg == 0) || (off == 0xFFFF && seg == 0xFFFF)) return 0;
    uint8_t first = cemu_memory_controller_peek8(&s->memory, st->home_plmn_addr);
    return first != 0 && first != 0xFF;
}

static void emit_registration(soc_t *s, gsm_legacy_adapter_state_t *st,
                              uint16_t payload_off, uint16_t payload_seg) {
    if (!cemu_event_native_trace_active(&s->instrumentation,
                                        "gsm_registration")) return;
    gsm_stub_state_t *bb = transport(s);
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "gsm_registration";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = st->adapter_pc;
    ev.has_value = 1; ev.value = GSM_REGISTERED_SELECTOR;
    ev.detail = "legacy synthetic C55 decoded registration call";
    cemu_event_field_i64(&ev.info, "event", (long)st->registration_events);
    cemu_event_field_i64(&ev.info, "payload_offset", payload_off);
    cemu_event_field_i64(&ev.info, "payload_segment", payload_seg);
    cemu_event_field_i64(&ev.info, "synchronized_responses",
                   bb ? (long)bb->synchronized_responses : 0);
    cemu_event_field_i64(&ev.info, "plmn_bcd",
        cemu_memory_controller_peek8(&s->memory, st->home_plmn_addr) |
        ((long)cemu_memory_controller_peek8(&s->memory, st->home_plmn_addr + 1u) << 8) |
        ((long)cemu_memory_controller_peek8(&s->memory, st->home_plmn_addr + 2u) << 16));
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static void emit_signal(soc_t *s, gsm_legacy_adapter_state_t *st) {
    if (!cemu_event_native_trace_active(&s->instrumentation,
                                        "gsm_signal")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "gsm_signal";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = st->signal_setter_pc;
    ev.has_value = 1; ev.value = st->signal_level;
    ev.detail = "legacy synthetic C55 reception-strength call";
    cemu_event_field_i64(&ev.info, "event", (long)st->signal_events);
    cemu_event_field_i64(&ev.info, "level", st->signal_level);
    cemu_event_field_i64(&ev.info, "registration_event",
                   (long)st->registration_events);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
}

static void inject_far_call(soc_t *s, uint32_t target_pc,
                            uint16_t r12, uint16_t r13,
                            uint16_t r14, uint16_t r15) {
    cpu_t *cpu = s->cpu;
    uint16_t return_csp = cpu->csp;
    uint16_t return_ip = cpu->ip;
    uint16_t sp = bus_read16(&s->bus, 0xFE12u);
    /* Match the M166 CALLS contract: push segment then offset. RETS consumes
     * offset then segment and resumes the saved pre-IDLE continuation. */
    sp = (uint16_t)(sp - 2u);
    bus_write16(&s->bus, 0xFE12u, sp);
    bus_write16(&s->bus, sp, return_csp);
    sp = (uint16_t)(sp - 2u);
    bus_write16(&s->bus, 0xFE12u, sp);
    bus_write16(&s->bus, sp, return_ip);
    cemu_cpu_set_gpr(cpu, 12, r12);
    cemu_cpu_set_gpr(cpu, 13, r13);
    cemu_cpu_set_gpr(cpu, 14, r14);
    cemu_cpu_set_gpr(cpu, 15, r15);
    cpu->csp = (uint16_t)((target_pc >> 16) & 0xFFu);
    cpu->ip = (uint16_t)target_pc;
    cpu->idle = 0;
}

static void inject_registration(soc_t *s, gsm_legacy_adapter_state_t *st) {
    if (!registration_inputs_ready(s, st)) return;
    uint16_t off = (uint16_t)(st->home_plmn_addr & 0x3FFFu);
    uint16_t seg = (uint16_t)(st->home_plmn_addr / 0x4000u);
    st->return_pc = cpu_pc(s->cpu);
    inject_far_call(s, st->adapter_pc, GSM_REGISTERED_SELECTOR, 0, off, seg);
    st->registration_events++;
    emit_registration(s, st, off, seg);
}

static int signal_inputs_ready(soc_t *s, gsm_legacy_adapter_state_t *st) {
    return st->enabled && st->registration_events == 1 &&
           st->signal_events == 0 && st->signal_setter_pc && s->cpu &&
           s->cpu->idle && cpu_pc(s->cpu) == st->return_pc;
}

static void inject_signal(soc_t *s, gsm_legacy_adapter_state_t *st) {
    if (!signal_inputs_ready(s, st)) return;
    inject_far_call(s, st->signal_setter_pc, st->signal_level, 0, 0, 0);
    st->signal_events++;
    emit_signal(s, st);
}

static void legacy_tick(peripheral_t *self, soc_t *s, int n) {
    (void)n;
    gsm_legacy_adapter_state_t *st = self->state;
    inject_registration(s, st);
    inject_signal(s, st);
}

static uint64_t legacy_next_event(peripheral_t *self, soc_t *s) {
    gsm_legacy_adapter_state_t *st = self->state;
    return registration_inputs_ready(s, st) || signal_inputs_ready(s, st)
         ? 1 : UINT64_MAX;
}

static void legacy_advance_quiet(peripheral_t *self, soc_t *s,
                                 uint64_t ticks) {
    (void)self; (void)s; (void)ticks;
}

void cemu_gsm_legacy_adapter_periph_init(
    peripheral_t *p, gsm_legacy_adapter_state_t *st,
    const device_gsm_legacy_adapter_config_t *cfg) {
    memset(p, 0, sizeof *p);
    memset(st, 0, sizeof *st);
    if (cfg) {
        st->adapter_pc = cfg->registration_adapter_pc;
        st->home_plmn_addr = cfg->registration_home_plmn_addr;
        st->network_state_addr = cfg->registration_network_state_addr;
        st->candidate_ptr_addr = cfg->registration_candidate_ptr_addr;
        st->candidate_seg_addr = cfg->registration_candidate_seg_addr;
        st->signal_setter_pc = cfg->registration_signal_setter_pc;
        st->signal_level = cfg->registration_signal_level;
        st->min_icount = cfg->registration_min_icount;
    }
    p->id = "gsm-legacy-adapter";
    p->state = st;
    p->tick = legacy_tick;
    p->next_event_ticks = legacy_next_event;
    p->advance_quiet = legacy_advance_quiet;
}

void cemu_gsm_legacy_adapter_set_enabled(peripheral_t *p, int enabled) {
    gsm_legacy_adapter_state_t *st = p->state;
    if (st->restored) {
        st->restored = 0;
        st->enabled = enabled && st->adapter_pc && st->signal_setter_pc;
        return;
    }
    gsm_legacy_adapter_state_t configured = *st;
    memset(st, 0, sizeof *st);
    st->adapter_pc = configured.adapter_pc;
    st->home_plmn_addr = configured.home_plmn_addr;
    st->network_state_addr = configured.network_state_addr;
    st->candidate_ptr_addr = configured.candidate_ptr_addr;
    st->candidate_seg_addr = configured.candidate_seg_addr;
    st->signal_setter_pc = configured.signal_setter_pc;
    st->signal_level = configured.signal_level;
    st->min_icount = configured.min_icount;
    st->enabled = enabled && st->adapter_pc && st->signal_setter_pc;
}
