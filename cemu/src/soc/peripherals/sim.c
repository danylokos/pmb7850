/* PMB7850 SIM interface and deterministic GSM 11.11 stub card. */
#include <string.h>
#include "sim.h"
#include "soc.h"

#define SIM_CTRL_ACTIVATE (1u << 13)
#define SIM_INITIAL_STATUS (1u << 1)
#define SIM_BYTE_TICKS 256u
#define SIM_RESULT_HI 0xEF60u
#define SIM_RESULT_LO 0xEF62u
#define PECC3  0xFEC6u
#define PECSN3 0xFED6u
#define SRCP3  0xFCECu
#define DSTP3  0xFCEEu

enum { INPUT_IDLE, INPUT_PPS, INPUT_APDU_HEADER, INPUT_APDU_DATA };

static const addr_range_t SIM_RANGES[] = {{SIM_REG_FIRST, SIM_REG_LAST + 1u}};
static const ic_node_t SIM_IC[] = {
    {SIM_BYTE_IC, 0x44, 0}, {SIM_STATUS_IC, 0x45, 0}, {SIM_EVENT_IC, 0x46, 0},
};
static const reg_name_t SIM_NAMES[] = {
    {SIM_CTRL, "SIM_CTRL"}, {SIM_RATE, "SIM_RATE"},
    {SIM_STATUS, "SIM_STATUS"}, {SIM_CONFIG, "SIM_CONFIG"},
    {SIM_TX, "SIM_TX"}, {SIM_RX, "SIM_RX"},
    {SIM_FRAME, "SIM_FRAME"}, {SIM_GUARD, "SIM_GUARD"},
    {0xEF60u, "SIM_EF60"}, {0xEF62u, "SIM_EF62"},
    {0xEF64u, "SIM_EF64"}, {0xEF66u, "SIM_EF66"},
    {0xEF68u, "SIM_EF68"}, {0xEF6Au, "SIM_EF6A"},
    {0xEF6Cu, "SIM_EF6C"},
    {SIM_BYTE_IC, "SIM_BYTE_IC"}, {SIM_STATUS_IC, "SIM_STATUS_IC"},
    {SIM_EVENT_IC, "SIM_EVENT_IC"},
};

static uint16_t ram_word(soc_t *s, uint32_t addr) {
    return (uint16_t)(memory_controller_ram_get(&s->memory, addr) |
                      ((uint16_t)memory_controller_ram_get(&s->memory, addr + 1u) << 8));
}

static void put_word(soc_t *s, uint32_t addr, uint16_t value) {
    memory_controller_ram_set(&s->memory, addr, (uint8_t)value);
    memory_controller_ram_set(&s->memory, addr + 1u, (uint8_t)(value >> 8));
}

static void emit_control(soc_t *s, uint16_t before, uint16_t after,
                         const char *detail) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "sim_control")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "sim_control";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = SIM_CTRL;
    ev.has_size = 1; ev.size = 2;
    ev.has_value = 1; ev.value = after;
    ev.detail = detail;
    cemu_event_field_i64(&ev.info, "before", before);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void raise_irq(soc_t *s, uint32_t addr, const char *reason) {
    uint16_t old = memory_controller_sfr_get(&s->memory, addr);
    uint16_t value = old | XIC_IR_BIT;
    cemu_memory_controller_sfr_put(&s->memory, addr, value);
    if (!cemu_event_native_trace_active(&s->instrumentation, "sim_irq")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "sim_irq";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = addr;
    ev.has_size = 1; ev.size = 2;
    ev.has_value = 1; ev.value = value;
    ev.detail = reason;
    cemu_event_field_bool(&ev.info, "already_pending", (old & XIC_IR_BIT) != 0);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void emit_byte(soc_t *s, sim_state_t *st, uint8_t byte,
                      const char *direction, const char *path) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "sim_byte")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "sim_byte";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1;
    ev.addr = !strcmp(direction, "card_to_phone") ? SIM_RX : SIM_TX;
    ev.has_size = 1; ev.size = 1;
    ev.has_value = 1; ev.value = byte;
    ev.detail = path;
    cemu_event_field_string(&ev.info, "direction", direction);
    cemu_event_field_i64(&ev.info, "sequence", (long)st->byte_seq);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void emit_apdu(soc_t *s, sim_state_t *st, uint16_t df_before,
                      uint16_t ef_before, int in_len, int out_len,
                      uint16_t sw) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "sim_apdu")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "sim_apdu";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_size = 1; ev.size = in_len;
    ev.has_value = 1; ev.value = sw;
    ev.detail = "GSM T=0 APDU";
    cemu_event_field_i64(&ev.info, "cla", st->input[0]);
    cemu_event_field_i64(&ev.info, "ins", st->input[1]);
    cemu_event_field_i64(&ev.info, "p1", st->input[2]);
    cemu_event_field_i64(&ev.info, "p2", st->input[3]);
    cemu_event_field_i64(&ev.info, "p3", st->input[4]);
    cemu_event_field_i64(&ev.info, "selected_df_before", df_before);
    cemu_event_field_i64(&ev.info, "selected_ef_before", ef_before);
    cemu_event_field_i64(&ev.info, "selected_df", st->selected_df);
    cemu_event_field_i64(&ev.info, "selected_ef", st->selected_ef);
    if (st->input[1] == 0xA4 && in_len >= 7)
        cemu_event_field_i64(&ev.info, "requested_file",
                       ((long)st->input[5] << 8) | st->input[6]);
    else
        cemu_event_field_null(&ev.info, "requested_file");
    cemu_event_field_i64(&ev.info, "status_word", sw);
    cemu_event_field_i64(&ev.info, "response_length", out_len);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void queue_byte(sim_state_t *st, uint8_t value, int requires_rx) {
    if (st->output_len >= SIM_QUEUE_CAP) return;
    uint16_t at = (uint16_t)((st->output_head + st->output_len) % SIM_QUEUE_CAP);
    st->output[at] = value;
    st->output_requires_rx[at] = requires_rx != 0;
    st->output_internal[at] = 0;
    st->output_len++;
}

static void queue_bytes(sim_state_t *st, const uint8_t *bytes, int n,
                        int requires_rx) {
    for (int i = 0; i < n; i++)
        queue_byte(st, bytes[i], requires_rx);
}

static void queue_procedure_byte(sim_state_t *st, uint8_t value) {
    queue_byte(st, value, 1);
    uint16_t at = (uint16_t)((st->output_head + st->output_len - 1u) %
                             SIM_QUEUE_CAP);
    st->output_internal[at] = 1;
}

static void output_pop(sim_state_t *st) {
    if (!st->output_len) return;
    st->output_head = (uint16_t)((st->output_head + 1u) % SIM_QUEUE_CAP);
    st->output_len--;
}

static void add_file(sim_state_t *st, uint16_t parent_df, uint16_t id,
                     sim_file_structure_t structure, uint8_t *data,
                     uint16_t len, uint8_t record_len) {
    if (st->file_count >= SIM_PROFILE_FILE_CAP) return;
    st->files[st->file_count++] = (sim_file_t){
        parent_df, id, structure, data, len, record_len
    };
}

static void init_stub_profile(sim_state_t *st) {
    static const uint8_t imsi[9] =
        {0x08, 0x10, 0x10, 0x10, 0x32, 0x54, 0x76, 0x98, 0xF0};
    static const uint8_t iccid[10] =
        {0x98, 0x94, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0};
    memcpy(st->imsi, imsi, sizeof imsi);
    memcpy(st->iccid, iccid, sizeof iccid);
    st->lp[0] = 0xFF;
    st->phase_ef[0] = 0x02;
    st->sst[0] = 0x03; st->sst[1] = 0x00;
    st->ad[0] = 0x00; st->ad[1] = 0x00;
    st->ad[2] = 0x00; st->ad[3] = 0x02;
    memset(st->kc, 0xFF, 8); st->kc[8] = 0x07;
    st->hpplmn[0] = 0x00;
    memset(st->plmn, 0xFF, sizeof st->plmn);
    memset(st->loci, 0xFF, 10); st->loci[10] = 0x01;
    memset(st->bcch, 0xFF, sizeof st->bcch);
    st->acc[0] = 0x00; st->acc[1] = 0x01;
    memset(st->fplmn, 0xFF, sizeof st->fplmn);
    /* CPHS phase 1 with the Operator Name String service activated. */
    st->cphs_info[0] = 0x01;
    st->cphs_info[1] = 0x02;
    st->cphs_info[2] = 0x00;
    memset(st->operator_name, 0xFF, sizeof st->operator_name);
    memcpy(st->operator_name, "UA-KYIVSTAR", 11);

    st->file_count = 0;
    add_file(st, 0x0000, 0x3F00, SIM_FILE_DIRECTORY, NULL, 0, 0);
    add_file(st, 0x3F00, 0x7F10, SIM_FILE_DIRECTORY, NULL, 0, 0);
    add_file(st, 0x3F00, 0x7F20, SIM_FILE_DIRECTORY, NULL, 0, 0);
    add_file(st, 0x3F00, 0x7F21, SIM_FILE_DIRECTORY, NULL, 0, 0);
    add_file(st, 0x3F00, 0x2FE2, SIM_FILE_TRANSPARENT, st->iccid, sizeof st->iccid, 0);
    add_file(st, 0x7F20, 0x6F07, SIM_FILE_TRANSPARENT, st->imsi, sizeof st->imsi, 0);
    add_file(st, 0x7F20, 0x6F05, SIM_FILE_TRANSPARENT, st->lp, sizeof st->lp, 0);
    add_file(st, 0x7F20, 0x6FAE, SIM_FILE_TRANSPARENT, st->phase_ef, sizeof st->phase_ef, 0);
    add_file(st, 0x7F20, 0x6F38, SIM_FILE_TRANSPARENT, st->sst, sizeof st->sst, 0);
    add_file(st, 0x7F20, 0x6FAD, SIM_FILE_TRANSPARENT, st->ad, sizeof st->ad, 0);
    add_file(st, 0x7F20, 0x6F20, SIM_FILE_TRANSPARENT, st->kc, sizeof st->kc, 0);
    add_file(st, 0x7F20, 0x6F31, SIM_FILE_TRANSPARENT, st->hpplmn, sizeof st->hpplmn, 0);
    add_file(st, 0x7F20, 0x6F30, SIM_FILE_TRANSPARENT, st->plmn, sizeof st->plmn, 0);
    add_file(st, 0x7F20, 0x6F7E, SIM_FILE_TRANSPARENT, st->loci, sizeof st->loci, 0);
    add_file(st, 0x7F20, 0x6F74, SIM_FILE_TRANSPARENT, st->bcch, sizeof st->bcch, 0);
    add_file(st, 0x7F20, 0x6F78, SIM_FILE_TRANSPARENT, st->acc, sizeof st->acc, 0);
    add_file(st, 0x7F20, 0x6F7B, SIM_FILE_TRANSPARENT, st->fplmn, sizeof st->fplmn, 0);
    add_file(st, 0x7F20, 0x6F16, SIM_FILE_TRANSPARENT,
             st->cphs_info, sizeof st->cphs_info, 0);
    add_file(st, 0x7F20, 0x6F14, SIM_FILE_TRANSPARENT,
             st->operator_name, sizeof st->operator_name, 0);
    st->selected_df = 0x3F00;
    st->selected_ef = 0;
}

static sim_file_t *find_file(sim_state_t *st, uint16_t id) {
    for (int i = 0; i < st->file_count; i++) {
        sim_file_t *f = &st->files[i];
        if (f->id != id) continue;
        if (f->structure == SIM_FILE_DIRECTORY || f->parent_df == st->selected_df)
            return f;
    }
    return NULL;
}

static sim_file_t *selected_file(sim_state_t *st) {
    if (!st->selected_ef) return NULL;
    for (int i = 0; i < st->file_count; i++)
        if (st->files[i].id == st->selected_ef) return &st->files[i];
    return NULL;
}

static sim_file_t *selected_entry(sim_state_t *st) {
    uint16_t id = st->selected_ef ? st->selected_ef : st->selected_df;
    for (int i = 0; i < st->file_count; i++)
        if (st->files[i].id == id) return &st->files[i];
    return NULL;
}

static int build_response(sim_state_t *st, uint8_t *out, int cap) {
    sim_file_t *f = selected_entry(st);
    int n = cap < 15 ? cap : 15;
    memset(out, 0, n);
    if (!f || n < 15) return n;
    out[2] = (uint8_t)(f->len >> 8); out[3] = (uint8_t)f->len;
    out[4] = (uint8_t)(f->id >> 8); out[5] = (uint8_t)f->id;
    if (f->structure == SIM_FILE_DIRECTORY) {
        out[6] = 0x02;
        out[12] = 0x09;
        out[13] = 0x93;
        if (cap >= 20) {
            out[16] = 0x02;
            out[18] = 0x83;
            out[19] = 0x8A;
        }
    } else {
        out[6] = 0x04;
        out[12] = 0x02;
        out[13] = (uint8_t)f->structure;
        out[14] = f->record_len;
    }
    return 15;
}

static int response_length(uint8_t p3) {
    return p3 ? p3 : 256;
}

static void execute_apdu(soc_t *s, sim_state_t *st) {
    uint8_t *a = st->input;
    uint8_t ins = a[1], p1 = a[2], p2 = a[3], p3 = a[4];
    const uint8_t *data = a + 5;
    uint16_t sw = 0x9000;
    uint8_t tmp[256];
    int out_len = 0;
    sim_file_t *f;
    uint16_t df_before = st->selected_df;
    uint16_t ef_before = st->selected_ef;
    switch (ins) {
        case 0xA4: {
            uint16_t id = ((uint16_t)data[0] << 8) | data[1];
            st->response_len = 0;
            f = find_file(st, id);
            if (!f) { sw = 0x9404; break; }
            if (f->structure == SIM_FILE_DIRECTORY) {
                st->selected_df = id;
                st->selected_ef = 0;
            } else {
                st->selected_ef = id;
            }
            st->response_len = (uint8_t)build_response(
                st, st->response, sizeof st->response);
            sw = (uint16_t)(0x9F00u | st->response_len);
            break;
        }
        case 0xC0: {
            int le = response_length(p3);
            out_len = le < st->response_len ? le : st->response_len;
            memcpy(tmp, st->response, out_len);
            break;
        }
        case 0xF2:
            out_len = response_length(p3);
            memset(tmp, 0, out_len);
            (void)build_response(st, tmp, out_len);
            break;
        case 0xB0: {
            int off = ((p1 & 0x7F) << 8) | p2;
            int le = response_length(p3);
            f = selected_file(st);
            if (!f || !f->data || off + le > f->len) { sw = 0x9402; break; }
            out_len = le; memcpy(tmp, f->data + off, out_len); break;
        }
        case 0xD6: {
            int off = ((p1 & 0x7F) << 8) | p2;
            f = selected_file(st);
            if (!f || !f->data || off + p3 > f->len) { sw = 0x9402; break; }
            memcpy(f->data + off, data, p3); break;
        }
        case 0xB2: {
            int le = response_length(p3);
            f = selected_file(st);
            if (!f || !f->data || !f->record_len || !p1 ||
                le > f->record_len || p1 * f->record_len > f->len) {
                sw = 0x9402; break;
            }
            out_len = le;
            memcpy(tmp, f->data + (p1 - 1) * f->record_len, out_len);
            break;
        }
        case 0xDC:
            f = selected_file(st);
            if (!f || !f->data || !f->record_len || !p1 ||
                p3 > f->record_len || p1 * f->record_len > f->len) {
                sw = 0x9402; break;
            }
            memcpy(f->data + (p1 - 1) * f->record_len, data, p3); break;
        case 0x20: sw = 0x9000; break;
        default: sw = 0x6D00; break;
    }
    if (out_len) queue_bytes(st, tmp, out_len, 1);
    st->completion_sw = sw;
    st->completion_pending = 1;
    emit_apdu(s, st, df_before, ef_before, st->input_len, out_len, sw);
    st->input_len = 0; st->input_need = 5;
    st->input_kind = INPUT_APDU_HEADER;
}

static int apdu_has_input(uint8_t ins) {
    return ins == 0xA4 || ins == 0xD6 || ins == 0xDC || ins == 0x20;
}

static void card_input(soc_t *s, sim_state_t *st, uint8_t byte) {
    st->byte_seq++;
    emit_byte(s, st, byte, "phone_to_card", "SIM_TX");
    if (st->input_kind == INPUT_IDLE) {
        st->input_len = 0;
        st->input_kind = byte == 0xFF ? INPUT_PPS : INPUT_APDU_HEADER;
        st->input_need = st->input_kind == INPUT_PPS ? 3 : 5;
    }
    if (st->input_len >= SIM_APDU_CAP) {
        st->input_kind = INPUT_IDLE; st->input_len = 0; return;
    }
    st->input[st->input_len++] = byte;
    if (st->input_kind == INPUT_PPS && st->input_len == 2) {
        uint8_t pps0 = st->input[1];
        st->input_need = (uint16_t)(3 + ((pps0 >> 4) & 1) +
                                    ((pps0 >> 5) & 1) + ((pps0 >> 6) & 1));
    }
    if (st->input_len < st->input_need) return;
    if (st->input_kind == INPUT_PPS) {
        uint8_t x = 0;
        for (int i = 0; i < st->input_len; i++) x ^= st->input[i];
        if (x == 0) queue_bytes(st, st->input, st->input_len, 1);
        st->input_kind = INPUT_IDLE; st->input_len = 0;
        st->due_tick = s->ticks + SIM_BYTE_TICKS; return;
    }
    if (st->input_kind == INPUT_APDU_HEADER) {
        uint8_t ins = st->input[1];
        if (apdu_has_input(ins) && st->input[4]) {
            queue_procedure_byte(st, ins); st->input_kind = INPUT_APDU_DATA;
            st->input_need = (uint16_t)(5 + st->input[4]);
            st->due_tick = s->ticks + SIM_BYTE_TICKS; return;
        }
        queue_procedure_byte(st, ins);
    }
    execute_apdu(s, st);
    st->due_tick = s->ticks + SIM_BYTE_TICKS;
}

static int pec3_count(soc_t *s) { return memory_controller_sfr_get(&s->memory, PECC3) & 0xFF; }
static uint32_t pec3_src(soc_t *s) {
    uint16_t seg = memory_controller_sfr_get(&s->memory, PECSN3);
    return (((uint32_t)(seg & 0xFF)) << 16) | cemu_memory_controller_peek16(&s->memory, SRCP3);
}
static uint32_t pec3_dst(soc_t *s) {
    uint16_t seg = memory_controller_sfr_get(&s->memory, PECSN3);
    return (((uint32_t)(seg >> 8)) << 16) | cemu_memory_controller_peek16(&s->memory, DSTP3);
}

static int pec3_receives_sim(soc_t *s) {
    return pec3_count(s) && (pec3_src(s) & 0xFFFFu) == SIM_RX;
}

static int sim_read8(peripheral_t *self, soc_t *s, uint32_t addr) {
    sim_state_t *st = (sim_state_t *)self->state;
    int value = memory_controller_ram_present(&s->memory, addr) ? memory_controller_ram_get(&s->memory, addr) : 0;
    if (addr == SIM_RX && st->rx_loaded) {
        st->rx_loaded = 0; output_pop(st);
        st->due_tick = s->ticks + SIM_BYTE_TICKS;
    }
    return value;
}

static int sim_peek8(peripheral_t *self, soc_t *s, uint32_t addr) {
    (void)self;
    return memory_controller_ram_present(&s->memory, addr) ? memory_controller_ram_get(&s->memory, addr) : 0;
}

static void begin_activation(sim_state_t *st, soc_t *s, uint16_t before) {
    uint16_t ctrl = ram_word(s, SIM_CTRL);
    st->phase = (ctrl & 1u) ? SIM_PHASE_READY : SIM_PHASE_INITIAL;
    st->due_tick = s->ticks + SIM_BYTE_TICKS;
    st->output_head = st->output_len = 0;
    st->input_len = st->input_need = 0; st->input_kind = INPUT_IDLE;
    st->completion_pending = 0;
    st->rx_loaded = st->initial_raised = 0;
    emit_control(s, before, ctrl, st->phase == SIM_PHASE_INITIAL
                 ? "activate SIM stub"
                 : "enable configured SIM transfer");
}

static int sim_write8(peripheral_t *self, soc_t *s,
                      uint32_t addr, uint8_t value) {
    (void)value;
    sim_state_t *st = (sim_state_t *)self->state;
    if (addr == SIM_CTRL) {
        uint16_t now = ram_word(s, SIM_CTRL), before = st->last_ctrl;
        st->last_ctrl = now;
        if (st->mode == SIM_MODE_STUB && !(before & SIM_CTRL_ACTIVATE) &&
            (now & SIM_CTRL_ACTIVATE)) begin_activation(st, s, before);
        if (!(now & SIM_CTRL_ACTIVATE) && (before & SIM_CTRL_ACTIVATE)) {
            st->phase = SIM_PHASE_OFF; st->output_len = 0;
            st->completion_pending = 0; st->rx_loaded = 0;
            emit_control(s, before, now, "deactivate SIM");
        }
    } else if (addr == SIM_TX && st->mode == SIM_MODE_STUB &&
               st->phase == SIM_PHASE_READY) {
        card_input(s, st, memory_controller_ram_get(&s->memory, SIM_TX));
        st->tx_waiting = 0;
        st->due_tick = s->ticks + SIM_BYTE_TICKS;
    }
    return 1;
}

static void deliver_output(sim_state_t *st, soc_t *s) {
    if (!st->output_len || st->rx_loaded) return;
    int requires_rx = st->output_requires_rx[st->output_head] != 0;
    if (requires_rx && !pec3_receives_sim(s)) return;

    uint8_t byte = st->output[st->output_head];
    memory_controller_ram_set(&s->memory, SIM_RX, byte); st->byte_seq++;
    if (pec3_receives_sim(s)) {
        st->rx_loaded = 1;
        emit_byte(s, st, byte, "card_to_phone", "PEC3 SIM_RX");
        raise_irq(s, SIM_BYTE_IC, "SIM RX byte");
        return;
    }

    output_pop(st);
    emit_byte(s, st, byte, "card_to_phone", "CPU SIM byte ISR");
    raise_irq(s, SIM_BYTE_IC, "SIM byte");
    st->due_tick = s->ticks + SIM_BYTE_TICKS;
}

static void sim_tick(peripheral_t *self, soc_t *s, int n) {
    (void)n;
    sim_state_t *st = (sim_state_t *)self->state;
    if (st->mode != SIM_MODE_STUB || st->phase == SIM_PHASE_OFF) return;
    if (st->phase == SIM_PHASE_INITIAL) {
        if (s->ticks < st->due_tick ||
            (memory_controller_sfr_get(&s->memory, SIM_STATUS_IC) & XIC_IR_BIT)) return;
        put_word(s, SIM_STATUS, SIM_INITIAL_STATUS); put_word(s, SIM_RX, 3);
        raise_irq(s, SIM_STATUS_IC, "initial character/direct convention");
        st->initial_raised = 1; st->phase = SIM_PHASE_ATR;
        st->due_tick = s->ticks + SIM_BYTE_TICKS;
        { const uint8_t atr_tail[] = {0x90, 0x11, 0x00};
          queue_bytes(st, atr_tail, sizeof atr_tail, 0); }
        return;
    }
    if (st->phase == SIM_PHASE_ATR) {
        if (memory_controller_sfr_get(&s->memory, SIM_STATUS_IC) & XIC_IR_BIT) return;
        if (s->ticks >= st->due_tick &&
            !(memory_controller_sfr_get(&s->memory, SIM_BYTE_IC) & XIC_IR_BIT))
            deliver_output(st, s);
        if (!st->output_len && !(memory_controller_sfr_get(&s->memory, SIM_BYTE_IC) & XIC_IR_BIT)) {
            st->phase = SIM_PHASE_READY; st->input_kind = INPUT_IDLE;
        }
        return;
    }
    if (st->output_len && st->output_internal[st->output_head] &&
        s->ticks >= st->due_tick && pec3_count(s) &&
        ((pec3_dst(s) & 0xFFFFu) == SIM_TX || pec3_receives_sim(s))) {
        uint8_t byte = st->output[st->output_head];
        st->byte_seq++;
        emit_byte(s, st, byte, "card_to_phone", "controller T=0 procedure");
        output_pop(st);
        st->due_tick = s->ticks + SIM_BYTE_TICKS;
        return;
    }
    int no_data_error = st->completion_sw != 0x9000 &&
                        (st->completion_sw & 0xFF00u) != 0x9F00u;
    if (st->completion_pending && !st->output_len && !st->rx_loaded &&
        s->ticks >= st->due_tick && (pec3_count(s) == 0 || no_data_error) &&
        !(memory_controller_sfr_get(&s->memory, SIM_BYTE_IC) & XIC_IR_BIT) &&
        !(memory_controller_sfr_get(&s->memory, SIM_STATUS_IC) & XIC_IR_BIT)) {
        st->completion_pending = 0;
        put_word(s, SIM_RESULT_HI, (uint8_t)(st->completion_sw >> 8));
        put_word(s, SIM_RESULT_LO, (uint8_t)st->completion_sw);
        put_word(s, SIM_STATUS, 0x0008);
        raise_irq(s, SIM_STATUS_IC, "SIM T=0 completion");
        return;
    }
    if (st->output_len && s->ticks >= st->due_tick &&
        !(memory_controller_sfr_get(&s->memory, SIM_BYTE_IC) & XIC_IR_BIT))
        deliver_output(st, s);
    if (!st->output_len && s->ticks >= st->due_tick && pec3_count(s) &&
        (pec3_dst(s) & 0xFFFFu) == SIM_TX &&
        !(memory_controller_sfr_get(&s->memory, SIM_BYTE_IC) & XIC_IR_BIT)) {
        st->tx_waiting = 1; raise_irq(s, SIM_BYTE_IC, "SIM TX ready");
    }
}

static uint64_t sim_next_event(peripheral_t *self, soc_t *s) {
    sim_state_t *st = (sim_state_t *)self->state;
    if (st->mode != SIM_MODE_STUB || st->phase == SIM_PHASE_OFF) return UINT64_MAX;
    return st->due_tick <= s->ticks ? 1 : st->due_tick - s->ticks;
}
static void sim_advance_quiet(peripheral_t *self, soc_t *s, uint64_t ticks) {
    (void)self; (void)s; (void)ticks;
}

static int sim_ic_will_fire(peripheral_t *self, soc_t *s, uint32_t ic_addr) {
    sim_state_t *st = (sim_state_t *)self->state;
    (void)ic_addr;
    if (st->mode != SIM_MODE_STUB || st->phase == SIM_PHASE_OFF) return 0;
    if (st->phase == SIM_PHASE_INITIAL || st->phase == SIM_PHASE_ATR) return 1;
    return st->output_len || st->completion_pending ||
           (pec3_count(s) && (pec3_dst(s) & 0xFFFFu) == SIM_TX);
}

void cemu_sim_periph_init(peripheral_t *p, sim_state_t *st) {
    memset(p, 0, sizeof *p); memset(st, 0, sizeof *st); init_stub_profile(st);
    p->id = "sim"; p->state = st;
    p->byte_ranges = SIM_RANGES; p->n_byte_ranges = 1;
    p->ic_nodes = SIM_IC; p->n_ic_nodes = (int)(sizeof SIM_IC / sizeof SIM_IC[0]);
    PERIPHERAL_REG_NAMES(p, SIM_NAMES, (int)(sizeof SIM_NAMES / sizeof SIM_NAMES[0]));
    p->read8 = sim_read8; p->peek8 = sim_peek8; p->write8 = sim_write8;
    p->tick = sim_tick; p->next_event_ticks = sim_next_event;
    p->advance_quiet = sim_advance_quiet; p->ic_will_fire = sim_ic_will_fire;
}

void cemu_sim_set_mode(peripheral_t *p, soc_t *s, sim_mode_t mode) {
    sim_state_t *st = (sim_state_t *)p->state;
    if (st->restored && mode == SIM_MODE_STUB) {
        st->restored = 0;
        st->mode = mode;
        return;
    }
    memset(st, 0, sizeof *st); st->mode = mode; init_stub_profile(st);
    st->last_ctrl = ram_word(s, SIM_CTRL);
    if (mode == SIM_MODE_NONE) put_word(s, SIM_STATUS, 0);
}

sim_mode_t cemu_sim_get_mode(const peripheral_t *p) {
    return ((const sim_state_t *)p->state)->mode;
}
const char *cemu_sim_mode_name(sim_mode_t mode) {
    return mode == SIM_MODE_STUB ? "stub" : "none";
}
