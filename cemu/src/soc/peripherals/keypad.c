/* Evidence-bounded x55 startup-key scanner and per-device matrix; see keypad.h. */
#include <string.h>
#include "keypad.h"
#include "soc.h"
#include "synth.h"

static void emit_command(soc_t *s, const keypad_state_t *st, uint32_t addr,
                         uint16_t command, int startup, int matrix,
                         int handled, const char *detail) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "keypad_command")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "keypad_command";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = addr;
    ev.has_size = 1; ev.size = 2;
    ev.has_value = 1; ev.value = command;
    ev.detail = detail;
    cemu_event_field_bool(&ev.info, "handled", handled);
    cemu_event_field_bool(&ev.info, "startup", startup);
    cemu_event_field_bool(&ev.info, "matrix", matrix);
    if (matrix)
        cemu_event_field_i64(&ev.info, "column", (uint16_t)(~command & 0x00FDu));
    cemu_event_field_bool(&ev.info, "startup_power_pending",
                          st->startup_power_pending);
    cemu_event_field_i64(&ev.info, "button_count", st->nbuttons);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void emit_result(soc_t *s, uint16_t command, uint16_t result,
                        const char *detail) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "keypad_result")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "keypad_result";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = KEYPAD_SCAN_RESULT;
    ev.has_size = 1; ev.size = 2;
    ev.has_value = 1; ev.value = result;
    ev.detail = detail;
    cemu_event_field_i64(&ev.info, "command", command);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void emit_startup_release(soc_t *s, const keypad_state_t *st) {
    if (!cemu_event_native_trace_active(
            &s->instrumentation, "keypad_startup_release")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "keypad_startup_release";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = KEYPAD_SCAN_RESULT;
    ev.has_size = 1; ev.size = 2;
    ev.has_value = 1; ev.value = st->last_result;
    ev.detail = "keypad startup power result consumed";
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void emit_input(soc_t *s, const keypad_state_t *st,
                       const keypad_button_t *button, int pressed,
                       int activity_irq_requested) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "keypad_input")) return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = "keypad_input";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_value = 1; ev.value = button->raw_code;
    ev.detail = "keypad button input";
    cemu_event_field_string(&ev.info, "button", button->name);
    cemu_event_field_i64(&ev.info, "logical_code", button->logical_code);
    cemu_event_field_bool(&ev.info, "pressed", pressed);
    cemu_event_field_bool(&ev.info, "activity_irq_requested",
                    activity_irq_requested);
    if (st->activity_ic_addr)
        cemu_event_field_i64(&ev.info, "activity_ic_addr", st->activity_ic_addr);
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void put_result(soc_t *s, keypad_state_t *st, uint16_t result,
                       int startup, const char *detail) {
    memory_controller_ram_set(&s->memory, KEYPAD_SCAN_RESULT, (uint8_t)result);
    memory_controller_ram_set(&s->memory, KEYPAD_SCAN_RESULT + 1u, (uint8_t)(result >> 8));
    st->results++;
    st->last_result = result;
    st->startup_result_tagged = !!startup;
    st->startup_result_read_mask = 0;
    emit_result(s, st->last_command, result, detail);
}

static uint16_t matrix_result(keypad_state_t *st, uint16_t command) {
    uint16_t result = st->matrix_idle_result;
    for (size_t i = 0; i < st->nbuttons; i++) {
        if (!(st->pressed & (1u << i))) continue;
        uint16_t raw = st->buttons[i].raw_code;
        if (raw == 0x000D) {
            if (command == st->scan_command) {
                st->sampled_pressed |= 1u << i;
                result = (uint16_t)((result & 0xFFF0u) |
                                    ((result & 0x000Fu) & 0x000Du));
            }
            continue;
        }
        uint16_t column = raw >> 4;
        uint16_t column_command = (uint16_t)(~column & 0x00FDu);
        uint16_t selected_column = (uint16_t)(~command & 0x00FFu);
        if (selected_column == column ||
            (command & 0x00FFu) == column_command) {
            st->sampled_pressed |= 1u << i;
            result = (uint16_t)((result & 0xFFF0u) |
                                ((result & 0x000Fu) & (raw & 0x000Fu)));
        }
    }
    return result;
}

static uint16_t matrix_column_result(keypad_state_t *st,
                                     uint16_t column) {
    uint16_t result = st->matrix_idle_result;
    for (size_t i = 0; i < st->nbuttons; i++) {
        if (!(st->pressed & (1u << i))) continue;
        uint16_t raw = st->buttons[i].raw_code;
        if ((raw >> 4) == column) {
            st->sampled_pressed |= 1u << i;
            result = (uint16_t)((result & 0xFFF0u) |
                                ((result & 0x000Fu) & (raw & 0x000Fu)));
        }
    }
    return result;
}

static uint16_t matrix_all_result(keypad_state_t *st, int *release_activity) {
    *release_activity = 0;
    int matrix_pressed = 0;
    int direct_pressed = 0;
    uint16_t direct_result = st->matrix_idle_result;
    for (size_t i = 0; i < st->nbuttons; i++) {
        if (!(st->pressed & (1u << i))) continue;
        uint16_t raw = st->buttons[i].raw_code;
        if ((raw >> 4) == 0) {
            st->sampled_pressed |= 1u << i;
            direct_pressed = 1;
            direct_result = (uint16_t)((direct_result & 0xFFF0u) |
                                      ((direct_result & 0x000Fu) &
                                       (raw & 0x000Fu)));
        } else {
            matrix_pressed = 1;
        }
    }
    if (direct_pressed) return direct_result;
    if (matrix_pressed) return st->matrix_idle_result;
    if (st->release_activity_pending) {
        st->release_activity_pending = 0;
        *release_activity = 1;
        return st->matrix_idle_result;
    }
    return st->idle_result;
}

static int is_matrix_command(const keypad_state_t *st, uint16_t command) {
    for (size_t i = 0; i < st->n_matrix_commands; i++)
        if ((command & 0x00FFu) == st->matrix_commands[i]) return 1;
    return 0;
}

static int keypad_write8(peripheral_t *self, soc_t *s,
                         uint32_t addr, uint8_t value) {
    (void)value;
    keypad_state_t *st = (keypad_state_t *)self->state;
    if (addr != KEYPAD_SCAN_COMMAND) return 0;

    uint16_t command = memory_controller_ram_get(&s->memory, KEYPAD_SCAN_COMMAND) |
                       ((uint16_t)memory_controller_ram_get(&s->memory, KEYPAD_SCAN_COMMAND + 1u) << 8);
    int suppressed = (s->synth_mask >> SYN_SUPPRESS_EF_MAILBOX_RESPONSE) & 1u;
    int startup_command = command == st->scan_command;
    int startup_result = startup_command && st->startup_power_pending;
    int matrix_command = is_matrix_command(st, command);
    int handled = (startup_command || matrix_command) && !suppressed;
    st->scans++;
    st->last_command = command;
    emit_command(s, st, KEYPAD_SCAN_COMMAND, command, startup_command,
                 matrix_command, handled, "keypad scan command");

    if (command == 0 && !startup_command) {
        put_result(s, st, 0, 0, "keypad command cleanup");
        return 1;
    }
    if (!handled) return 0;

    int release_activity = 0;
    uint16_t result = st->matrix_select_addr && command == st->scan_command
                    ? matrix_all_result(st, &release_activity)
                    : matrix_result(st, command);
    const char *detail = "keypad matrix scan";
    if (startup_result) {
        result = cemu_keypad_startup_power_result(st);
        detail = "keypad startup power key";
    } else if (command == st->scan_command) {
        detail = result == st->idle_result ? "keypad idle scan" :
                 result == st->matrix_idle_result ? "keypad matrix probe" :
                                                    "keypad direct key";
    }
    st->handled_scans++;
    put_result(s, st, result, startup_result, detail);
    return 1;
}

static int keypad_read8(peripheral_t *self, soc_t *s, uint32_t addr) {
    keypad_state_t *st = (keypad_state_t *)self->state;
    if (addr == KEYPAD_SCAN_RESULT && st->matrix_scan_phase == 1) {
        uint16_t value = memory_controller_ram_get(&s->memory, addr) |
                         ((uint16_t)memory_controller_ram_get(&s->memory, addr + 1u) << 8);
        if ((value & 0x000Fu) == 0x000Fu && value != st->idle_result)
            st->matrix_scan_phase = 2;
    }
    int value = memory_controller_ram_get(&s->memory, addr);
    if (st->startup_result_tagged &&
        (addr == KEYPAD_SCAN_RESULT || addr == KEYPAD_SCAN_RESULT + 1u)) {
        st->startup_result_read_mask |=
            (uint8_t)(1u << (addr - KEYPAD_SCAN_RESULT));
        if (st->startup_result_read_mask == 3u &&
            st->startup_power_pending) {
            st->startup_power_pending = 0;
            st->startup_result_tagged = 0;
            st->startup_releases++;
            emit_startup_release(s, st);
        }
    }
    return value;
}

static void keypad_sfr_write(peripheral_t *self, soc_t *s,
                             uint32_t word_addr, uint16_t stored) {
    keypad_state_t *st = (keypad_state_t *)self->state;
    if (!st->matrix_select_addr || word_addr != st->matrix_select_addr) return;
    uint16_t column = stored & st->matrix_select_mask;
    if (st->startup_power_pending) {
        if (column) return;
        int suppressed =
            (s->synth_mask >> SYN_SUPPRESS_EF_MAILBOX_RESPONSE) & 1u;
        st->scans++;
        st->last_command = stored;
        emit_command(s, st, word_addr, stored, 1, 1, !suppressed,
                     "keypad startup matrix select");
        if (!suppressed) {
            st->handled_scans++;
            put_result(s, st, cemu_keypad_startup_power_result(st), 1,
                       "keypad startup power key");
        }
        return;
    }
    uint16_t command = (uint16_t)(~column & 0x00FDu);
    int selected = column && !(column & (column - 1u));
    int scan_activity = column != st->matrix_select_mask;
    st->scans++;
    st->last_command = command;
    emit_command(s, st, word_addr, command, 0, scan_activity, scan_activity,
                 "keypad matrix column select");
    if (column == st->matrix_select_mask) {
        st->matrix_scan_phase = 0;
        put_result(s, st, st->idle_result, 0, "keypad matrix idle");
        return;
    }
    if (st->matrix_scan_phase == 0 && !column) {
        int release_activity = 0;
        st->matrix_scan_phase = 1;
        uint16_t result = matrix_all_result(st, &release_activity);
        put_result(s, st, result, 0, release_activity
                   ? "keypad matrix release activity"
                   : "keypad matrix probe");
        return;
    }
    if (st->matrix_scan_phase == 2 && !column) {
        put_result(s, st, st->matrix_idle_result, 0,
                   "keypad matrix column gap");
        return;
    }
    if (!selected) {
        put_result(s, st, st->matrix_idle_result, 0, "keypad matrix idle");
        return;
    }
    st->handled_scans++;
    put_result(s, st, matrix_column_result(st, column), 0,
               "keypad matrix scan");
}

const keypad_button_t *cemu_keypad_buttons(const keypad_state_t *st, size_t *count) {
    if (count) *count = st ? st->nbuttons : 0;
    return st ? st->buttons : NULL;
}

const keypad_button_t *cemu_keypad_button_by_name(const keypad_state_t *st,
                                              const char *name) {
    if (!st || !name) return NULL;
    if (!strcmp(name, "*")) name = "star";
    else if (!strcmp(name, "#")) name = "hash";
    else if (!strcmp(name, "call")) name = "send";
    else if (!strcmp(name, "end")) name = "power";
    for (size_t i = 0; i < st->nbuttons; i++)
        if (!strcmp(name, st->buttons[i].name)) return &st->buttons[i];
    return NULL;
}

int cemu_keypad_set_button(keypad_state_t *st, soc_t *s, const char *name,
                      int pressed) {
    const keypad_button_t *button = cemu_keypad_button_by_name(st, name);
    if (!st || !s || !button) return -1;
    size_t index = (size_t)(button - st->buttons);
    uint32_t bit = 1u << index;
    uint32_t pressed_before = st->pressed;
    int before = !!(pressed_before & bit);
    if (pressed) st->pressed |= bit;
    else st->pressed &= ~bit;
    if (before != !!pressed) st->sampled_pressed &= ~bit;
    if (before != !!pressed) {
        if (pressed) {
            st->release_activity_pending = 0;
        } else if (st->release_activity_on_last_key_up &&
                   pressed_before && !st->pressed) {
            st->release_activity_pending = 1;
        }
        int activity_irq_requested = st->activity_ic_addr != 0;
        if (activity_irq_requested)
            cemu_memory_controller_sfr_put(&s->memory, st->activity_ic_addr,
                        memory_controller_sfr_get(&s->memory, st->activity_ic_addr) | XIC_IR_BIT);
        emit_input(s, st, button, pressed, activity_irq_requested);
    }
    return before != !!pressed;
}

uint32_t cemu_keypad_sampled_buttons(const keypad_state_t *st) {
    return st ? st->sampled_pressed : 0;
}

uint16_t cemu_keypad_startup_power_result(const keypad_state_t *st) {
    const keypad_button_t *power = cemu_keypad_button_by_name(st, "power");
    return power ? power->raw_code : 0;
}

void cemu_keypad_capture_mutable(const keypad_state_t *st,
                                 keypad_mutable_state_t *out) {
    if (!st || !out) return;
    *out = (keypad_mutable_state_t){
        .scans = st->scans,
        .handled_scans = st->handled_scans,
        .results = st->results,
        .startup_releases = st->startup_releases,
        .pressed = st->pressed,
        .sampled_pressed = st->sampled_pressed,
        .last_command = st->last_command,
        .last_result = st->last_result,
        .startup_power_pending = st->startup_power_pending,
        .startup_result_tagged = st->startup_result_tagged,
        .startup_result_read_mask = st->startup_result_read_mask,
        .matrix_scan_phase = st->matrix_scan_phase,
        .release_activity_pending = st->release_activity_pending,
    };
}

int cemu_keypad_restore_mutable(keypad_state_t *st,
                                const keypad_mutable_state_t *saved) {
    if (!st || !saved || saved->startup_power_pending > 1 ||
        saved->startup_result_tagged > 1 ||
        saved->startup_result_read_mask > 3 ||
        saved->matrix_scan_phase > 2 ||
        saved->release_activity_pending > 1 ||
        (saved->startup_result_tagged && !saved->startup_power_pending) ||
        (st->nbuttons < 32 &&
         ((saved->pressed | saved->sampled_pressed) >> st->nbuttons)) ||
        (saved->sampled_pressed & ~saved->pressed))
        return 0;
    st->scans = saved->scans;
    st->handled_scans = saved->handled_scans;
    st->results = saved->results;
    st->startup_releases = saved->startup_releases;
    st->pressed = saved->pressed;
    st->sampled_pressed = saved->sampled_pressed;
    st->last_command = saved->last_command;
    st->last_result = saved->last_result;
    st->startup_power_pending = saved->startup_power_pending;
    st->startup_result_tagged = saved->startup_result_tagged;
    st->startup_result_read_mask = saved->startup_result_read_mask;
    st->matrix_scan_phase = saved->matrix_scan_phase;
    st->release_activity_pending = saved->release_activity_pending;
    return 1;
}

void cemu_keypad_restore_legacy(keypad_state_t *st) {
    if (!st) return;
    keypad_mutable_state_t released = {0};
    (void)cemu_keypad_restore_mutable(st, &released);
}

void cemu_keypad_periph_init(peripheral_t *p, keypad_state_t *st,
                        const device_keypad_config_t *cfg,
                        uint32_t activity_ic_addr) {
    memset(p, 0, sizeof *p);
    memset(st, 0, sizeof *st);
    st->scan_command = cfg->scan_command;
    st->idle_result = cfg->idle_result;
    st->matrix_idle_result = cfg->matrix_idle_result;
    st->release_activity_on_last_key_up =
        !!cfg->release_activity_on_last_key_up;
    st->buttons = cfg->buttons;
    st->nbuttons = cfg->nbuttons;
    st->n_matrix_commands = cfg->n_matrix_commands;
    memcpy(st->matrix_commands, cfg->matrix_commands,
           sizeof st->matrix_commands);
    st->matrix_select_addr = cfg->matrix_select_addr;
    st->matrix_select_mask = cfg->matrix_select_mask;
    st->activity_ic_addr = activity_ic_addr;
    st->startup_power_pending =
        cemu_keypad_button_by_name(st, "power") != NULL;
    if (st->matrix_select_addr)
        st->sfr_words[st->n_sfr_words++] = st->matrix_select_addr;
    st->ranges[0] = (addr_range_t){KEYPAD_SCAN_RESULT, KEYPAD_SCAN_COMMAND + 1u};
    st->reg_names[0] = (reg_name_t){KEYPAD_SCAN_RESULT, "KEYPAD_SCAN_RESULT"};
    st->reg_names[1] = (reg_name_t){KEYPAD_SCAN_COMMAND, "KEYPAD_SCAN_COMMAND"};
    p->id = "keypad";
    p->state = st;
    p->byte_ranges = st->ranges;
    p->n_byte_ranges = 1;
    p->sfr_words = st->sfr_words;
    p->n_sfr_words = (int)st->n_sfr_words;
    PERIPHERAL_REG_NAMES(p, st->reg_names, 2);
    p->write8 = keypad_write8;
    p->read8 = keypad_read8;
    p->on_sfr_write = keypad_sfr_write;
}
