#include "flash.h"

#include <string.h>

#include "soc.h"

int cemu_flash_hex_parse(const char *text, uint8_t *bytes, size_t count) {
    if (!text || !bytes) return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if (strlen(text) != count * 2u) return -1;
    for (size_t i = 0; i < count; i++) {
        char hi_char = text[i * 2u], lo_char = text[i * 2u + 1u];
        int hi = hi_char >= '0' && hi_char <= '9' ? hi_char - '0' :
                 hi_char >= 'a' && hi_char <= 'f' ? hi_char - 'a' + 10 :
                 hi_char >= 'A' && hi_char <= 'F' ? hi_char - 'A' + 10 : -1;
        int lo = lo_char >= '0' && lo_char <= '9' ? lo_char - '0' :
                 lo_char >= 'a' && lo_char <= 'f' ? lo_char - 'a' + 10 :
                 lo_char >= 'A' && lo_char <= 'F' ? lo_char - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return -1;
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

int cemu_flash_has_factory_uid(const flash_state_t *st) {
    return st && (st->kind == FLASH_MODEL_M58LW064D ||
                  st->kind == FLASH_MODEL_W30);
}

int cemu_flash_factory_uid_set(flash_state_t *st, const uint8_t uid[8]) {
    if (!st || !uid) return 0;
    if (st->kind == FLASH_MODEL_M58LW064D)
        cemu_m58lw064d_factory_uid_set(&st->u.m58, uid);
    else if (st->kind == FLASH_MODEL_W30)
        cemu_w30_factory_uid_set(&st->u.w30, uid);
    else return 0;
    return 1;
}

static void emit_operation_event(soc_t *s, peripheral_t *p,
                                 const flash_operation_info_t *operation,
                                 const char *kind, const char *detail) {
#if CEMU_INSTRUMENTED
    if (!s || !operation ||
        !cemu_event_native_trace_active(&s->instrumentation, kind))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.detail = detail;
    if (s->memory.n_flash_chips > 1) {
        for (int i = 0; i < s->memory.n_flash_chips; i++) {
            if (s->memory.flash_endpoints[i] != p) continue;
            cemu_event_field_i64(&ev.info, "chip_index", i);
            cemu_event_field_string(&ev.info, "chip_name", p->id);
            break;
        }
    }
    cemu_event_field_string(&ev.info, "device", p->id);
    cemu_event_field_string(&ev.info, "model",
                   cemu_flash_model_str((const flash_state_t *)p->state));
    cemu_event_field_string(&ev.info, "operation", operation->kind);
    cemu_event_field_i64(&ev.info, "operation_offset", operation->offset);
    if (operation->partition >= 0)
        cemu_event_field_i64(&ev.info, "partition", operation->partition);
    cemu_event_field_i64(&ev.info, "remaining_ticks", operation->remaining_ticks);
    if (((const flash_state_t *)p->state)->kind ==
        FLASH_MODEL_AM29LV)
        cemu_event_field_i64(&ev.info, "sector_offset", operation->offset);
    cemu_event_field_i64(&ev.info, "tick", (long)s->ticks);
    cemu_event_emit_native_trace(&s->instrumentation, &ev,
                               CEMU_EVENT_PERIPHERAL);
#else
    (void)s; (void)p; (void)operation; (void)kind; (void)detail;
#endif
}

static int flash_chip_index(const soc_t *s, const peripheral_t *p) {
    if (!s || !p) return -1;
    for (int i = 0; i < s->memory.n_flash_chips; i++)
        if (s->memory.flash_endpoints[i] == p) return i;
    return -1;
}

void cemu_flash_emit_mutation(soc_t *s, peripheral_t *p,
                              flash_mutation_kind_t kind,
                              uint32_t offset, uint32_t size) {
#if CEMU_INSTRUMENTED
    if (!s || !p || !size) return;
    const char *trace_kind = kind == FLASH_MUTATION_ERASE
                           ? "flash_erase_complete"
                           : "flash_program_complete";
    int typed_active = cemu_event_active(
        &s->instrumentation, CEMU_EVENT_FLASH_MUTATION);
    int trace_active = cemu_event_native_trace_active(
        &s->instrumentation, trace_kind);
    if (!typed_active && !trace_active) return;

    int chip_index = flash_chip_index(s, p);
    const flash_state_t *state = (const flash_state_t *)p->state;
    uint64_t icount = s->cpu ? s->cpu->icount : 0;
    uint32_t pc = s->cpu ? cpu_pc(s->cpu) : 0;
    if (typed_active) {
        cemu_event_t event = {.type = CEMU_EVENT_FLASH_MUTATION};
        event.as.flash_mutation = (cemu_flash_mutation_event_t){
            .kind = kind == FLASH_MUTATION_ERASE
                  ? CEMU_FLASH_MUTATION_ERASE
                  : CEMU_FLASH_MUTATION_PROGRAM,
            .chip_index = chip_index,
            .chip_name = p->id,
            .model = cemu_flash_model_str(state),
            .offset = offset,
            .size = size,
            .tick = s->ticks,
            .icount = icount,
            .pc = pc,
        };
        cemu_event_emit(&s->instrumentation, &event);
    }
    if (trace_active) {
        cemu_native_trace_event_t ev = {
            .kind = trace_kind,
            .icount = icount,
            .pc = pc,
            .has_size = 1,
            .size = (int)size,
            .detail = kind == FLASH_MUTATION_ERASE ? "erase" : "program",
        };
        cemu_event_field_i64(&ev.info, "chip_index", chip_index);
        cemu_event_field_string(&ev.info, "chip_name", p->id);
        cemu_event_field_string(&ev.info, "device", p->id);
        cemu_event_field_string(&ev.info, "model",
                                cemu_flash_model_str(state));
        cemu_event_field_string(&ev.info, "operation",
                                kind == FLASH_MUTATION_ERASE
                                    ? "erase" : "program");
        cemu_event_field_i64(&ev.info, "operation_offset", offset);
        cemu_event_field_i64(&ev.info, "tick", (long)s->ticks);
        cemu_event_emit_native_trace(&s->instrumentation, &ev,
                                     CEMU_EVENT_PERIPHERAL);
    }
#else
    (void)s; (void)p; (void)kind; (void)offset; (void)size;
#endif
}

static void flash_tick(peripheral_t *p, soc_t *s, int n) {
    flash_state_t *st = (flash_state_t *)p->state;
    if (!st) return;
    flash_operation_info_t before;
    int had_operation = cemu_flash_operation_info(st, &before);
    if (st->kind == FLASH_MODEL_AM29LV) {
        uint32_t events = cemu_am29lv_tick(&st->u.am29, (uint32_t)n);
        if (events & AM29_TICK_ERASE_TIMER)
            emit_operation_event(s, p, had_operation ? &before : NULL,
                                 "flash_erase_timer",
                                 "sector erase timer expired");
        if ((events & AM29_TICK_ERASE_COMPLETE) && had_operation)
            cemu_flash_emit_mutation(s, p, FLASH_MUTATION_ERASE,
                                     before.offset, AM29LV_SECTOR_SIZE);
    } else if (st->kind == FLASH_MODEL_W30) {
        uint32_t events = cemu_w30_tick(&st->u.w30, (uint32_t)n);
        const char *kind = events & W30_TICK_ERASE_COMPLETE
                         ? "flash_erase_complete"
                         : events & W30_TICK_PROTECTION_COMPLETE
                         ? "flash_protection_complete"
                         : events & W30_TICK_PROGRAM_COMPLETE
                         ? "flash_program_complete" : NULL;
        if ((events & W30_TICK_PROGRAM_COMPLETE) && had_operation)
            cemu_flash_emit_mutation(s, p, FLASH_MUTATION_PROGRAM,
                                     before.offset, 2);
        else if ((events & W30_TICK_ERASE_COMPLETE) && had_operation) {
            uint32_t start = 0, size = 0;
            if (cemu_w30_block_for_offset(&st->u.w30, before.offset,
                                          &start, &size) >= 0)
                cemu_flash_emit_mutation(s, p, FLASH_MUTATION_ERASE,
                                         start, size);
        } else if (kind)
            emit_operation_event(s, p, had_operation ? &before : NULL,
                                 kind, had_operation ? before.kind : "");
    }
}

static uint64_t flash_next_event(peripheral_t *p, soc_t *s) {
    (void)s;
    flash_state_t *st = (flash_state_t *)p->state;
    if (st && st->kind == FLASH_MODEL_AM29LV)
        return cemu_am29lv_next_event_ticks(&st->u.am29);
    if (st && st->kind == FLASH_MODEL_W30)
        return cemu_w30_next_event_ticks(&st->u.w30);
    return UINT64_MAX;
}

static void flash_advance_quiet(peripheral_t *p, soc_t *s, uint64_t ticks) {
    (void)s;
    flash_state_t *st = (flash_state_t *)p->state;
    if (st && st->kind == FLASH_MODEL_AM29LV)
        cemu_am29lv_advance_quiet(&st->u.am29, ticks);
    else if (st && st->kind == FLASH_MODEL_W30)
        cemu_w30_advance_quiet(&st->u.w30, ticks);
}

int cemu_flash_periph_init(peripheral_t *p, flash_state_t *st, const char *model) {
    if (!p || !st || !model) return 0;
    memset(st, 0, sizeof(*st));
    if (!strcmp(model, "m58lw064d")) {
        st->kind = FLASH_MODEL_M58LW064D;
        cemu_m58lw064d_periph_init(p, &st->u.m58);
        p->state = st;
        return 1;
    }
    if (!strcmp(model, AM29LV128MH_MODEL)) {
        st->kind = FLASH_MODEL_AM29LV;
        cemu_am29lv128mh_periph_init(p, &st->u.am29);
        p->state = st;
        p->tick = flash_tick;
        p->next_event_ticks = flash_next_event;
        p->advance_quiet = flash_advance_quiet;
        return 1;
    }
    if (!strcmp(model, AM29LV640MH_MODEL)) {
        st->kind = FLASH_MODEL_AM29LV;
        cemu_am29lv640mh_periph_init(p, &st->u.am29);
        p->state = st;
        p->tick = flash_tick;
        p->next_event_ticks = flash_next_event;
        p->advance_quiet = flash_advance_quiet;
        return 1;
    }
    if (!strcmp(model, W30_64MBIT_TOP_MODEL)) {
        st->kind = FLASH_MODEL_W30;
        cemu_w30_64mbit_top_periph_init(p, &st->u.w30);
        p->state = st;
        p->tick = flash_tick;
        p->next_event_ticks = flash_next_event;
        p->advance_quiet = flash_advance_quiet;
        return 1;
    }
    if (!strcmp(model, W30_128MBIT_TOP_MODEL)) {
        st->kind = FLASH_MODEL_W30;
        cemu_w30_128mbit_top_periph_init(p, &st->u.w30);
        p->state = st;
        p->tick = flash_tick;
        p->next_event_ticks = flash_next_event;
        p->advance_quiet = flash_advance_quiet;
        return 1;
    }
    memset(p, 0, sizeof(*p));
    return 0;
}

void cemu_flash_state_free(flash_state_t *st) {
    if (!st) return;
    if (st->kind == FLASH_MODEL_M58LW064D)
        cemu_m58lw064d_state_free(&st->u.m58);
    else if (st->kind == FLASH_MODEL_AM29LV)
        cemu_am29lv_state_free(&st->u.am29);
    else if (st->kind == FLASH_MODEL_W30)
        cemu_w30_state_free(&st->u.w30);
    memset(st, 0, sizeof(*st));
}

int cemu_flash_state_copy(flash_state_t *dst, const flash_state_t *src) {
    if (!dst || !src) return 0;
    if (dst->kind && dst->kind != src->kind) cemu_flash_state_free(dst);
    if (src->kind == FLASH_MODEL_M58LW064D) {
        if (!cemu_m58lw064d_state_copy(&dst->u.m58, &src->u.m58)) return 0;
        dst->kind = src->kind;
        return 1;
    }
    if (src->kind == FLASH_MODEL_AM29LV) {
        if (!cemu_am29lv_state_copy(&dst->u.am29, &src->u.am29)) return 0;
        dst->kind = src->kind;
        return 1;
    }
    if (src->kind == FLASH_MODEL_W30) {
        if (!cemu_w30_state_copy(&dst->u.w30, &src->u.w30)) return 0;
        dst->kind = src->kind;
        return 1;
    }
    return 0;
}

int cemu_flash_seed_bytes(const uint8_t *data, size_t len, flash_state_t *st,
                     uint32_t off, const uint8_t *bytes, size_t count) {
    if (!st) return 0;
    if (st->kind == FLASH_MODEL_M58LW064D)
        return cemu_m58lw064d_seed_bytes(data, len, &st->u.m58,
                                    off, bytes, count);
    if (st->kind == FLASH_MODEL_AM29LV)
        return cemu_am29lv_seed_bytes(data, len, &st->u.am29,
                                      off, bytes, count);
    if (st->kind == FLASH_MODEL_W30)
        return cemu_w30_seed_bytes(data, len, &st->u.w30,
                                        off, bytes, count);
    return 0;
}

uint8_t cemu_flash_read8(const uint8_t *data, size_t len, flash_state_t *st,
                    uint32_t off, int command_visible, flash_access_t *access) {
    if (st && st->kind == FLASH_MODEL_M58LW064D) {
        return cemu_m58lw064d_read8(data, len, &st->u.m58, off,
                               command_visible, access);
    }
    if (st && st->kind == FLASH_MODEL_AM29LV) {
        return cemu_am29lv_read8(data, len, &st->u.am29, off,
                                 command_visible, access);
    }
    if (st && st->kind == FLASH_MODEL_W30) {
        w30_access_t concrete;
        uint8_t value = cemu_w30_read8(
            data, len, &st->u.w30, off, command_visible,
            access ? &concrete : NULL);
        if (access) *access = concrete;
        return value;
    }
    if (access) {
        access->subtype = NULL;
        access->detail = "flash";
        access->include_mode = 0;
        access->mutation_count = 0;
    }
    return 0xFF;
}

uint8_t cemu_flash_array_read8(const uint8_t *data, size_t len,
                          const flash_state_t *st, uint32_t off) {
    if (st && st->kind == FLASH_MODEL_M58LW064D)
        return cemu_m58lw064d_array_read8(data, len, &st->u.m58, off);
    if (st && st->kind == FLASH_MODEL_AM29LV)
        return cemu_am29lv_array_read8(data, len, &st->u.am29, off);
    if (st && st->kind == FLASH_MODEL_W30)
        return cemu_w30_array_read8(data, len, &st->u.w30, off);
    return 0xFF;
}

uint8_t cemu_flash_peek8(const uint8_t *data, size_t len, const flash_state_t *st,
                    uint32_t off, int command_visible) {
    if (st && st->kind == FLASH_MODEL_AM29LV)
        return cemu_am29lv_peek8(data, len, &st->u.am29, off,
                                 command_visible);
    if (st && st->kind == FLASH_MODEL_M58LW064D) {
        m58lw064d_state_t copy = st->u.m58;
        return cemu_m58lw064d_read8(data, len, &copy, off, command_visible, NULL);
    }
    if (st && st->kind == FLASH_MODEL_W30)
        return cemu_w30_peek8(data, len, &st->u.w30, off,
                                   command_visible);
    return 0xFF;
}

void cemu_flash_write8(const uint8_t *data, size_t len, flash_state_t *st,
                  uint32_t off, uint8_t value, int command_visible,
                  flash_access_t *access) {
    if (st && st->kind == FLASH_MODEL_M58LW064D) {
        cemu_m58lw064d_write8(data, len, &st->u.m58, off, value,
                         command_visible, access);
    } else if (st && st->kind == FLASH_MODEL_AM29LV) {
        cemu_am29lv_write8(data, len, &st->u.am29, off, value,
                           command_visible, access);
    } else if (st && st->kind == FLASH_MODEL_W30) {
        w30_access_t concrete;
        cemu_w30_write8(data, len, &st->u.w30, off, value,
                             command_visible, access ? &concrete : NULL);
        if (access) *access = concrete;
    }
}

void cemu_flash_write16(const uint8_t *data, size_t len, flash_state_t *st,
                   uint32_t off, uint16_t value, int command_visible,
                   flash_access_t *access) {
    if (st && st->kind == FLASH_MODEL_M58LW064D) {
        cemu_m58lw064d_write16(data, len, &st->u.m58, off, value,
                          command_visible, access);
    } else if (st && st->kind == FLASH_MODEL_AM29LV) {
        cemu_am29lv_write16(data, len, &st->u.am29, off, value,
                            command_visible, access);
    } else if (st && st->kind == FLASH_MODEL_W30) {
        w30_access_t concrete;
        cemu_w30_write16(data, len, &st->u.w30, off, value,
                              command_visible, access ? &concrete : NULL);
        if (access) *access = concrete;
    }
}

const char *cemu_flash_mode_str(const flash_state_t *st) {
    if (st && st->kind == FLASH_MODEL_M58LW064D)
        return cemu_m58lw064d_mode_str(&st->u.m58);
    if (st && st->kind == FLASH_MODEL_AM29LV)
        return cemu_am29lv_mode_str(&st->u.am29);
    if (st && st->kind == FLASH_MODEL_W30)
        return cemu_w30_mode_str(&st->u.w30);
    return "invalid";
}

const char *cemu_flash_model_str(const flash_state_t *st) {
    if (st && st->kind == FLASH_MODEL_M58LW064D) return "m58lw064d";
    if (st && st->kind == FLASH_MODEL_AM29LV)
        return cemu_am29lv_model_str(&st->u.am29);
    if (st && st->kind == FLASH_MODEL_W30)
        return cemu_w30_model_str(&st->u.w30);
    return "invalid";
}

m58lw064d_state_t *cemu_flash_m58_state(flash_state_t *st) {
    return st && st->kind == FLASH_MODEL_M58LW064D ? &st->u.m58 : NULL;
}

const m58lw064d_state_t *cemu_flash_m58_state_const(const flash_state_t *st) {
    return st && st->kind == FLASH_MODEL_M58LW064D ? &st->u.m58 : NULL;
}

am29lv_state_t *cemu_flash_am29_state(flash_state_t *st) {
    return st && st->kind == FLASH_MODEL_AM29LV ? &st->u.am29 : NULL;
}

const am29lv_state_t *cemu_flash_am29_state_const(const flash_state_t *st) {
    return st && st->kind == FLASH_MODEL_AM29LV ? &st->u.am29 : NULL;
}

w30_state_t *cemu_flash_w30_state(flash_state_t *st) {
    return st && st->kind == FLASH_MODEL_W30 ? &st->u.w30 : NULL;
}

const w30_state_t *cemu_flash_w30_state_const(const flash_state_t *st) {
    return st && st->kind == FLASH_MODEL_W30 ? &st->u.w30 : NULL;
}

int cemu_flash_operation_info(const flash_state_t *st, flash_operation_info_t *info) {
    if (!st || !info) return 0;
    memset(info, 0, sizeof(*info));
    info->partition = -1;
    if (st->kind == FLASH_MODEL_AM29LV && st->u.am29.erase_active) {
        info->kind = "erase";
        info->offset = st->u.am29.erase_sector;
        info->remaining_ticks = st->u.am29.erase_ticks_remaining;
        info->suspended = st->u.am29.erase_suspended;
        return 1;
    }
    if (st->kind == FLASH_MODEL_W30) {
        const w30_operation_t *op = st->u.w30.active.kind != W30_OP_NONE
                                  ? &st->u.w30.active
                                  : st->u.w30.suspended_program.kind !=
                                        W30_OP_NONE
                                  ? &st->u.w30.suspended_program
                                  : &st->u.w30.suspended_erase;
        if (op->kind == W30_OP_NONE) return 0;
        info->kind = cemu_w30_operation_str(op->kind);
        info->offset = op->off;
        info->remaining_ticks = op->ticks_remaining;
        info->partition = op->partition;
        info->suspended = st->u.w30.active.kind == W30_OP_NONE;
        return 1;
    }
    return 0;
}

void cemu_flash_configure_am29(flash_state_t *st, uint32_t protected_start,
                          uint32_t protected_size, int secsi_factory_locked) {
    am29lv_state_t *am29 = cemu_flash_am29_state(st);
    if (am29)
        cemu_am29lv_configure(am29, protected_start, protected_size,
                              secsi_factory_locked);
}

void cemu_flash_reset(flash_state_t *st) {
    if (!st) return;
    if (st->kind == FLASH_MODEL_AM29LV)
        cemu_am29lv_reset(&st->u.am29);
    else if (st->kind == FLASH_MODEL_W30)
        cemu_w30_reset(&st->u.w30);
    else if (st->kind == FLASH_MODEL_M58LW064D) {
        st->u.m58.read_mode = FLASH_ARRAY;
        st->u.m58.command_phase = FLASH_PHASE_IDLE;
        st->u.m58.status = 0x80;
        st->u.m58.write_buffer_state = FLASH_WB_IDLE;
        st->u.m58.write_buffer_words_left = 0;
        st->u.m58.write_buffer_block = 0;
        st->u.m58.write_buffer_page = UINT32_MAX;
        st->u.m58.write_buffer_len = 0;
    }
}
