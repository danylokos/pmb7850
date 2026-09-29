/* C166S CPU core — C port of emu/src/cpu/c166.py. See cpu.h for the state split.
 *
 * This is a deliberately faithful transcription: the opcode dispatch mirrors the
 * Python `_exec` if/elif ladder as a switch, and the six PSW flag helpers keep
 * the exact rules the Python (and the M166 manual) pin — C=borrow for SUB/CMP,
 * sticky-Z for ADDC/SUBC, previous-then-current C/V ordering in the shift loops.
 * Where the Python relied on unbounded ints + masks, we use uint32_t and mask
 * explicitly at the same points. */
#include <string.h>
#include "cpu.h"

/* ---- bus shorthands ---------------------------------------------------- */
static inline uint8_t  rd8(cpu_t *c, uint32_t a)             { return bus_read8(c->bus, a); }
static inline void     wr8(cpu_t *c, uint32_t a, uint8_t v)  { bus_write8(c->bus, a, v); }
static inline uint16_t rd16(cpu_t *c, uint32_t a)            { return bus_read16(c->bus, a); }
static inline void     wr16(cpu_t *c, uint32_t a, uint16_t v){ bus_write16(c->bus, a, v); }

/* ---- register-bank access (GPRs memory-mapped at CP + 2n) -------------- */
static inline uint32_t cp_base(cpu_t *c) { return rd16(c, 0xFE10); }

uint16_t cemu_cpu_gpr(cpu_t *c, int n)              { return rd16(c, cp_base(c) + 2u * (n & 0xF)); }
void     cemu_cpu_set_gpr(cpu_t *c, int n, uint16_t v){ wr16(c, cp_base(c) + 2u * (n & 0xF), v); }

static uint8_t gpr_b(cpu_t *c, int n) {
    return rd8(c, cp_base(c) + (n & 0xF));
}
static void set_gpr_b(cpu_t *c, int n, uint8_t val) {
    wr8(c, cp_base(c) + (n & 0xF), val);
}

/* ---- PSW / flags ------------------------------------------------------- */
uint16_t cemu_cpu_psw(cpu_t *c)              { return rd16(c, 0xFF10); }
void     cemu_cpu_set_psw(cpu_t *c, uint16_t v){ wr16(c, 0xFF10, v); }

static void set_flags_logic(cpu_t *c, uint32_t res, int width, int has_op2, uint32_t op2) {
    uint32_t mask = (width == 8) ? 0xFF : 0xFFFF;
    uint32_t sign = (width == 8) ? 0x80 : 0x8000;
    res &= mask;
    uint16_t p = cemu_cpu_psw(c) & ~(F_N | F_Z | F_C | F_V | F_E);
    if (res == 0)          p |= F_Z;
    if (res & sign)        p |= F_N;
    if (has_op2 && (op2 & mask) == sign) p |= F_E;
    cemu_cpu_set_psw(c, p);
}

static uint16_t set_flags_add(cpu_t *c, uint32_t a, uint32_t b, int width,
                              int carry_in, int sticky) {
    uint32_t mask = (width == 8) ? 0xFF : 0xFFFF;
    uint32_t sign = (width == 8) ? 0x80 : 0x8000;
    uint32_t full = (a & mask) + (b & mask) + (uint32_t)carry_in;
    uint32_t r = full & mask;
    int prev_z = (cemu_cpu_psw(c) & F_Z) != 0;
    uint16_t p = cemu_cpu_psw(c) & ~(F_N | F_Z | F_C | F_V | F_E);
    if (r == 0 && (!sticky || prev_z)) p |= F_Z;
    if (r & sign)                      p |= F_N;
    if (full & (mask + 1))             p |= F_C;
    if ((b & mask) == sign)            p |= F_E;
    uint32_t sa = a & sign, sb = b & sign, sr = r & sign;
    if (sa == sb && sr != sa)          p |= F_V;
    cemu_cpu_set_psw(c, p);
    return (uint16_t)r;
}

static uint16_t set_flags_sub(cpu_t *c, uint32_t a, uint32_t b, int width,
                              int borrow_in, int sticky) {
    uint32_t mask = (width == 8) ? 0xFF : 0xFFFF;
    uint32_t sign = (width == 8) ? 0x80 : 0x8000;
    int32_t full = (int32_t)(a & mask) - (int32_t)(b & mask) - borrow_in;
    uint32_t r = (uint32_t)full & mask;
    int prev_z = (cemu_cpu_psw(c) & F_Z) != 0;
    uint16_t p = cemu_cpu_psw(c) & ~(F_N | F_Z | F_C | F_V | F_E);
    if (r == 0 && (!sticky || prev_z)) p |= F_Z;
    if (r & sign)                      p |= F_N;
    if (full < 0)                      p |= F_C;   /* C = borrow for SUB/CMP */
    if ((b & mask) == sign)            p |= F_E;
    uint32_t sa = a & sign, sb = b & sign, sr = r & sign;
    if (sa != sb && sr != sa)          p |= F_V;
    cemu_cpu_set_psw(c, p);
    return (uint16_t)r;
}

static void mov_flags(cpu_t *c, uint32_t val, int width) {
    uint32_t mask = (width == 8) ? 0xFF : 0xFFFF;
    uint32_t sign = (width == 8) ? 0x80 : 0x8000;
    uint32_t v = val & mask;
    uint16_t p = cemu_cpu_psw(c) & ~(F_E | F_Z | F_N);   /* leave V, C */
    if (v == 0)    p |= F_Z;
    if (v & sign)  p |= F_N;
    if (v == sign) p |= F_E;
    cemu_cpu_set_psw(c, p);
}

static void movbz_flags(cpu_t *c, uint8_t src) {
    uint16_t p = cemu_cpu_psw(c) & ~(F_E | F_Z | F_N);
    if (src == 0) p |= F_Z;
    cemu_cpu_set_psw(c, p);
}

static void movbs_flags(cpu_t *c, uint8_t src) {
    uint16_t p = cemu_cpu_psw(c) & ~(F_E | F_Z | F_N);
    if (src == 0)     p |= F_Z;
    if (src & 0x80)   p |= F_N;
    cemu_cpu_set_psw(c, p);
}

/* ---- condition codes --------------------------------------------------- */
static int cond(cpu_t *c, int cc) {
    uint16_t p = cemu_cpu_psw(c);
    int Z = (p & F_Z) != 0, N = (p & F_N) != 0, C = (p & F_C) != 0;
    int V = (p & F_V) != 0, E = (p & F_E) != 0;
    switch (cc) {
        case 0x0: return 1;                 /* UC */
        case 0x1: return !(Z || E);         /* NET */
        case 0x2: return Z;                 /* EQ / Z */
        case 0x3: return !Z;                /* NE / NZ */
        case 0x4: return V;
        case 0x5: return !V;
        case 0x6: return N;
        case 0x7: return !N;
        case 0x8: return C;                 /* C / ULT */
        case 0x9: return !C;                /* NC / UGE */
        case 0xA: return !(Z || (N ^ V));   /* SGT */
        case 0xB: return Z || (N ^ V);      /* SLE */
        case 0xC: return N ^ V;             /* SLT */
        case 0xD: return !(N ^ V);          /* SGE */
        case 0xE: return !(Z || C);         /* UGT */
        case 0xF: return Z || C;            /* ULE */
    }
    return 0;
}

/* ---- sign extension ---------------------------------------------------- */
static int32_t sext(uint32_t v, int bits) {
    uint32_t s = 1u << (bits - 1);
    return (int32_t)((v ^ s) - s);
}

/* ---- data address resolution (DPP/EXTP/EXTS) --------------------------- */
static uint32_t resolve_data(cpu_t *c, uint16_t off16) {
    if (c->ext_kind == EXT_SEG)
        return ((uint32_t)(c->ext_val & 0xFF) << 16) | off16;
    if (c->ext_kind == EXT_PAGE)
        return ((uint32_t)(c->ext_val & 0x3FF) << 14) | (off16 & 0x3FFF);
    uint32_t dpp = rd16(c, 0xFE00 + 2u * ((off16 >> 14) & 3)) & 0x3FF;
    return (dpp << 14) | (off16 & 0x3FFF);
}

/* ---- short 'reg' operand ----------------------------------------------- */
/* Word read/write of an 8-bit reg operand. */
static uint16_t read_reg_w(cpu_t *c, uint8_t reg) {
    if ((reg >> 4) == 0xF) return cemu_cpu_gpr(c, reg & 0xF);
    uint32_t base = c->extr ? 0xF000 : 0xFE00;
    return rd16(c, base + 2u * reg);
}
static void write_reg_w(cpu_t *c, uint8_t reg, uint16_t val) {
    if ((reg >> 4) == 0xF) { cemu_cpu_set_gpr(c, reg & 0xF, val); return; }
    uint32_t base = c->extr ? 0xF000 : 0xFE00;
    wr16(c, base + 2u * reg, val);
}
static uint8_t read_reg_b(cpu_t *c, uint8_t reg) {
    if ((reg >> 4) == 0xF) return gpr_b(c, reg & 0xF);
    uint32_t base = c->extr ? 0xF000 : 0xFE00;
    return rd8(c, base + 2u * reg);
}
static void write_reg_b(cpu_t *c, uint8_t reg, uint8_t val) {
    if ((reg >> 4) == 0xF) { set_gpr_b(c, reg & 0xF, val); return; }
    uint32_t base = c->extr ? 0xF000 : 0xFE00;
    wr8(c, base + 2u * reg, val);
}

/* ---- bitoff operand ---------------------------------------------------- */
static uint32_t bitoff_addr(cpu_t *c, uint8_t bitoff) {
    if (bitoff <= 0x7F) return 0xFD00 + 2u * bitoff;
    if (bitoff <= 0xEF) {
        uint32_t base = c->extr ? 0xF100 : 0xFF00;
        return base + 2u * (bitoff & 0x7F);
    }
    return cp_base(c) + 2u * (bitoff & 0x0F);
}

/* ---- fetch ------------------------------------------------------------- */
static uint8_t fetch8(cpu_t *c) {
    uint8_t b = bus_fetch8(c->bus, cpu_pc(c));
    c->ip = (c->ip + 1) & 0xFFFF;
#if CEMU_INSTRUMENTED
    c->fetch_len++;
#endif
    return b;
}
static uint16_t fetch16(cpu_t *c) {
    uint8_t lo = fetch8(c);
    uint8_t hi = fetch8(c);
    return lo | ((uint16_t)hi << 8);
}

/* ---- stack ------------------------------------------------------------- */
static void cpu_push(cpu_t *c, uint16_t val) {
    uint16_t sp = (rd16(c, 0xFE12) - 2) & 0xFFFF;
    wr16(c, 0xFE12, sp);
    wr16(c, sp & 0xFFFF, val);         /* system stack lives in seg 0 */
}
static uint16_t cpu_pop(cpu_t *c) {
    uint16_t sp = rd16(c, 0xFE12);
    uint16_t val = rd16(c, sp & 0xFFFF);
    wr16(c, 0xFE12, (sp + 2) & 0xFFFF);
    return val;
}

/* ---- typed instrumentation events ------------------------------------- */
#if CEMU_INSTRUMENTED
static inline void notify_call(cpu_t *c, uint32_t caller_pc, uint32_t target_pc,
                               const char *kind) {
    if (!cemu_event_active(c->instrumentation, CEMU_EVENT_CONTROL)) return;
    cemu_event_t event = { .type = CEMU_EVENT_CONTROL };
    event.as.control.caller_pc = caller_pc;
    event.as.control.target_pc = target_pc;
    event.as.control.kind = kind;
    event.as.control.is_return = 0;
    cemu_event_emit(c->instrumentation, &event);
}
static inline void notify_ret(cpu_t *c, const char *kind) {
    if (!cemu_event_active(c->instrumentation, CEMU_EVENT_CONTROL)) return;
    cemu_event_t event = { .type = CEMU_EVENT_CONTROL };
    event.as.control.kind = kind;
    event.as.control.is_return = 1;
    cemu_event_emit(c->instrumentation, &event);
}
static inline void record_trap(cpu_t *c, int trap) {
    if (cemu_event_statistics_enabled(c->instrumentation))
        c->trap_counts[trap & 0xFF]++;
    if (cemu_event_active(c->instrumentation, CEMU_EVENT_TRAP)) {
        cemu_event_t event = { .type = CEMU_EVENT_TRAP };
        event.as.trap.trap = trap;
        cemu_event_emit(c->instrumentation, &event);
    }
}
static inline void record_protected(cpu_t *c, int slot) {
    if (cemu_event_statistics_enabled(c->instrumentation))
        c->protected_ops[slot]++;
    if (cemu_event_active(c->instrumentation, CEMU_EVENT_PROTECTED)) {
        cemu_event_t event = { .type = CEMU_EVENT_PROTECTED };
        event.as.protected_op.slot = slot;
        cemu_event_emit(c->instrumentation, &event);
    }
}
#else
#define notify_call(c, caller_pc, target_pc, kind) ((void)0)
#define notify_ret(c, kind) ((void)0)
#define record_trap(c, trap) ((void)0)
#define record_protected(c, slot) ((void)0)
#endif

/* ---- traps ------------------------------------------------------------- */
static void hardware_trap(cpu_t *c, int trap, uint16_t tfr_bit, uint32_t fault_pc) {
    wr16(c, TFR_ADDR, rd16(c, TFR_ADDR) | tfr_bit);
    cpu_push(c, cemu_cpu_psw(c));
    cpu_push(c, c->csp);
    cpu_push(c, fault_pc & 0xFFFF);
    c->traps_taken++;
    record_trap(c, trap);
    cemu_cpu_set_psw(c, (cemu_cpu_psw(c) & ~0xF000) | 0xF000);
    c->csp = 0x00;
    c->ip = (trap * 4) & 0xFFFF;
    notify_call(c, fault_pc, cpu_pc(c), "btrap");
}

/* ---- EXT window helpers ------------------------------------------------ */
static void do_ext_apply(cpu_t *c, int mode, uint16_t val, int count) {
    /* modes: 0=exts 1=extp 2=extsr 3=extpr */
    switch (mode) {
        case 0: c->ext_kind = EXT_SEG;  c->ext_val = val & 0xFF;  c->ext_count = count; break;
        case 1: c->ext_kind = EXT_PAGE; c->ext_val = val & 0x3FF; c->ext_count = count; break;
        case 2: c->ext_kind = EXT_SEG;  c->ext_val = val & 0xFF;  c->extr = 1; c->ext_count = count; break;
        default:c->ext_kind = EXT_PAGE; c->ext_val = val & 0x3FF; c->extr = 1; c->ext_count = count; break;
    }
}

static void tick_ext(cpu_t *c) {
    if (c->ext_count > 0) {
        c->ext_count--;
        if (c->ext_count == 0) { c->ext_kind = EXT_NONE; c->extr = 0; }
    }
}

/* ---- ALU compute ------------------------------------------------------- */
enum { A_ADD, A_ADDC, A_SUB, A_SUBC, A_CMP, A_XOR, A_AND, A_OR };

static uint16_t alu_compute(cpu_t *c, int name, uint32_t a, uint32_t b, int w) {
    uint32_t mask = (w == 8) ? 0xFF : 0xFFFF;
    switch (name) {
        case A_ADD:  return set_flags_add(c, a, b, w, 0, 0);
        case A_ADDC: return set_flags_add(c, a, b, w, (cemu_cpu_psw(c) & F_C) ? 1 : 0, 1);
        case A_SUB:  return set_flags_sub(c, a, b, w, 0, 0);
        case A_SUBC: return set_flags_sub(c, a, b, w, (cemu_cpu_psw(c) & F_C) ? 1 : 0, 1);
        case A_CMP:  set_flags_sub(c, a, b, w, 0, 0); return (uint16_t)(a & mask);
        case A_AND:  { uint32_t r = (a & b) & mask; set_flags_logic(c, r, w, 1, b); return (uint16_t)r; }
        case A_OR:   { uint32_t r = (a | b) & mask; set_flags_logic(c, r, w, 1, b); return (uint16_t)r; }
        case A_XOR:  { uint32_t r = (a ^ b) & mask; set_flags_logic(c, r, w, 1, b); return (uint16_t)r; }
    }
    return 0;
}

/* Maps ALU high nibble -> name (add/addc/sub/subc/cmp/xor/and/or). */
static int alu_name_for(int hi) {
    switch (hi) {
        case 0x0: return A_ADD;  case 0x1: return A_ADDC;
        case 0x2: return A_SUB;  case 0x3: return A_SUBC;
        case 0x4: return A_CMP;  case 0x5: return A_XOR;
        case 0x6: return A_AND;  case 0x7: return A_OR;
    }
    return -1;
}

static void do_alu(cpu_t *c, uint8_t op, int name) {
    int lo = op & 0xF;
    int byte = (op & 1) == 1;
    int form = lo & 0xE;   /* 0 reg,reg 2 reg,lmem 4 lmem,reg 6 reg,#data 8 idx */
    int w = byte ? 8 : 16;

    if (form == 0x0) {
        uint8_t b = fetch8(c); int dn = (b >> 4) & 0xF, sm = b & 0xF;
        uint32_t a  = byte ? gpr_b(c, dn) : cemu_cpu_gpr(c, dn);
        uint32_t bb = byte ? gpr_b(c, sm) : cemu_cpu_gpr(c, sm);
        uint16_t res = alu_compute(c, name, a, bb, w);
        if (name != A_CMP) { if (byte) set_gpr_b(c, dn, (uint8_t)res); else cemu_cpu_set_gpr(c, dn, res); }
        return;
    }
    if (form == 0x6) {  /* reg, #data16/#data8 */
        uint8_t reg = fetch8(c); uint16_t data = fetch16(c);
        uint32_t a;
        if (byte) { data &= 0xFF; a = read_reg_b(c, reg); }
        else      { a = read_reg_w(c, reg); }
        uint16_t res = alu_compute(c, name, a, data, w);
        if (name != A_CMP) { if (byte) write_reg_b(c, reg, (uint8_t)res); else write_reg_w(c, reg, res); }
        return;
    }
    if (form == 0x8) {  /* reg, [rwi] or #data3 */
        uint8_t b = fetch8(c); int dn = (b >> 4) & 0xF;
        int idx_imm = (b >> 3) & 1, idx_mode = (b >> 2) & 1, rwi = b & 3;
        uint32_t src;
        if (idx_imm == 0) {
            src = b & 7;
        } else {
            uint32_t addr = resolve_data(c, cemu_cpu_gpr(c, rwi));
            src = byte ? rd8(c, addr) : rd16(c, addr);
            if (idx_mode == 1) cemu_cpu_set_gpr(c, rwi, (cemu_cpu_gpr(c, rwi) + (byte ? 1 : 2)) & 0xFFFF);
        }
        uint32_t a = byte ? gpr_b(c, dn) : cemu_cpu_gpr(c, dn);
        uint16_t res = alu_compute(c, name, a, src, w);
        if (name != A_CMP) { if (byte) set_gpr_b(c, dn, (uint8_t)res); else cemu_cpu_set_gpr(c, dn, res); }
        return;
    }
    /* form 0x2 / 0x4: mem <-> lmem */
    {
        uint8_t reg = fetch8(c); uint16_t mem = fetch16(c);
        uint32_t addr = resolve_data(c, mem);
        if (form == 0x2) {  /* dst=reg(short), src=lmem */
            uint32_t a   = byte ? read_reg_b(c, reg) : read_reg_w(c, reg);
            uint32_t src = byte ? rd8(c, addr)       : rd16(c, addr);
            uint16_t res = alu_compute(c, name, a, src, w);
            if (name != A_CMP) { if (byte) write_reg_b(c, reg, (uint8_t)res); else write_reg_w(c, reg, res); }
        } else {            /* dst=lmem, src=reg(short) */
            uint32_t a   = byte ? rd8(c, addr)       : rd16(c, addr);
            uint32_t src = byte ? read_reg_b(c, reg) : read_reg_w(c, reg);
            uint16_t res = alu_compute(c, name, a, src, w);
            if (name != A_CMP) { if (byte) wr8(c, addr, (uint8_t)res); else wr16(c, addr, res); }
        }
    }
}

/* ---- shifts ------------------------------------------------------------ */
enum { SH_ROL, SH_ROR, SH_SHL, SH_SHR };

static void do_shift(cpu_t *c, uint8_t op) {
    int imm = (op == 0x1C || op == 0x3C || op == 0x5C || op == 0x7C);
    int kind;
    switch (op) {
        case 0x0C: case 0x1C: kind = SH_ROL; break;
        case 0x2C: case 0x3C: kind = SH_ROR; break;
        case 0x4C: case 0x5C: kind = SH_SHL; break;
        default:              kind = SH_SHR; break;   /* 0x6C / 0x7C */
    }
    uint8_t b = fetch8(c);
    int rwm, cnt;
    if (imm) { rwm = b & 0xF; cnt = (b >> 4) & 0xF; }
    else     { int rwn = (b >> 4) & 0xF; cnt = cemu_cpu_gpr(c, b & 0xF) & 0xF; rwm = rwn; }
    uint16_t v = cemu_cpu_gpr(c, rwm);
    cnt &= 0xF;
    int carry = 0, v_flag = 0;
    uint16_t r = v;
    for (int i = 0; i < cnt; i++) {
        switch (kind) {
            case SH_SHL: carry = (r >> 15) & 1; r = (r << 1) & 0xFFFF; break;
            case SH_SHR: v_flag |= carry; carry = r & 1; r = (r >> 1) & 0xFFFF; break;
            case SH_ROL: carry = (r >> 15) & 1; r = ((r << 1) | carry) & 0xFFFF; break;
            case SH_ROR: v_flag |= carry; carry = r & 1; r = ((r >> 1) | (carry << 15)) & 0xFFFF; break;
        }
    }
    cemu_cpu_set_gpr(c, rwm, r);
    uint16_t p = cemu_cpu_psw(c) & ~(F_N | F_Z | F_C | F_V | F_E);
    if (r == 0)     p |= F_Z;
    if (r & 0x8000) p |= F_N;
    if (carry)      p |= F_C;
    if (v_flag)     p |= F_V;
    cemu_cpu_set_psw(c, p);
}

/* ---- BSET/BCLR --------------------------------------------------------- */
static void do_bitop(cpu_t *c, uint8_t bitoff, int q, int set_bit) {
    uint32_t addr = bitoff_addr(c, bitoff);
    uint16_t v = rd16(c, addr);
    int prev_bit = (v >> q) & 1;
    v = set_bit ? (v | (1u << q)) : (v & ~(1u << q));
    wr16(c, addr, v);
    uint16_t p = cemu_cpu_psw(c) & ~(F_N | F_Z | F_V | F_C | F_E);
    if (prev_bit) p |= F_N; else p |= F_Z;
    cemu_cpu_set_psw(c, p);
}

/* ---- forward decl of exec --------------------------------------------- */
static void exec(cpu_t *c, uint8_t op, uint32_t pc0);

/* ---- interrupts -------------------------------------------------------- */
static int get_pending_interrupt(cpu_t *c, interrupt_request_t *request) {
    if (!c->interrupts || !c->interrupts->pending) return 0;
    return c->interrupts->pending(c->interrupts->ctx, request);
}

static int deliver_interrupt(cpu_t *c, const interrupt_request_t *request) {
    uint16_t psw = cemu_cpu_psw(c);
    if (!(psw & (1u << 11))) return 0;          /* IEN clear */
    int cur_lvl = (psw >> 12) & 0xF;
    if (request->ilvl <= cur_lvl) return 0;
    if (c->interrupts->begin_service &&
        c->interrupts->begin_service(c->interrupts->ctx, request) ==
            INTERRUPT_SERVICE_PEC)
        return 1;
#if CEMU_INSTRUMENTED
    uint32_t caller_pc = cpu_pc(c);
#endif
    cpu_push(c, psw);
    cpu_push(c, c->csp);
    cpu_push(c, c->ip);
    c->interrupts_delivered++;
    cemu_cpu_set_psw(c, (psw & ~0xF000) | ((uint16_t)request->ilvl << 12));
    if (c->interrupts->acknowledge)
        c->interrupts->acknowledge(c->interrupts->ctx, request);
    c->csp = 0x00;
    c->ip = (request->trap * 4) & 0xFFFF;
    notify_call(c, caller_pc, cpu_pc(c), "irq");
    return 1;
}

static int check_interrupts(cpu_t *c) {
    interrupt_request_t request;
    if (!get_pending_interrupt(c, &request)) return 0;
    return deliver_interrupt(c, &request);
}

/* ---- lifecycle --------------------------------------------------------- */
static const char *PROTECTED_OP_NAMES[6] = {
    "diswdt", "einit", "idle", "pwrdn", "srst", "srvwdt"
};
const char *cemu_cpu_protected_op_name(int slot) {
    return (slot >= 0 && slot < 6) ? PROTECTED_OP_NAMES[slot] : "?";
}

void cemu_cpu_init(cpu_t *c, bus_t *bus) {
    c->bus = bus;
    c->interrupts = NULL;
    c->csp = 0; c->ip = 0;
    c->ext_kind = EXT_NONE; c->ext_val = 0; c->ext_count = 0; c->extr = 0;
    c->halted = 0; c->idle = 0;
    c->icount = 0;
    c->interrupts_delivered = 0; c->traps_taken = 0;
    memset(c->trap_counts, 0, sizeof c->trap_counts);
    memset(c->protected_ops, 0, sizeof c->protected_ops);
#if CEMU_INSTRUMENTED
    c->fetch_len = 0;
#endif
    c->unimpl = 0; c->unimpl_op = 0; c->unimpl_pc = 0;
    c->instrumentation = NULL;
}

void cemu_cpu_attach_interrupt_port(cpu_t *c, interrupt_port_t *interrupts) {
    c->interrupts = interrupts;
}

void cemu_cpu_reset(cpu_t *c) {
    c->csp = 0; c->ip = 0;
    c->ext_kind = EXT_NONE; c->ext_val = 0; c->ext_count = 0; c->extr = 0;
    c->halted = 0;
}

step_result_t cemu_cpu_step(cpu_t *c) {
    c->last_ran = 0;
    if (c->halted) return STEP_OK;
    if (c->bus->tick) c->bus->tick(c->bus->ctx, 1);

    if (c->idle) {
        interrupt_request_t request;
        if (get_pending_interrupt(c, &request)) {
            /* An enabled request terminates IDLE independently of whether
             * interrupt/PEC service is currently allowed.  In particular,
             * ATOMIC/EXT still defers service, but it must not leave the CPU
             * parked forever with an instruction remaining in the window. */
            c->idle = 0;
            if (c->ext_kind == EXT_NONE && !c->extr && c->ext_count == 0)
                (void)deliver_interrupt(c, &request);
        }
    } else if (c->ext_kind == EXT_NONE && !c->extr && c->ext_count == 0) {
        (void)check_interrupts(c);
    }
    if (c->idle) { c->icount++; return STEP_OK; }

    uint32_t pc0 = cpu_pc(c);
    int had_ext = (c->ext_kind != EXT_NONE) || c->extr || c->ext_count > 0;
#if CEMU_INSTRUMENTED
    c->fetch_len = 0;
#endif
    uint8_t op = fetch8(c);
    c->icount++;
    exec(c, op, pc0);
    /* A nested ATOMIC/EXT instruction reloads the one hardware sequence
     * counter. Its new count covers the following instructions, so do not
     * consume the freshly loaded count as part of the older sequence. */
    if (had_ext && op != 0xD1 && op != 0xD7 && op != 0xDC) tick_ext(c);
    c->last_ran = 1;
    return c->unimpl ? STEP_UNIMPL : STEP_OK;
}

static void set_unimpl(cpu_t *c, uint32_t op, uint32_t pc0) {
    c->unimpl = 1; c->unimpl_op = op; c->unimpl_pc = pc0;
}

/* ======================================================================= */
static void exec(cpu_t *c, uint8_t op, uint32_t pc0) {
    /* ---- 32-bit fixed-pattern protected ops --------------------------- */
    if (op == 0xA5 || op == 0xB5 || op == 0x87 || op == 0x97 || op == 0xB7 || op == 0xA7) {
        uint8_t b1 = fetch8(c);
        uint16_t w2 = fetch16(c);
        uint32_t full = op | ((uint32_t)b1 << 8) | ((uint32_t)w2 << 16);
        switch (full) {
            case 0xA5A55AA5u: /* diswdt */ record_protected(c, POP_DISWDT); return;
            case 0xB5B54AB5u: /* einit  */
                record_protected(c, POP_EINIT);
                if (c->bus->end_init) c->bus->end_init(c->bus->ctx);
                return;
            case 0x87877887u: /* idle   */ record_protected(c, POP_IDLE); c->idle = 1; return;
            case 0x97976897u: /* pwrdn  */ record_protected(c, POP_PWRDN); c->idle = 1; return;
            case 0xB7B748B7u: /* srst   */
                record_protected(c, POP_SRST);
                if (c->bus->reset_core) c->bus->reset_core(c->bus->ctx);
                cemu_cpu_set_psw(c, 0x0000);
                cemu_cpu_reset(c);
                return;
            case 0xA7A758A7u: /* srvwdt */ record_protected(c, POP_SRVWDT); return;
            default: set_unimpl(c, full, pc0); return;
        }
    }

    if (op == 0xCC) { fetch8(c); return; }   /* nop */

    if (op == 0xCB) { fetch8(c); c->ip = cpu_pop(c); notify_ret(c, "ret"); return; }      /* ret */
    if (op == 0xDB) { fetch8(c); c->ip = cpu_pop(c); c->csp = cpu_pop(c) & 0xFF;          /* rets */
                      notify_ret(c, "rets"); return; }
    if (op == 0xFB) {                                                        /* reti */
        fetch8(c);
        c->ip = cpu_pop(c);
        c->csp = cpu_pop(c) & 0xFF;
        cemu_cpu_set_psw(c, cpu_pop(c));
        notify_ret(c, "reti");
        return;
    }

    int hi = op >> 4, lo = op & 0xF;

    /* ---- MOV/MOVB immediate & reg-reg short forms --------------------- */
    if (op == 0xE0) { uint8_t b = fetch8(c); int data4 = (b >> 4) & 0xF, rwm = b & 0xF;
                      cemu_cpu_set_gpr(c, rwm, data4); mov_flags(c, data4, 16); return; }
    if (op == 0xE1) { uint8_t b = fetch8(c); int data4 = (b >> 4) & 0xF, rbm = b & 0xF;
                      set_gpr_b(c, rbm, data4); mov_flags(c, data4, 8); return; }
    if (op == 0xF0) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint16_t src = cemu_cpu_gpr(c, rwm); cemu_cpu_set_gpr(c, rwn, src); mov_flags(c, src, 16); return; }
    if (op == 0xF1) { uint8_t b = fetch8(c); int rbn = (b >> 4) & 0xF, rbm = b & 0xF;
                      uint8_t src = gpr_b(c, rbm); set_gpr_b(c, rbn, src); mov_flags(c, src, 8); return; }

    if (op == 0xE6) { uint8_t reg = fetch8(c); uint16_t data = fetch16(c);
                      write_reg_w(c, reg, data); mov_flags(c, data, 16); return; }
    if (op == 0xE7) { uint8_t reg = fetch8(c); uint16_t data = fetch16(c) & 0xFF;
                      write_reg_b(c, reg, (uint8_t)data); mov_flags(c, data, 8); return; }

    if (op == 0xF2) { uint8_t reg = fetch8(c); uint16_t mem = fetch16(c);
                      uint16_t v = rd16(c, resolve_data(c, mem)); write_reg_w(c, reg, v); mov_flags(c, v, 16); return; }
    if (op == 0xF6) { uint8_t reg = fetch8(c); uint16_t mem = fetch16(c);
                      uint16_t v = read_reg_w(c, reg); wr16(c, resolve_data(c, mem), v); mov_flags(c, v, 16); return; }
    if (op == 0xF3) { uint8_t reg = fetch8(c); uint16_t mem = fetch16(c);
                      uint8_t v = rd8(c, resolve_data(c, mem)); write_reg_b(c, reg, v); mov_flags(c, v, 8); return; }
    if (op == 0xF7) { uint8_t reg = fetch8(c); uint16_t mem = fetch16(c);
                      uint8_t v = read_reg_b(c, reg); wr8(c, resolve_data(c, mem), v); mov_flags(c, v, 8); return; }

    /* ---- indexed MOV [rwm + #off16] ----------------------------------- */
    if (op == 0xC4 || op == 0xD4 || op == 0xE4 || op == 0xF4) {
        uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
        uint16_t off = fetch16(c);
        uint32_t addr = resolve_data(c, (cemu_cpu_gpr(c, rwm) + off) & 0xFFFF);
        if (op == 0xD4)      { uint16_t v = rd16(c, addr); cemu_cpu_set_gpr(c, rwn, v); mov_flags(c, v, 16); }
        else if (op == 0xC4) { uint16_t v = cemu_cpu_gpr(c, rwn); wr16(c, addr, v); mov_flags(c, v, 16); }
        else if (op == 0xF4) { uint8_t v = rd8(c, addr); set_gpr_b(c, rwn, v); mov_flags(c, v, 8); }
        else                 { uint8_t v = gpr_b(c, rwn); wr8(c, addr, v); mov_flags(c, v, 8); }
        return;
    }

    /* ---- indirect MOV [rwm] family ------------------------------------ */
    if (op == 0xA8) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint16_t v = rd16(c, resolve_data(c, cemu_cpu_gpr(c, rwm))); cemu_cpu_set_gpr(c, rwn, v); mov_flags(c, v, 16); return; }
    if (op == 0x98) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint16_t v = rd16(c, resolve_data(c, cemu_cpu_gpr(c, rwm))); cemu_cpu_set_gpr(c, rwn, v);
                      cemu_cpu_set_gpr(c, rwm, (cemu_cpu_gpr(c, rwm) + 2) & 0xFFFF); mov_flags(c, v, 16); return; }
    if (op == 0xB8) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint16_t v = cemu_cpu_gpr(c, rwn); wr16(c, resolve_data(c, cemu_cpu_gpr(c, rwm)), v); mov_flags(c, v, 16); return; }
    if (op == 0x88) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      cemu_cpu_set_gpr(c, rwm, (cemu_cpu_gpr(c, rwm) - 2) & 0xFFFF);
                      uint16_t v = cemu_cpu_gpr(c, rwn); wr16(c, resolve_data(c, cemu_cpu_gpr(c, rwm)), v); mov_flags(c, v, 16); return; }
    if (op == 0xA9) { uint8_t b = fetch8(c); int rbn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint8_t v = rd8(c, resolve_data(c, cemu_cpu_gpr(c, rwm))); set_gpr_b(c, rbn, v); mov_flags(c, v, 8); return; }
    if (op == 0x99) { uint8_t b = fetch8(c); int rbn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint8_t v = rd8(c, resolve_data(c, cemu_cpu_gpr(c, rwm))); set_gpr_b(c, rbn, v);
                      cemu_cpu_set_gpr(c, rwm, (cemu_cpu_gpr(c, rwm) + 1) & 0xFFFF); mov_flags(c, v, 8); return; }
    if (op == 0xB9) { uint8_t b = fetch8(c); int rbn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint8_t v = gpr_b(c, rbn); wr8(c, resolve_data(c, cemu_cpu_gpr(c, rwm)), v); mov_flags(c, v, 8); return; }

    /* ---- MOVBZ / MOVBS reg-reg & reg-mem ------------------------------ */
    if (op == 0xC0) { uint8_t b = fetch8(c); int rwm = b & 0xF, rbn = (b >> 4) & 0xF;
                      uint8_t src = gpr_b(c, rbn); cemu_cpu_set_gpr(c, rwm, src); movbz_flags(c, src); return; }
    if (op == 0xD0) { uint8_t b = fetch8(c); int rwm = b & 0xF, rbn = (b >> 4) & 0xF;
                      uint8_t src = gpr_b(c, rbn); cemu_cpu_set_gpr(c, rwm, (uint16_t)sext(src, 8)); movbs_flags(c, src); return; }
    if (op == 0xC2) { uint8_t reg = fetch8(c); uint16_t mem = fetch16(c);
                      uint8_t src = rd8(c, resolve_data(c, mem)); write_reg_w(c, reg, src); movbz_flags(c, src); return; }
    if (op == 0xC5) { uint8_t reg = fetch8(c); uint16_t mem = fetch16(c);
                      uint8_t src = read_reg_b(c, reg); wr16(c, resolve_data(c, mem), src); movbz_flags(c, src); return; }
    if (op == 0xD2) { uint8_t reg = fetch8(c); uint16_t mem = fetch16(c);
                      uint8_t src = rd8(c, resolve_data(c, mem)); write_reg_w(c, reg, (uint16_t)sext(src, 8)); movbs_flags(c, src); return; }
    if (op == 0xD5) { uint8_t reg = fetch8(c); uint16_t mem = fetch16(c);
                      uint8_t src = read_reg_b(c, reg); wr16(c, resolve_data(c, mem), (uint16_t)sext(src, 8)); movbs_flags(c, src); return; }

    /* ---- undefined CMP store forms 0x44/0x45 -> BTRAP ----------------- */
    if (hi == 0x4 && (lo == 0x4 || lo == 0x5)) {
        hardware_trap(c, BTRAP_VEC, TFR_UNDOPC, pc0);
        return;
    }

    /* ---- ALU families ------------------------------------------------- */
    {
        int name = alu_name_for(hi);
        if (name >= 0 && lo <= 0x9) { do_alu(c, op, name); return; }
    }

    /* ---- BCLR/BSET (low nibble E/F) ----------------------------------- */
    if (lo == 0xE) { uint8_t bitoff = fetch8(c); do_bitop(c, bitoff, hi, 0); return; }
    if (lo == 0xF) { uint8_t bitoff = fetch8(c); do_bitop(c, bitoff, hi, 1); return; }

    /* ---- JMPR cc,rel (low nibble D) ----------------------------------- */
    if (lo == 0xD) {
        int cc = hi; uint8_t rel = fetch8(c);
        if (cond(c, cc)) c->ip = (c->ip + sext(rel, 8) * 2) & 0xFFFF;
        return;
    }

    /* ---- two-bitoff bit logic ----------------------------------------- */
    if (op == 0x6A || op == 0x5A || op == 0x7A || op == 0x4A || op == 0x3A || op == 0x2A) {
        uint8_t b1 = fetch8(c); uint16_t w = fetch16(c);
        uint32_t src_addr = bitoff_addr(c, b1);
        uint32_t dst_addr = bitoff_addr(c, w & 0xFF);
        int bit_z = (w >> 8) & 0xF, bit_q = (w >> 12) & 0xF;
        int qv = (rd16(c, src_addr) >> bit_q) & 1;
        int zv = (rd16(c, dst_addr) >> bit_z) & 1;
        int r = -1;   /* -1 = bcmp (no write) */
        int logic = 1;
        switch (op) {
            case 0x6A: r = zv & qv; break;      /* band */
            case 0x5A: r = zv | qv; break;      /* bor */
            case 0x7A: r = zv ^ qv; break;      /* bxor */
            case 0x4A: r = qv; logic = 0; break;/* bmov */
            case 0x3A: r = qv ^ 1; logic = 0; break; /* bmovn */
            case 0x2A: logic = 1; break;        /* bcmp (no write) */
        }
        uint16_t p = cemu_cpu_psw(c) & ~(F_Z | F_N | F_E | F_C | F_V);
        if (op == 0x4A || op == 0x3A) {   /* bmov/bmovn: Z/N reflect source bit */
            if (qv) p |= F_N;
            if (qv == 0) p |= F_Z;
        } else {                          /* band/bor/bxor/bcmp */
            if (zv ^ qv) p |= F_N;
            if (zv & qv) p |= F_C;
            if (zv | qv) p |= F_V;
            if ((zv | qv) == 0) p |= F_Z;
        }
        cemu_cpu_set_psw(c, p);
        if (op != 0x2A) {   /* everything but bcmp writes */
            uint16_t dv = rd16(c, dst_addr);
            dv = r ? (dv | (1u << bit_z)) : (dv & ~(1u << bit_z));
            wr16(c, dst_addr, dv);
        }
        (void)logic;
        return;
    }

    /* ---- bfldl (0x0A) / bfldh (0x1A) ---------------------------------- */
    if (op == 0x0A || op == 0x1A) {
        uint8_t bitoff = fetch8(c); uint16_t w = fetch16(c);
        int mask, data;
        if (op == 0x0A) { mask = w & 0xFF; data = (w >> 8) & 0xFF; }
        else            { data = w & 0xFF; mask = (w >> 8) & 0xFF; }
        uint32_t addr = bitoff_addr(c, bitoff);
        uint16_t cur = rd16(c, addr);
        if (op == 0x0A) cur = (cur & ~mask) | data;
        else            cur = (cur & ~(mask << 8)) | (data << 8);
        wr16(c, addr, cur);
        set_flags_logic(c, cur, 16, 0, 0);
        return;
    }

    /* ---- JB / JNB / JBC ----------------------------------------------- */
    if (op == 0x8A || op == 0x9A || op == 0xAA) {
        uint8_t bitoff = fetch8(c); uint16_t w = fetch16(c);
        int q = (w >> 12) & 0xF; uint8_t rel = w & 0xFF;
        uint32_t addr = bitoff_addr(c, bitoff);
        int bitval = (rd16(c, addr) >> q) & 1;
        int take = (op == 0x9A) ? (bitval == 0) : (bitval == 1);
        if (op == 0xAA) {   /* JBC sets flags from tested bit */
            uint16_t p = cemu_cpu_psw(c) & ~(F_N | F_Z | F_V | F_C | F_E);
            p |= bitval ? F_N : F_Z;
            cemu_cpu_set_psw(c, p);
        }
        if (take) {
            if (op == 0xAA) wr16(c, addr, rd16(c, addr) & ~(1u << q));
            c->ip = (c->ip + sext(rel, 8) * 2) & 0xFFFF;
        }
        return;
    }

    /* ---- JMPA / CALLA ------------------------------------------------- */
    if (op == 0xEA || op == 0xCA) {
        uint8_t b = fetch8(c); uint16_t caddr = fetch16(c);
        int cc = (b >> 4) & 0xF;
        if (cond(c, cc)) {
            if (op == 0xCA) cpu_push(c, c->ip);
            c->ip = caddr;
            if (op == 0xCA) notify_call(c, pc0, cpu_pc(c), "call");
        }
        return;
    }

    /* ---- CALLR -------------------------------------------------------- */
    if (op == 0xBB) {
        uint8_t rel = fetch8(c);
        cpu_push(c, c->ip);
        c->ip = (c->ip + sext(rel, 8) * 2) & 0xFFFF;
        notify_call(c, pc0, cpu_pc(c), "call");
        return;
    }

    /* ---- JMPS / CALLS ------------------------------------------------- */
    if (op == 0xFA || op == 0xDA) {
        uint8_t seg = fetch8(c); uint16_t caddr = fetch16(c);
        if (op == 0xDA) { cpu_push(c, c->csp); cpu_push(c, c->ip); }
        c->csp = seg & 0xFF; c->ip = caddr;
        if (op == 0xDA) notify_call(c, pc0, cpu_pc(c), "call");
        return;
    }

    /* ---- JMPI / CALLI ------------------------------------------------- */
    if (op == 0x9C || op == 0xAB) {
        uint8_t b = fetch8(c); int cc = (b >> 4) & 0xF, rwm = b & 0xF;
        if (cond(c, cc)) {
            if (op == 0xAB) cpu_push(c, c->ip);
            c->ip = cemu_cpu_gpr(c, rwm);
            if (op == 0xAB) notify_call(c, pc0, cpu_pc(c), "call");
        }
        return;
    }

    /* ---- EXTS(R)/EXTP(R) immediate (0xD7) / indirect (0xDC) ----------- */
    if (op == 0xD7) {
        uint8_t b = fetch8(c); uint16_t w = fetch16(c);
        int mode = (b >> 6) & 3, count = ((b >> 4) & 3) + 1;
        do_ext_apply(c, mode, w, count);
        return;
    }
    if (op == 0xDC) {
        uint8_t b = fetch8(c);
        int mode = (b >> 6) & 3, count = ((b >> 4) & 3) + 1, rwm = b & 0xF;
        do_ext_apply(c, mode, cemu_cpu_gpr(c, rwm), count);
        return;
    }
    if (op == 0xD1) {   /* atomic / extr */
        uint8_t b = fetch8(c);
        int mode = (b >> 6) & 3, count = ((b >> 4) & 3) + 1;
        if (mode == 2)      { c->extr = 1; c->ext_count = count; return; }
        else if (mode == 0) { c->ext_count = count; return; }
        set_unimpl(c, 0xD1, pc0);
        return;
    }

    /* ---- TRAP #n ------------------------------------------------------ */
    if (op == 0x9B) {
        uint8_t b = fetch8(c); int trap7 = (b >> 1) & 0x7F; int vec = trap7 * 4;
        cpu_push(c, cemu_cpu_psw(c)); cpu_push(c, c->csp); cpu_push(c, c->ip);
        c->traps_taken++;
        record_trap(c, trap7);
        c->csp = 0x00; c->ip = vec;
        notify_call(c, pc0, cpu_pc(c), "trap");
        return;
    }

    /* ---- PCALL -------------------------------------------------------- */
    if (op == 0xE2) {
        uint8_t reg = fetch8(c); uint16_t caddr = fetch16(c);
        uint16_t val = read_reg_w(c, reg);
        cpu_push(c, val); cpu_push(c, c->ip);
        c->ip = caddr;
        notify_call(c, pc0, cpu_pc(c), "call");
        mov_flags(c, val, 16);
        return;
    }

    /* ---- SCXT --------------------------------------------------------- */
    if (op == 0xC6 || op == 0xD6) {
        uint8_t reg = fetch8(c); uint16_t src = fetch16(c);
        uint16_t cur;
        int is_gpr = (reg >> 4) == 0xF;
        uint32_t memaddr = c->extr ? 0xF000 + 2u * reg : 0xFE00 + 2u * reg;
        cur = is_gpr ? cemu_cpu_gpr(c, reg & 0xF) : rd16(c, memaddr);
        cpu_push(c, cur);
        uint16_t newval = (op == 0xC6) ? src : rd16(c, resolve_data(c, src));
        if (is_gpr) cemu_cpu_set_gpr(c, reg & 0xF, newval); else wr16(c, memaddr, newval);
        return;
    }

    /* ---- PRIOR -------------------------------------------------------- */
    if (op == 0x2B) {
        uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
        uint16_t src = cemu_cpu_gpr(c, rwm);
        uint16_t v = src; int count = 0;
        if (v != 0) while (!(v & 0x8000) && count < 15) { v = (v << 1) & 0xFFFF; count++; }
        cemu_cpu_set_gpr(c, rwn, count);
        uint16_t p = cemu_cpu_psw(c) & ~(F_N | F_Z | F_V | F_C | F_E);
        if (src == 0) p |= F_Z;
        cemu_cpu_set_psw(c, p);
        return;
    }

    /* ---- CPL / NEG (word) --------------------------------------------- */
    if (op == 0x91) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF;
                      uint16_t orig = cemu_cpu_gpr(c, rwn); uint16_t v = ~orig; cemu_cpu_set_gpr(c, rwn, v);
                      set_flags_logic(c, v, 16, 1, orig); return; }
    if (op == 0x81) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF;
                      uint16_t orig = cemu_cpu_gpr(c, rwn); uint16_t v = set_flags_sub(c, 0, orig, 16, 0, 0);
                      cemu_cpu_set_gpr(c, rwn, v); return; }

    /* ---- shifts ------------------------------------------------------- */
    if (op == 0x0C || op == 0x1C || op == 0x2C || op == 0x3C ||
        op == 0x4C || op == 0x5C || op == 0x6C || op == 0x7C) {
        do_shift(c, op);
        return;
    }

    /* ---- ASHR --------------------------------------------------------- */
    if (op == 0xAC || op == 0xBC) {
        uint8_t b = fetch8(c);
        int cnt, dst;
        if (op == 0xAC) { int rwn = (b >> 4) & 0xF; cnt = cemu_cpu_gpr(c, b & 0xF) & 0xF; dst = rwn; }
        else            { cnt = (b >> 4) & 0xF; dst = b & 0xF; }
        uint16_t v = cemu_cpu_gpr(c, dst); uint16_t sign = v & 0x8000;
        uint16_t r = v; int carry = 0, v_flag = 0;
        for (int i = 0; i < cnt; i++) {
            v_flag |= carry;
            carry = r & 1;
            r = ((r >> 1) | sign) & 0xFFFF;
        }
        cemu_cpu_set_gpr(c, dst, r);
        uint16_t p = cemu_cpu_psw(c) & ~(F_N | F_Z | F_C | F_V | F_E);
        if (r == 0)     p |= F_Z;
        if (r & 0x8000) p |= F_N;
        if (carry)      p |= F_C;
        if (v_flag)     p |= F_V;
        cemu_cpu_set_psw(c, p);
        return;
    }

    /* ---- CMPI/CMPD loop ops ------------------------------------------- */
    if (op == 0x80 || op == 0x82 || op == 0x86 || op == 0x90 || op == 0x92 || op == 0x96 ||
        op == 0xA0 || op == 0xA2 || op == 0xA6 || op == 0xB0 || op == 0xB2 || op == 0xB6) {
        int delta;
        switch (hi) { case 0x8: delta = +1; break; case 0x9: delta = +2; break;
                      case 0xA: delta = -1; break; default: delta = -2; break; }
        int rwn;
        if (lo == 0x0) { uint8_t b = fetch8(c); int data4 = (b >> 4) & 0xF; rwn = b & 0xF;
                         set_flags_sub(c, cemu_cpu_gpr(c, rwn), data4, 16, 0, 0); }
        else if (lo == 0x2) { uint8_t reg = fetch8(c); uint16_t mem = fetch16(c); rwn = reg & 0xF;
                              set_flags_sub(c, cemu_cpu_gpr(c, rwn), rd16(c, resolve_data(c, mem)), 16, 0, 0); }
        else { uint8_t reg = fetch8(c); uint16_t data = fetch16(c); rwn = reg & 0xF;
               set_flags_sub(c, cemu_cpu_gpr(c, rwn), data, 16, 0, 0); }
        cemu_cpu_set_gpr(c, rwn, (cemu_cpu_gpr(c, rwn) + delta) & 0xFFFF);
        return;
    }

    /* ---- NEGB / CPLB -------------------------------------------------- */
    if (op == 0xA1) { uint8_t b = fetch8(c); int rbn = (b >> 4) & 0xF;
                      uint8_t orig = gpr_b(c, rbn); uint8_t v = (uint8_t)set_flags_sub(c, 0, orig, 8, 0, 0);
                      set_gpr_b(c, rbn, v); return; }
    if (op == 0xB1) { uint8_t b = fetch8(c); int rbn = (b >> 4) & 0xF;
                      uint8_t orig = gpr_b(c, rbn); uint8_t v = ~orig; set_gpr_b(c, rbn, v);
                      set_flags_logic(c, v, 8, 1, orig); return; }

    /* ---- JNBS --------------------------------------------------------- */
    if (op == 0xBA) {
        uint8_t bitoff = fetch8(c); uint16_t w = fetch16(c);
        int q = (w >> 12) & 0xF; uint8_t rel = w & 0xFF;
        uint32_t addr = bitoff_addr(c, bitoff);
        int bitval = (rd16(c, addr) >> q) & 1;
        uint16_t p = cemu_cpu_psw(c) & ~(F_N | F_Z | F_V | F_C | F_E);
        p |= bitval ? F_N : F_Z;
        cemu_cpu_set_psw(c, p);
        if (bitval == 0) {
            wr16(c, addr, rd16(c, addr) | (1u << q));
            c->ip = (c->ip + sext(rel, 8) * 2) & 0xFFFF;
        }
        return;
    }

    /* ---- RETP --------------------------------------------------------- */
    if (op == 0xEB) {
        uint8_t reg = fetch8(c);
        c->ip = cpu_pop(c);
        uint16_t tmp = cpu_pop(c);
        write_reg_w(c, reg, tmp);
        mov_flags(c, tmp, 16);
        return;
    }

    /* ---- MOV/MOVB indirect reg-pair & [Rn],mem ------------------------ */
    if (op == 0xC8) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint16_t v = rd16(c, resolve_data(c, cemu_cpu_gpr(c, rwm)));
                      wr16(c, resolve_data(c, cemu_cpu_gpr(c, rwn)), v); mov_flags(c, v, 16); return; }
    if (op == 0xD8) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint16_t v = rd16(c, resolve_data(c, cemu_cpu_gpr(c, rwm)));
                      wr16(c, resolve_data(c, cemu_cpu_gpr(c, rwn)), v);
                      cemu_cpu_set_gpr(c, rwn, (cemu_cpu_gpr(c, rwn) + 2) & 0xFFFF); mov_flags(c, v, 16); return; }
    if (op == 0xE8) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint16_t v = rd16(c, resolve_data(c, cemu_cpu_gpr(c, rwm)));
                      wr16(c, resolve_data(c, cemu_cpu_gpr(c, rwn)), v);
                      cemu_cpu_set_gpr(c, rwm, (cemu_cpu_gpr(c, rwm) + 2) & 0xFFFF); mov_flags(c, v, 16); return; }
    if (op == 0xC9) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint8_t v = rd8(c, resolve_data(c, cemu_cpu_gpr(c, rwm)));
                      wr8(c, resolve_data(c, cemu_cpu_gpr(c, rwn)), v); mov_flags(c, v, 8); return; }
    if (op == 0xD9) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint8_t v = rd8(c, resolve_data(c, cemu_cpu_gpr(c, rwm)));
                      wr8(c, resolve_data(c, cemu_cpu_gpr(c, rwn)), v);
                      cemu_cpu_set_gpr(c, rwn, (cemu_cpu_gpr(c, rwn) + 1) & 0xFFFF); mov_flags(c, v, 8); return; }
    if (op == 0xE9) { uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
                      uint8_t v = rd8(c, resolve_data(c, cemu_cpu_gpr(c, rwm)));
                      wr8(c, resolve_data(c, cemu_cpu_gpr(c, rwn)), v);
                      cemu_cpu_set_gpr(c, rwm, (cemu_cpu_gpr(c, rwm) + 1) & 0xFFFF); mov_flags(c, v, 8); return; }
    if (op == 0x84) { uint8_t b = fetch8(c); int rwn = b & 0xF; uint16_t mem = fetch16(c);
                      uint16_t v = rd16(c, resolve_data(c, mem));
                      wr16(c, resolve_data(c, cemu_cpu_gpr(c, rwn)), v); mov_flags(c, v, 16); return; }
    if (op == 0x94) { uint8_t b = fetch8(c); int rwn = b & 0xF; uint16_t mem = fetch16(c);
                      uint16_t v = rd16(c, resolve_data(c, cemu_cpu_gpr(c, rwn)));
                      wr16(c, resolve_data(c, mem), v); mov_flags(c, v, 16); return; }
    if (op == 0xA4) { uint8_t b = fetch8(c); int rwn = b & 0xF; uint16_t mem = fetch16(c);
                      uint8_t v = rd8(c, resolve_data(c, mem));
                      wr8(c, resolve_data(c, cemu_cpu_gpr(c, rwn)), v); mov_flags(c, v, 8); return; }
    if (op == 0xB4) { uint8_t b = fetch8(c); int rwn = b & 0xF; uint16_t mem = fetch16(c);
                      uint8_t v = rd8(c, resolve_data(c, cemu_cpu_gpr(c, rwn)));
                      wr8(c, resolve_data(c, mem), v); mov_flags(c, v, 8); return; }
    if (op == 0x89) { uint8_t b = fetch8(c); int rbn = (b >> 4) & 0xF, rwm = b & 0xF;
                      cemu_cpu_set_gpr(c, rwm, (cemu_cpu_gpr(c, rwm) - 1) & 0xFFFF);
                      uint8_t v = gpr_b(c, rbn); wr8(c, resolve_data(c, cemu_cpu_gpr(c, rwm)), v); mov_flags(c, v, 8); return; }

    /* ---- MUL / MULU --------------------------------------------------- */
    if (op == 0x0B || op == 0x1B) {
        uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF;
        uint16_t x = cemu_cpu_gpr(c, rwn), y = cemu_cpu_gpr(c, rwm);
        uint32_t res32;
        if (op == 0x0B) res32 = (uint32_t)(int32_t)(sext(x, 16) * sext(y, 16));
        else            res32 = (uint32_t)x * (uint32_t)y;
        wr16(c, 0xFE0E, res32 & 0xFFFF);          /* MDL */
        wr16(c, 0xFE0C, (res32 >> 16) & 0xFFFF);  /* MDH */
        uint16_t p = cemu_cpu_psw(c) & ~(F_C | F_V | F_E | F_Z | F_N);
        if (res32 == 0)          p |= F_Z;
        if (res32 & 0x80000000u) p |= F_N;
        uint16_t mdl16 = res32 & 0xFFFF, mdh16 = (res32 >> 16) & 0xFFFF;
        int fits;
        if (op == 0x0B) fits = (mdh16 == ((mdl16 & 0x8000) ? 0xFFFF : 0x0000));
        else            fits = (mdh16 == 0x0000);
        if (!fits) p |= F_V;
        cemu_cpu_set_psw(c, p);
        return;
    }

    /* ---- DIV / DIVU / DIVL / DIVLU ------------------------------------ */
    if (op == 0x4B || op == 0x5B || op == 0x6B || op == 0x7B) {
        uint8_t b = fetch8(c); int rwn = (b >> 4) & 0xF;
        uint16_t divisor = cemu_cpu_gpr(c, rwn);
        uint16_t mdl = rd16(c, 0xFE0E), mdh = rd16(c, 0xFE0C);
        uint16_t p = cemu_cpu_psw(c) & ~(F_C | F_V | F_E | F_Z | F_N);
        if (divisor == 0) { p |= F_V; cemu_cpu_set_psw(c, p); return; }
        int64_t q, r;
        if (op == 0x4B)      { int32_t dv = sext(mdl, 16); int32_t dd = sext(divisor, 16);
                               q = dv / dd; r = dv - q * dd; }
        else if (op == 0x5B) { q = mdl / divisor; r = mdl % divisor; }
        else if (op == 0x6B) { int32_t dv = (int32_t)(((uint32_t)mdh << 16) | mdl); int32_t dd = sext(divisor, 16);
                               q = dv / dd; r = dv - q * dd; }
        else                 { uint32_t dv = ((uint32_t)mdh << 16) | mdl; q = dv / divisor; r = dv % divisor; }
        int qfits = (op == 0x4B || op == 0x6B) ? (q >= -0x8000 && q <= 0x7FFF) : (q >= 0 && q <= 0xFFFF);
        if (!qfits) p |= F_V;
        wr16(c, 0xFE0E, (uint16_t)(q & 0xFFFF));  /* MDL = quotient */
        wr16(c, 0xFE0C, (uint16_t)(r & 0xFFFF));  /* MDH = remainder */
        if ((q & 0xFFFF) == 0) p |= F_Z;
        if (q & 0x8000)        p |= F_N;
        cemu_cpu_set_psw(c, p);
        return;
    }

    /* ---- PUSH / POP --------------------------------------------------- */
    if (op == 0xEC) { uint8_t reg = fetch8(c); uint16_t val = read_reg_w(c, reg); cpu_push(c, val); mov_flags(c, val, 16); return; }
    if (op == 0xFC) { uint8_t reg = fetch8(c); uint16_t val = cpu_pop(c); write_reg_w(c, reg, val); mov_flags(c, val, 16); return; }

    set_unimpl(c, op, pc0);
}
