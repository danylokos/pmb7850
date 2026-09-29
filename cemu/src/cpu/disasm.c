/* Contextual C166S disassembler, using event-free peeks over a local cursor. */
#include <stdio.h>
#include <string.h>
#include "disasm.h"
#include "names.h"

/* C166S V1 manual, PDF pp.82-85: byte indices alias halves of R0..R7. */
static const char *const BYTE_REG[16] = {
    "rl0", "rh0", "rl1", "rh1", "rl2", "rh2", "rl3", "rh3",
    "rl4", "rh4", "rl5", "rh5", "rl6", "rh6", "rl7", "rh7",
};

/* Condition-code names, used as cc_<name>. */
static const char *CC_NAME[16] = {
    "uc","net","eq","ne","v","nv","n","nn","c","nc","sgt","sle","slt","sge","ugt","ule",
};

/* Local decode cursor: byte fetch over event-free peeks; tracks length. */
typedef struct { cpu_t *cpu; soc_t *soc; uint8_t csp; uint16_t ip; int len; } cur_t;

static uint8_t f8(cur_t *c) {
    uint32_t pc = ((uint32_t)(c->csp & 0xFF) << 16) | (c->ip & 0xFFFF);
    uint8_t b = cemu_memory_controller_peek8(&c->soc->memory, pc);
    c->ip = (c->ip + 1) & 0xFFFF;
    c->len++;
    return b;
}
static uint16_t f16(cur_t *c) { uint8_t lo = f8(c); uint8_t hi = f8(c); return lo | ((uint16_t)hi << 8); }

static int32_t sext8(uint8_t v) { return (int32_t)((v ^ 0x80) - 0x80); }

/* Width-aware GPR name; otherwise retain contextual core-SFR names and
 * event-free EXTR-window address formatting. */
static void regname(cur_t *c, uint8_t reg, int byte, char *out, int cap) {
    if ((reg >> 4) == 0xF) {
        if (byte) snprintf(out, cap, "%s", BYTE_REG[reg & 0xF]);
        else snprintf(out, cap, "r%d", reg & 0xF);
        return;
    }
    uint32_t base = c->cpu->extr ? 0xF000u : 0xFE00u;
    uint32_t addr = base + 2u * reg;
    const char *nm = cemu_core_sfr_name(addr);
    if (nm) snprintf(out, cap, "%s", nm);
    else snprintf(out, cap, "0x%04x", addr);
}

/* Python f"{v:#08x}": "0x" + at least 6 hex digits (min field width 8 counts the
 * "0x"), so values needing >6 digits are NOT zero-padded further. */
static void py08x(uint32_t v, char *out, int cap) { snprintf(out, cap, "0x%06x", v); }

/* _reltgt(pc0, rel, extra): target = (pc0+2+extra) + sext(rel)*2, same segment. */
static void reltgt(uint32_t pc0, uint8_t rel, int extra, char *out, int cap) {
    uint16_t nip = (uint16_t)((pc0 + 2 + extra) & 0xFFFF);
    uint16_t tgt = (uint16_t)((nip + sext8(rel) * 2) & 0xFFFF);
    py08x((pc0 & 0xFF0000) | tgt, out, cap);
}

int cpu_disasm(cpu_t *cpu, soc_t *soc, char *buf, int cap) {
    return cpu_disasm_at(cpu, soc, cpu_pc(cpu), buf, cap);
}

int cpu_disasm_at(cpu_t *cpu, soc_t *soc, uint32_t addr, char *buf, int cap) {
    cur_t c = { cpu, soc, (uint8_t)(addr >> 16), (uint16_t)(addr & 0xFFFF), 0 };
    uint32_t pc0 = addr & 0xFFFFFF;
    uint8_t op = f8(&c);
    char r1[24], r2[24];

    /* 32-bit fixed-pattern protected ops. */
    if (op == 0xA5 || op == 0xB5 || op == 0x87 || op == 0x97 || op == 0xB7 || op == 0xA7) {
        uint8_t b1 = f8(&c); uint16_t w2 = f16(&c);
        uint32_t full = op | ((uint32_t)b1 << 8) | ((uint32_t)w2 << 16);
        const char *nm = "?";
        switch (full) {
            case 0xA5A55AA5u: nm = "diswdt"; break; case 0xB5B54AB5u: nm = "einit"; break;
            case 0x87877887u: nm = "idle"; break;   case 0x97976897u: nm = "pwrdn"; break;
            case 0xB7B748B7u: nm = "srst"; break;    case 0xA7A758A7u: nm = "srvwdt"; break;
        }
        snprintf(buf, cap, "%s", nm);
        return c.len;
    }
    if (op == 0xCC) { f8(&c); snprintf(buf, cap, "nop"); return c.len; }
    if (op == 0xCB) { snprintf(buf, cap, "ret"); return c.len; }
    if (op == 0xDB) { snprintf(buf, cap, "rets"); return c.len; }
    if (op == 0xFB) { snprintf(buf, cap, "reti"); return c.len; }

    int hi = op >> 4, lo = op & 0xF;

    if (op == 0xE0) { uint8_t b = f8(&c); snprintf(buf, cap, "mov r%d,#0x%x", b & 0xF, (b >> 4) & 0xF); return c.len; }
    if (op == 0xE1) { uint8_t b = f8(&c); snprintf(buf, cap, "movb %s,#0x%x", BYTE_REG[b & 0xF], (b >> 4) & 0xF); return c.len; }
    if (op == 0xF0) { uint8_t b = f8(&c); snprintf(buf, cap, "mov r%d,r%d", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0xF1) { uint8_t b = f8(&c); snprintf(buf, cap, "movb %s,%s", BYTE_REG[(b >> 4) & 0xF], BYTE_REG[b & 0xF]); return c.len; }
    if (op == 0xE6) { uint8_t reg = f8(&c); uint16_t d = f16(&c); regname(&c, reg, 0, r1, sizeof r1); snprintf(buf, cap, "mov %s,#0x%x", r1, d); return c.len; }
    if (op == 0xE7) { uint8_t reg = f8(&c); uint16_t d = f16(&c) & 0xFF; regname(&c, reg, 1, r1, sizeof r1); snprintf(buf, cap, "movb %s,#0x%x", r1, d); return c.len; }
    if (op == 0xF2) { uint8_t reg = f8(&c); uint16_t m = f16(&c); regname(&c, reg, 0, r1, sizeof r1); snprintf(buf, cap, "mov %s,0x%04x", r1, m); return c.len; }
    if (op == 0xF6) { uint8_t reg = f8(&c); uint16_t m = f16(&c); regname(&c, reg, 0, r1, sizeof r1); snprintf(buf, cap, "mov 0x%04x,%s", m, r1); return c.len; }
    if (op == 0xF3) { uint8_t reg = f8(&c); uint16_t m = f16(&c); regname(&c, reg, 1, r1, sizeof r1); snprintf(buf, cap, "movb %s,0x%04x", r1, m); return c.len; }
    if (op == 0xF7) { uint8_t reg = f8(&c); uint16_t m = f16(&c); regname(&c, reg, 1, r1, sizeof r1); snprintf(buf, cap, "movb 0x%04x,%s", m, r1); return c.len; }

    if (op == 0xC4 || op == 0xD4 || op == 0xE4 || op == 0xF4) {
        uint8_t b = f8(&c); int rwn = (b >> 4) & 0xF, rwm = b & 0xF; uint16_t off = f16(&c);
        if (op == 0xD4)      snprintf(buf, cap, "mov r%d,[r%d+#0x%x]", rwn, rwm, off);
        else if (op == 0xC4) snprintf(buf, cap, "mov [r%d+#0x%x],r%d", rwm, off, rwn);
        else if (op == 0xF4) snprintf(buf, cap, "movb %s,[r%d+#0x%x]", BYTE_REG[rwn], rwm, off);
        else                 snprintf(buf, cap, "movb [r%d+#0x%x],%s", rwm, off, BYTE_REG[rwn]);
        return c.len;
    }
    if (op == 0xA8) { uint8_t b = f8(&c); snprintf(buf, cap, "mov r%d,[r%d]", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0x98) { uint8_t b = f8(&c); snprintf(buf, cap, "mov r%d,[r%d+]", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0xB8) { uint8_t b = f8(&c); snprintf(buf, cap, "mov [r%d],r%d", b & 0xF, (b >> 4) & 0xF); return c.len; }
    if (op == 0x88) { uint8_t b = f8(&c); snprintf(buf, cap, "mov [-r%d],r%d", b & 0xF, (b >> 4) & 0xF); return c.len; }
    if (op == 0xA9) { uint8_t b = f8(&c); snprintf(buf, cap, "movb %s,[r%d]", BYTE_REG[(b >> 4) & 0xF], b & 0xF); return c.len; }
    if (op == 0x99) { uint8_t b = f8(&c); snprintf(buf, cap, "movb %s,[r%d+]", BYTE_REG[(b >> 4) & 0xF], b & 0xF); return c.len; }
    if (op == 0xB9) { uint8_t b = f8(&c); snprintf(buf, cap, "movb [r%d],%s", b & 0xF, BYTE_REG[(b >> 4) & 0xF]); return c.len; }

    if (op == 0xC0) { uint8_t b = f8(&c); snprintf(buf, cap, "movbz r%d,%s", b & 0xF, BYTE_REG[(b >> 4) & 0xF]); return c.len; }
    if (op == 0xD0) { uint8_t b = f8(&c); snprintf(buf, cap, "movbs r%d,%s", b & 0xF, BYTE_REG[(b >> 4) & 0xF]); return c.len; }
    if (op == 0xC2) { uint8_t reg = f8(&c); uint16_t m = f16(&c); regname(&c, reg, 0, r1, sizeof r1); snprintf(buf, cap, "movbz %s,0x%04x", r1, m); return c.len; }
    if (op == 0xC5) { uint8_t reg = f8(&c); uint16_t m = f16(&c); regname(&c, reg, 1, r1, sizeof r1); snprintf(buf, cap, "movbz 0x%04x,%s", m, r1); return c.len; }
    if (op == 0xD2) { uint8_t reg = f8(&c); uint16_t m = f16(&c); regname(&c, reg, 0, r1, sizeof r1); snprintf(buf, cap, "movbs %s,0x%04x", r1, m); return c.len; }
    if (op == 0xD5) { uint8_t reg = f8(&c); uint16_t m = f16(&c); regname(&c, reg, 1, r1, sizeof r1); snprintf(buf, cap, "movbs 0x%04x,%s", m, r1); return c.len; }

    /* undefined CMP store forms 0x44/0x45 -> BTRAP. */
    if (hi == 0x4 && (lo == 0x4 || lo == 0x5)) {
        snprintf(buf, cap, "undefined opcode -> BTRAP vec 0x0028");
        return c.len;
    }

    /* ALU families. */
    static const char *ALU[8] = { "add","addc","sub","subc","cmp","xor","and","or" };
    if (hi <= 0x7 && lo <= 0x9) {
        const char *nm = ALU[hi];
        const char *bs = (op & 1) ? "b" : "";
        int form = lo & 0xE;
        if (form == 0x0) { uint8_t b = f8(&c); regname(&c, 0xF0 | (b >> 4), op & 1, r1, sizeof r1); regname(&c, 0xF0 | (b & 15), op & 1, r2, sizeof r2); snprintf(buf, cap, "%s%s %s,%s", nm, bs, r1, r2); return c.len; }
        if (form == 0x6) { uint8_t reg = f8(&c); uint16_t d = f16(&c); if (op & 1) d &= 0xFF; regname(&c, reg, (op & 1), r1, sizeof r1); snprintf(buf, cap, "%s%s %s,#0x%x", nm, bs, r1, d); return c.len; }
        if (form == 0x8) {
            uint8_t b = f8(&c); int dn = (b >> 4) & 0xF;
            int idx_imm = (b >> 3) & 1, idx_mode = (b >> 2) & 1, rwi = b & 3;
            char label[16];
            if (idx_imm == 0) snprintf(label, sizeof label, "#0x%x", b & 7);
            else snprintf(label, sizeof label, "[r%d%s]", rwi, idx_mode ? "+" : "");
            regname(&c, 0xF0 | dn, op & 1, r1, sizeof r1); snprintf(buf, cap, "%s%s %s,%s", nm, bs, r1, label);
            return c.len;
        }
        /* form 0x2 / 0x4: mem<->reg */
        f8(&c); f16(&c);
        snprintf(buf, cap, "%s%s mem<->reg", nm, bs);
        return c.len;
    }

    if (lo == 0xE) { uint8_t bitoff = f8(&c); snprintf(buf, cap, "bclr 0x%02x.%d", bitoff, hi); return c.len; }
    if (lo == 0xF) { uint8_t bitoff = f8(&c); snprintf(buf, cap, "bset 0x%02x.%d", bitoff, hi); return c.len; }

    if (lo == 0xD) { uint8_t rel = f8(&c); reltgt(pc0, rel, 0, r1, sizeof r1);
                     snprintf(buf, cap, "jmpr cc_%s,%s", CC_NAME[hi], r1); return c.len; }

    /* two-bitoff bit logic. */
    if (op == 0x6A || op == 0x5A || op == 0x7A || op == 0x4A || op == 0x3A || op == 0x2A) {
        uint8_t b1 = f8(&c); uint16_t w = f16(&c);
        const char *nm = op==0x6A?"band":op==0x5A?"bor":op==0x7A?"bxor":op==0x4A?"bmov":op==0x3A?"bmovn":"bcmp";
        int bit_z = (w >> 8) & 0xF, bit_q = (w >> 12) & 0xF;
        snprintf(buf, cap, "%s 0x%02x.%d,0x%02x.%d", nm, w & 0xFF, bit_z, b1, bit_q);
        return c.len;
    }
    if (op == 0x0A || op == 0x1A) {
        uint8_t bitoff = f8(&c); uint16_t w = f16(&c);
        int mask, data;
        if (op == 0x0A) { mask = w & 0xFF; data = (w >> 8) & 0xFF; }
        else            { data = w & 0xFF; mask = (w >> 8) & 0xFF; }
        snprintf(buf, cap, "%s 0x%02x,#0x%x,#0x%x", op == 0x0A ? "bfldl" : "bfldh", bitoff, mask, data);
        return c.len;
    }
    if (op == 0x8A || op == 0x9A || op == 0xAA) {
        uint8_t bitoff = f8(&c); uint16_t w = f16(&c);
        int q = (w >> 12) & 0xF; uint8_t rel = w & 0xFF;
        const char *nm = op==0x8A?"jb":op==0x9A?"jnb":"jbc";
        reltgt(pc0, rel, 2, r1, sizeof r1);
        snprintf(buf, cap, "%s 0x%02x.%d,%s", nm, bitoff, q, r1);
        return c.len;
    }
    if (op == 0xEA || op == 0xCA) { uint8_t b = f8(&c); uint16_t caddr = f16(&c);
        snprintf(buf, cap, "%s cc_%s,0x%04x", op == 0xEA ? "jmpa" : "calla", CC_NAME[(b >> 4) & 0xF], caddr); return c.len; }
    if (op == 0xBB) { uint8_t rel = f8(&c); reltgt(pc0, rel, 0, r1, sizeof r1); snprintf(buf, cap, "callr %s", r1); return c.len; }
    if (op == 0xFA || op == 0xDA) { uint8_t seg = f8(&c); uint16_t caddr = f16(&c);
        char tgt[16]; py08x(((uint32_t)(seg & 0xFF) << 16) | caddr, tgt, sizeof tgt);
        snprintf(buf, cap, "%s 0x%02x,0x%04x -> %s", op == 0xFA ? "jmps" : "calls", seg, caddr, tgt); return c.len; }
    if (op == 0x9C || op == 0xAB) { uint8_t b = f8(&c);
        snprintf(buf, cap, "%s cc_%s,[r%d]", op == 0x9C ? "jmpi" : "calli", CC_NAME[(b >> 4) & 0xF], b & 0xF); return c.len; }

    if (op == 0xD7) { uint8_t b = f8(&c); uint16_t w = f16(&c);
        int mode = (b >> 6) & 3, count = ((b >> 4) & 3) + 1;
        if (mode == 0) snprintf(buf, cap, "exts #0x%x,#%d", w & 0xFF, count);
        else if (mode == 1) snprintf(buf, cap, "extp #0x%x,#%d", w & 0x3FF, count);
        else if (mode == 2) snprintf(buf, cap, "extsr #0x%x,#%d", w & 0xFF, count);
        else snprintf(buf, cap, "extpr #0x%x,#%d", w & 0x3FF, count);
        return c.len;
    }
    if (op == 0xDC) { uint8_t b = f8(&c);
        int mode = (b >> 6) & 3, count = ((b >> 4) & 3) + 1, rwm = b & 0xF;
        const char *nm = mode==0?"exts":mode==1?"extp":mode==2?"extsr":"extpr";
        snprintf(buf, cap, "%s r%d,#%d", nm, rwm, count);
        return c.len;
    }
    if (op == 0xD1) { uint8_t b = f8(&c); int mode = (b >> 6) & 3, count = ((b >> 4) & 3) + 1;
        if (mode == 2) snprintf(buf, cap, "extr #%d", count);
        else snprintf(buf, cap, "atomic #%d", count);
        return c.len;
    }
    if (op == 0x9B) { uint8_t b = f8(&c); int trap7 = (b >> 1) & 0x7F;
        snprintf(buf, cap, "trap #0x%x -> vec 0x%04x", trap7, trap7 * 4); return c.len; }
    if (op == 0xE2) { uint8_t reg = f8(&c); uint16_t caddr = f16(&c); regname(&c, reg, 0, r1, sizeof r1);
        snprintf(buf, cap, "pcall %s,0x%04x", r1, caddr); return c.len; }
    if (op == 0xC6 || op == 0xD6) { uint8_t reg = f8(&c); uint16_t src = f16(&c); regname(&c, reg, 0, r1, sizeof r1);
        snprintf(buf, cap, "scxt %s,#0x%x", r1, src); return c.len; }
    if (op == 0x2B) { uint8_t b = f8(&c); snprintf(buf, cap, "prior r%d,r%d", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0x91) { uint8_t b = f8(&c); snprintf(buf, cap, "cpl r%d", (b >> 4) & 0xF); return c.len; }
    if (op == 0x81) { uint8_t b = f8(&c); snprintf(buf, cap, "neg r%d", (b >> 4) & 0xF); return c.len; }

    /* shifts (rol/ror/shl/shr). imm forms decode cnt from high nibble. */
    if (op == 0x0C || op == 0x1C || op == 0x2C || op == 0x3C ||
        op == 0x4C || op == 0x5C || op == 0x6C || op == 0x7C) {
        int imm = (op == 0x1C || op == 0x3C || op == 0x5C || op == 0x7C);
        const char *kind = (op==0x0C||op==0x1C)?"rol":(op==0x2C||op==0x3C)?"ror":(op==0x4C||op==0x5C)?"shl":"shr";
        uint8_t b = f8(&c);
        int rwm, cnt;
        if (imm) { rwm = b & 0xF; cnt = (b >> 4) & 0xF; }
        else { rwm = (b >> 4) & 0xF; cnt = cemu_cpu_gpr(cpu, b & 0xF) & 0xF; }
        snprintf(buf, cap, "%s r%d,#%d", kind, rwm, cnt & 0xF);
        return c.len;
    }
    if (op == 0xAC || op == 0xBC) {
        uint8_t b = f8(&c); int cnt, dst;
        if (op == 0xAC) { dst = (b >> 4) & 0xF; cnt = cemu_cpu_gpr(cpu, b & 0xF) & 0xF; }
        else            { cnt = (b >> 4) & 0xF; dst = b & 0xF; }
        snprintf(buf, cap, "ashr r%d,#%d", dst, cnt);
        return c.len;
    }
    /* CMPI/CMPD loop ops. */
    if (op == 0x80 || op == 0x82 || op == 0x86 || op == 0x90 || op == 0x92 || op == 0x96 ||
        op == 0xA0 || op == 0xA2 || op == 0xA6 || op == 0xB0 || op == 0xB2 || op == 0xB6) {
        const char *nm = hi==0x8?"cmpi1":hi==0x9?"cmpi2":hi==0xA?"cmpd1":"cmpd2";
        int rwn;
        if (lo == 0x0) { uint8_t b = f8(&c); rwn = b & 0xF; }
        else if (lo == 0x2) { uint8_t reg = f8(&c); f16(&c); rwn = reg & 0xF; }
        else { uint8_t reg = f8(&c); f16(&c); rwn = reg & 0xF; }
        snprintf(buf, cap, "%s r%d", nm, rwn);
        return c.len;
    }
    if (op == 0xA1) { uint8_t b = f8(&c); snprintf(buf, cap, "negb %s", BYTE_REG[(b >> 4) & 0xF]); return c.len; }
    if (op == 0xB1) { uint8_t b = f8(&c); snprintf(buf, cap, "cplb %s", BYTE_REG[(b >> 4) & 0xF]); return c.len; }
    if (op == 0xBA) { uint8_t bitoff = f8(&c); uint16_t w = f16(&c); int q = (w >> 12) & 0xF;
        snprintf(buf, cap, "jnbs 0x%02x.%d", bitoff, q); return c.len; }
    if (op == 0xEB) { uint8_t reg = f8(&c); regname(&c, reg, 0, r1, sizeof r1); snprintf(buf, cap, "retp %s", r1); return c.len; }

    /* MOV/MOVB indirect reg-pair & [Rn],mem. */
    if (op == 0xC8) { uint8_t b = f8(&c); snprintf(buf, cap, "mov [r%d],[r%d]", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0xD8) { uint8_t b = f8(&c); snprintf(buf, cap, "mov [r%d+],[r%d]", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0xE8) { uint8_t b = f8(&c); snprintf(buf, cap, "mov [r%d],[r%d+]", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0xC9) { uint8_t b = f8(&c); snprintf(buf, cap, "movb [r%d],[r%d]", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0xD9) { uint8_t b = f8(&c); snprintf(buf, cap, "movb [r%d+],[r%d]", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0xE9) { uint8_t b = f8(&c); snprintf(buf, cap, "movb [r%d],[r%d+]", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0x84) { uint8_t b = f8(&c); uint16_t m = f16(&c); snprintf(buf, cap, "mov [r%d],0x%04x", b & 0xF, m); return c.len; }
    if (op == 0x94) { uint8_t b = f8(&c); uint16_t m = f16(&c); snprintf(buf, cap, "mov 0x%04x,[r%d]", m, b & 0xF); return c.len; }
    if (op == 0xA4) { uint8_t b = f8(&c); uint16_t m = f16(&c); snprintf(buf, cap, "movb [r%d],0x%04x", b & 0xF, m); return c.len; }
    if (op == 0xB4) { uint8_t b = f8(&c); uint16_t m = f16(&c); snprintf(buf, cap, "movb 0x%04x,[r%d]", m, b & 0xF); return c.len; }
    if (op == 0x89) { uint8_t b = f8(&c); snprintf(buf, cap, "movb [-r%d],%s", b & 0xF, BYTE_REG[(b >> 4) & 0xF]); return c.len; }

    if (op == 0x0B || op == 0x1B) { uint8_t b = f8(&c);
        snprintf(buf, cap, "%s r%d,r%d", op == 0x0B ? "mul" : "mulu", (b >> 4) & 0xF, b & 0xF); return c.len; }
    if (op == 0x4B || op == 0x5B || op == 0x6B || op == 0x7B) {
        uint8_t b = f8(&c); int rwn = (b >> 4) & 0xF;
        if (cemu_cpu_gpr(cpu, rwn) == 0) snprintf(buf, cap, "div r%d (DIV0)", rwn);
        else snprintf(buf, cap, "div r%d", rwn);
        return c.len;
    }
    if (op == 0xEC) { uint8_t reg = f8(&c); regname(&c, reg, 0, r1, sizeof r1); snprintf(buf, cap, "push %s", r1); return c.len; }
    if (op == 0xFC) { uint8_t reg = f8(&c); regname(&c, reg, 0, r1, sizeof r1); snprintf(buf, cap, "pop %s", r1); return c.len; }

    (void)r2;
    snprintf(buf, cap, "?op 0x%02x", op);
    return c.len;
}
