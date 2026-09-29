/* Run summary renderer — see summary.h. A 1:1 mirror of boot.py
 * format_run_summary, including the ANSI color palette and the _LiveProgress
 * in-place repaint. With color=0 the output matches the persisted summary.txt
 * byte-for-byte; with color=1 it carries the same SGR codes Python emits. */
#define _POSIX_C_SOURCE 200809L   /* strdup under -std=c11 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "summary.h"
#include "names.h"

/* ---- color palette (mirror boot.py _STYLES) --------------------------- */
/* style name -> SGR parameter string. Kept in sync with _STYLES. */
static const char *style_sgr(const char *style) {
    if (!strcmp(style, "head")) return "1";    /* bold section header */
    if (!strcmp(style, "dim"))  return "2";    /* de-emphasized labels/units */
    if (!strcmp(style, "ok"))   return "32";   /* green — productive/benign */
    if (!strcmp(style, "warn")) return "33";   /* yellow — polling unmodeled I/O */
    if (!strcmp(style, "bad"))  return "31";   /* red — stuck/could-fire */
    if (!strcmp(style, "cyan")) return "36";   /* addresses/identifiers */
    if (!strcmp(style, "read")) return "92";   /* bright green — read counts */
    if (!strcmp(style, "write"))return "91";   /* bright red — write counts */
    return NULL;
}
/* Verdict -> the color that frames the whole report (mirror _VERDICT_STYLE). */
static const char *verdict_style(const char *status) {
    if (!strcmp(status, "waiting_io") || !strcmp(status, "waiting_dpram") ||
        !strcmp(status, "idle")) return "warn";
    if (!strcmp(status, "spin") || !strcmp(status, "unimplemented") ||
        !strcmp(status, "memory") || !strcmp(status, "error") ||
        !strcmp(status, "serial_exit")) return "bad";
    if (!strcmp(status, "limit") || !strcmp(status, "interrupted")) return "dim";
    if (!strcmp(status, "halted")) return "ok";
    return "head";   /* "running" (live) + fallback */
}

/* A colorizer bound to a run: paint `text` in `style` into `out` (mirror the
 * `c = lambda t,s: _paint(t,s,color)` closure). No-op when color off / unknown. */
typedef struct { int color; } paint_t;
static const char *pc(paint_t *p, char *out, size_t cap, const char *text, const char *style) {
    const char *sgr = p->color ? style_sgr(style) : NULL;
    if (sgr) snprintf(out, cap, "\033[%sm%s\033[0m", sgr, text);
    else snprintf(out, cap, "%s", text);
    return out;
}

/* ---- a growable output buffer ----------------------------------------- */
typedef struct { char *buf; int cap, len; } sb_t;
static void sb_puts(sb_t *b, const char *s) {
    int n = (int)strlen(s);
    if (b->len + n < b->cap) memcpy(b->buf + b->len, s, n);
    else if (b->len < b->cap) memcpy(b->buf + b->len, s, b->cap - b->len);
    b->len += n;
}
static void sb_line(sb_t *b, const char *s) { sb_puts(b, s); sb_puts(b, "\n"); }

/* ---- visible length (ANSI-SGR-stripped; mirror _visible_len) ---------- */
static int visible_len(const char *s) {
    int n = 0;
    for (const char *p = s; *p; ) {
        if (p[0] == '\033' && p[1] == '[') {   /* skip CSI ... m */
            p += 2;
            while (*p && *p != 'm') p++;
            if (*p == 'm') p++;
        } else { n++; p++; }
    }
    return n;
}

/* Python f"{v:#0Nx}" ALWAYS prepends 0x (even for v==0) and counts it in the
 * width, whereas C's "%#0Nx" omits 0x when v==0. */
static void py04x(char *o, size_t c, unsigned v) { snprintf(o, c, "0x%02x", v); }
static void py06x(char *o, size_t c, unsigned v) { snprintf(o, c, "0x%04x", v); }
static void py08x(char *o, size_t c, unsigned long v) { snprintf(o, c, "0x%06lx", v & 0xFFFFFFUL); }

/* Comma-grouped decimal (e.g. 2817823 -> "2,817,823"). */
static void commafmt(char *out, size_t cap, uint64_t v) {
    char tmp[32]; int n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
    int o = 0;
    for (int i = 0; i < n && o < (int)cap - 1; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = tmp[i];
    }
    out[o] = 0;
}

/* Compact duration (mirror _fmt_duration): <60s -> "4.1s"; else "1h2m5s"/"2m5s". */
static void fmt_duration(char *out, size_t cap, double sec) {
    if (sec < 60.0) { snprintf(out, cap, "%.1fs", sec); return; }
    long total = (long)sec, h = total / 3600, rem = total % 3600, m = rem / 60, s = rem % 60;
    if (h) snprintf(out, cap, "%ldh%ldm%lds", h, m, s);
    else snprintf(out, cap, "%ldm%lds", m, s);
}

/* ---- region classification (mirror _region) --------------------------- */
static const char *region_of(uint32_t a) {
    a &= 0xFFFFFF;
    if (ESFR_START <= a && a < ESFR_END) return "esfr";
    if (SFR_START <= a && a < SFR_END) return "sfr";
    if (INTERNAL_IO_START <= a && a < INTERNAL_IO_END) return "internal io";
    if (DPRAM_START <= a && a < DPRAM_END) return "dpram";
    return "mem";
}

/* Style for a cell's symbolic label (mirror _label_style): green if the
 * SFR/ESFR is backed by a real model, else cyan. */
static const char *label_style(soc_t *s, uint32_t a) {
    const char *rg = region_of(a);
    if ((!strcmp(rg, "sfr") || !strcmp(rg, "esfr")) && cemu_soc_sfr_modeled(s, a)) return "ok";
    return "cyan";
}

/* CORE_SFRS are per-instruction housekeeping, not firmware-facing I/O: keep
 * them out of the polled-I/O grids (mirror boot.py's CORE_SFRS filter). */
static int skip_core_sfr_in_io(uint32_t a) {
    switch (a) {
        case 0xFE00: case 0xFE02: case 0xFE04: case 0xFE06:
        case 0xFE10: case 0xFE12: case 0xFE14: case 0xFE16:
        case 0xFF10:
            return 1;
        default:
            return 0;
    }
}

/* Per-cell access token (mirror boot.py _rw): default x<reads>; with
 * show_writes, r<reads> in bright green and w<writes> in bright red, omitting
 * zero sides. */
static const char *rw_token(paint_t *p, char *out, size_t cap,
                            uint64_t reads, uint64_t writes, int show_writes) {
    char n[32], tok[40], painted[96];
    int pos = 0;

    if (!show_writes) {
        commafmt(n, sizeof n, reads);
        snprintf(out, cap, "x%s", n);
        return out;
    }

    out[0] = 0;
    if (reads) {
        commafmt(n, sizeof n, reads);
        snprintf(tok, sizeof tok, "r%s", n);
        pos += snprintf(out + pos, pos < (int)cap ? cap - (size_t)pos : 0, "%s",
                        pc(p, painted, sizeof painted, tok, "read"));
    }
    if (writes) {
        commafmt(n, sizeof n, writes);
        snprintf(tok, sizeof tok, "w%s", n);
        pos += snprintf(out + pos, pos < (int)cap ? cap - (size_t)pos : 0, "%s%s",
                        pos ? " " : "",
                        pc(p, painted, sizeof painted, tok, "write"));
    }
    return out;
}

/* ---- _grid: lay cells in `columns`, aligned by VISIBLE width; row/col major */
static void grid(sb_t *b, char **cells, int ncells, int columns,
                 const char *indent, int column_major) {
    if (ncells == 0) return;
    int rows = (ncells + columns - 1) / columns;
    int *w = calloc(columns, sizeof(int));
    for (int col = 0; col < columns; col++)
        for (int r = 0; r < rows; r++) {
            int i = column_major ? col * rows + r : r * columns + col;
            if (i < ncells) { int l = visible_len(cells[i]); if (l > w[col]) w[col] = l; }
        }
    for (int r = 0; r < rows; r++) {
        char line[2048]; int p = 0;
        p += snprintf(line + p, sizeof line - p, "%s", indent);
        for (int col = 0; col < columns; col++) {
            int i = column_major ? col * rows + r : r * columns + col;
            if (i >= ncells) continue;
            int has_more = 0;
            for (int cc = col + 1; cc < columns; cc++) {
                int j = column_major ? cc * rows + r : r * columns + cc;
                if (j < ncells) { has_more = 1; break; }
            }
            int cl = visible_len(cells[i]);
            if (col > 0) p += snprintf(line + p, sizeof line - p, "  ");
            p += snprintf(line + p, sizeof line - p, "%s", cells[i]);
            if (has_more) for (int k = 0; k < w[col] - cl && p < (int)sizeof line - 1; k++) line[p++] = ' ';
        }
        /* rstrip trailing spaces (visible-only padding leaves no ANSI at EOL) */
        while (p > 0 && line[p - 1] == ' ') p--;
        line[p] = 0;
        sb_line(b, line);
    }
    free(w);
}

/* A dynamic cell list. */
typedef struct { char **v; int n, cap; } cells_t;
static void cells_add(cells_t *c, const char *fmt, ...) {
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 64; c->v = realloc(c->v, c->cap * sizeof(char *)); }
    char tmp[256]; va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    c->v[c->n++] = strdup(tmp);
}
static void cells_free(cells_t *c) { for (int i = 0; i < c->n; i++) free(c->v[i]); free(c->v); c->v = NULL; c->n = c->cap = 0; }

/* qsort comparator for (seg,count) sorted by count desc then seg asc. */
typedef struct { uint32_t key; uint64_t cnt; } kc_t;
static int kc_cnt_desc(const void *a, const void *b) {
    const kc_t *x = a, *y = b;
    if (x->cnt != y->cnt) return x->cnt < y->cnt ? 1 : -1;
    return x->key < y->key ? -1 : 1;
}
static int kc_key_asc(const void *a, const void *b) {
    const kc_t *x = a, *y = b;
    return x->key < y->key ? -1 : (x->key > y->key ? 1 : 0);
}

/* Symbolic label for a register or named internal-memory cell. */
static void cell_label(soc_t *s, char *out, size_t cap, uint32_t a, int named) {
    char h[16]; py06x(h, sizeof h, a);
    if (named) { const char *nm = cemu_soc_sfr_name(s, a); snprintf(out, cap, "%s@%s", nm ? nm : "?", h); return; }
    const char *region = region_of(a);
    const char *dp = !strcmp(region, "dpram") ? cemu_soc_dpram_name(s, a)
                   : !strcmp(region, "internal io")
                         ? cemu_soc_internal_io_name(s, a) : NULL;
    if (dp) snprintf(out, cap, "%s@%s", dp, h);
    else snprintf(out, cap, "%s", h);
}

int format_run_summary(char *out, int cap, const run_result_t *r,
                       soc_t *soc, cpu_t *cpu, const monitor_t *mon,
                       int color, int show_writes) {
    sb_t b = { out, cap, 0 };
    paint_t P = { color };
    char n1[32], n2[32];
    /* scratch paint buffers */
    char t1[128], t2[128], t3[128], t4[128], t5[192];

    /* ---- progress ------------------------------------------------------ */
    sb_line(&b, "");
    sb_line(&b, pc(&P, t1, sizeof t1, "progress:", "head"));
    /* traps detail: sorted by (-count, vector); the whole "[...]" is dim. */
    char traps_detail[512]; traps_detail[0] = 0;
    {
        kc_t tc[256]; int nt = 0;
        for (int t = 0; t < 256; t++) if (cpu->trap_counts[t]) { tc[nt].key = t; tc[nt].cnt = cpu->trap_counts[t]; nt++; }
        qsort(tc, nt, sizeof(kc_t), kc_cnt_desc);
        if (nt) {
            char inner[400]; int p = 0;
            inner[0] = 0;
            for (int i = 0; i < nt; i++) {
                char tn[24]; cemu_soc_trap_name((int)tc[i].key, tn, sizeof tn);
                commafmt(n1, sizeof n1, tc[i].cnt);
                char th[16]; py04x(th, sizeof th, (unsigned)tc[i].key);
                p += snprintf(inner + p, sizeof inner - p, "%s%s(%s)x%s",
                              i ? "  " : "", tn, th, n1);
            }
            char bracketed[440]; snprintf(bracketed, sizeof bracketed, "[%s]", inner);
            char painted[480]; pc(&P, painted, sizeof painted, bracketed, "dim");
            snprintf(traps_detail, sizeof traps_detail, " %s", painted);
        }
    }
    {
        char line[900];
        char bt[32], bb[32], irq[32], tr[32];
        commafmt(bt, sizeof bt, monitor_branch_targets(mon));
        commafmt(bb, sizeof bb, monitor_basic_blocks(mon));
        commafmt(irq, sizeof irq, cpu->interrupts_delivered);
        commafmt(tr, sizeof tr, cpu->traps_taken);
        snprintf(line, sizeof line, "  %s: %s  %s: %s  %s: %s  %s: %s%s",
                 pc(&P, t1, sizeof t1, "branch targets", "dim"), bt,
                 pc(&P, t2, sizeof t2, "basic blocks", "dim"), bb,
                 pc(&P, t3, sizeof t3, "interrupts", "dim"), irq,
                 pc(&P, t4, sizeof t4, "traps", "dim"), tr, traps_detail);
        sb_line(&b, line);
    }
    /* exec seg: sorted by count desc, columns=8 row-major. Each cell: cyan seg + " x<n>". */
    {
        kc_t seg[256]; int ns = 0;
        for (int s = 0; s < 256; s++) if (mon->seg_exec[s]) { seg[ns].key = s; seg[ns].cnt = mon->seg_exec[s]; ns++; }
        if (ns) {
            qsort(seg, ns, sizeof(kc_t), kc_cnt_desc);
            snprintf(t5, sizeof t5, "  %s: %s", pc(&P, t1, sizeof t1, "exec seg", "dim"),
                     pc(&P, t2, sizeof t2, "(instrs per seg)", "dim"));
            sb_line(&b, t5);
            cells_t c = {0};
            for (int i = 0; i < ns; i++) {
                commafmt(n1, sizeof n1, seg[i].cnt); char sh[16]; py04x(sh, sizeof sh, (unsigned)seg[i].key);
                cells_add(&c, "%s x%s", pc(&P, t1, sizeof t1, sh, "cyan"), n1);
            }
            grid(&b, c.v, c.n, 8, "    ", 0);
            cells_free(&c);
        }
    }
    /* hot blocks: top-6 by hits, columns=3 row-major. */
    {
        uint32_t ha[6]; uint64_t hh[6];
        int nh = monitor_hot_blocks(mon, 6, ha, hh);
        if (nh) {
            snprintf(t5, sizeof t5, "  %s:", pc(&P, t1, sizeof t1, "hot blocks", "dim"));
            sb_line(&b, t5);
            cells_t c = {0};
            for (int i = 0; i < nh; i++) {
                commafmt(n1, sizeof n1, hh[i]); char ah[16]; py08x(ah, sizeof ah, ha[i]);
                cells_add(&c, "%s x%s", pc(&P, t1, sizeof t1, ah, "cyan"), n1);
            }
            grid(&b, c.v, c.n, 3, "    ", 0);
            cells_free(&c);
        }
    }
    /* stack extent */
    if (mon->sp_min >= 0) {
        char line[256]; commafmt(n1, sizeof n1, (uint64_t)(mon->sp_max - mon->sp_min));
        char smin[16], smax[16]; py06x(smin, sizeof smin, (unsigned)mon->sp_min); py06x(smax, sizeof smax, (unsigned)mon->sp_max);
        char range[48]; snprintf(range, sizeof range, "%s..%s", smin, smax);
        char deep[48]; snprintf(deep, sizeof deep, "(%s bytes deep)", n1);
        snprintf(line, sizeof line, "  %s: %s %s %s",
                 pc(&P, t1, sizeof t1, "stack", "dim"),
                 pc(&P, t2, sizeof t2, "sp", "dim"),
                 pc(&P, t3, sizeof t3, range, "cyan"),
                 pc(&P, t4, sizeof t4, deep, "dim"));
        sb_line(&b, line);
    }
    /* memory metrics */
    uint64_t sfr_reads = 0, esfr_reads = 0, sfr_writes = 0, esfr_writes = 0;
    for (uint32_t i = 0; i < SFR_WORDS; i++) {
        uint32_t a = SFR_BASE + 2 * i;
        const char *rg = region_of(a);
        if (!strcmp(rg, "sfr")) { sfr_reads += soc->memory.sfr_read_counts[i]; sfr_writes += soc->memory.sfr_write_counts[i]; }
        else if (!strcmp(rg, "esfr")) { esfr_reads += soc->memory.sfr_read_counts[i]; esfr_writes += soc->memory.sfr_write_counts[i]; }
    }
    uint64_t internal_io_reads_t = 0, internal_io_writes_t = 0;
    for (uint32_t i = 0; i < INTERNAL_IO_WORDS; i++) {
        internal_io_reads_t += soc->memory.internal_io_read_counts[i];
        internal_io_writes_t += soc->memory.internal_io_write_counts[i];
    }
    uint64_t dpram_reads_t = 0, dpram_writes_t = 0;
    for (uint32_t i = 0; i < DPRAM_WORDS; i++) { dpram_reads_t += soc->memory.dpram_read_counts[i]; dpram_writes_t += soc->memory.dpram_write_counts[i]; }
    uint64_t internal_writes = internal_io_writes_t + dpram_writes_t;
    uint64_t mem_writes_only = soc->memory.mem_write_seq > internal_writes
                             ? soc->memory.mem_write_seq - internal_writes : 0;
    {
        char line[400]; commafmt(n1, sizeof n1, mem_writes_only); commafmt(n2, sizeof n2, soc->memory.mem_write_footprint);
        snprintf(line, sizeof line, "  %s: %s %s  %s %s %s",
                 pc(&P, t1, sizeof t1, "mem", "dim"),
                 pc(&P, t2, sizeof t2, "writes", "dim"), n1,
                 pc(&P, t3, sizeof t3, "blocks", "dim"), n2,
                 pc(&P, t4, sizeof t4, "(distinct 256B blocks — real coverage)", "dim"));
        sb_line(&b, line);
    }
    /* write seg: sorted by seg asc, 0x00 minus internal writes. */
    {
        kc_t seg[256]; int ns = 0;
        for (int s = 0; s < 256; s++) if (soc->memory.mem_write_segs[s]) { seg[ns].key = s; seg[ns].cnt = soc->memory.mem_write_segs[s]; ns++; }
        if (ns) {
            qsort(seg, ns, sizeof(kc_t), kc_key_asc);
            snprintf(t5, sizeof t5, "  %s: %s",
                     pc(&P, t1, sizeof t1, "write seg", "dim"),
                     pc(&P, t2, sizeof t2, "(writes per seg; 0x00 without internal I/O/DPRAM/SFR/ESFR)", "dim"));
            sb_line(&b, t5);
            cells_t c = {0};
            for (int i = 0; i < ns; i++) {
                uint64_t v = seg[i].cnt;
                if (seg[i].key == 0) v = v > internal_writes ? v - internal_writes : 0;
                if (!v) continue;
                commafmt(n1, sizeof n1, v); char sh[16]; py04x(sh, sizeof sh, (unsigned)seg[i].key);
                cells_add(&c, "%s x%s", pc(&P, t1, sizeof t1, sh, "cyan"), n1);
            }
            grid(&b, c.v, c.n, 8, "    ", 0);
            cells_free(&c);
        }
    }
    {
        char line[256];
        commafmt(n1, sizeof n1, internal_io_reads_t);
        commafmt(n2, sizeof n2, internal_io_writes_t);
        snprintf(line, sizeof line, "  %s: %s %s  %s %s",
                 pc(&P, t1, sizeof t1, "internal io", "dim"),
                 pc(&P, t2, sizeof t2, "reads", "dim"), n1,
                 pc(&P, t3, sizeof t3, "writes", "dim"), n2);
        sb_line(&b, line);
        commafmt(n1, sizeof n1, dpram_reads_t); commafmt(n2, sizeof n2, dpram_writes_t);
        snprintf(line, sizeof line, "  %s: %s %s  %s %s",
                 pc(&P, t1, sizeof t1, "dpram", "dim"), pc(&P, t2, sizeof t2, "reads", "dim"), n1,
                 pc(&P, t3, sizeof t3, "writes", "dim"), n2);
        sb_line(&b, line);
        commafmt(n1, sizeof n1, sfr_reads); commafmt(n2, sizeof n2, sfr_writes);
        snprintf(line, sizeof line, "  %s: %s %s  %s %s",
                 pc(&P, t1, sizeof t1, "sfr", "dim"), pc(&P, t2, sizeof t2, "reads", "dim"), n1,
                 pc(&P, t3, sizeof t3, "writes", "dim"), n2);
        sb_line(&b, line);
        commafmt(n1, sizeof n1, esfr_reads); commafmt(n2, sizeof n2, esfr_writes);
        snprintf(line, sizeof line, "  %s: %s %s  %s %s",
                 pc(&P, t1, sizeof t1, "esfr", "dim"), pc(&P, t2, sizeof t2, "reads", "dim"), n1,
                 pc(&P, t3, sizeof t3, "writes", "dim"), n2);
        sb_line(&b, line);
    }

    /* ---- absorbed effects (CPU protected ops; flash effects not tracked) */
    static const char *EFFECT[6] = {
        "disable watchdog (watchdog not modeled)",
        "end-of-init / SYSCON locked / RSTOUT high",
        "CPU powered down until interrupt (modeled as idle+wake)",
        "power-down until external reset (modeled as idle)",
        "software reset (re-seeds core, vectors to 0:0000)",
        "service watchdog (watchdog not modeled)",
    };
    {
        int order[6], no = 0;
        for (int i = 0; i < 6; i++) if (cpu->protected_ops[i]) order[no++] = i;
        for (int i = 1; i < no; i++) { int k = order[i], j = i - 1;
            while (j >= 0 && strcmp(cemu_cpu_protected_op_name(order[j]), cemu_cpu_protected_op_name(k)) > 0) { order[j+1] = order[j]; j--; }
            order[j+1] = k; }
        if (no) {
            sb_line(&b, "");
            sb_line(&b, pc(&P, t1, sizeof t1, "absorbed effects (executed but not fully modeled)", "head"));
            for (int i = 0; i < no; i++) {
                char line[400]; commafmt(n1, sizeof n1, cpu->protected_ops[order[i]]);
                snprintf(line, sizeof line, "  %s x%s  %s",
                         pc(&P, t1, sizeof t1, cemu_cpu_protected_op_name(order[i]), "cyan"), n1,
                         pc(&P, t2, sizeof t2, EFFECT[order[i]], "dim"));
                sb_line(&b, line);
            }
        }
    }

    /* ---- polled I/O grids (dpram / sfr / esfr / mem) ------------------- */
    {
        const char *io_header = r->have_loop
            ? "polled I/O (cumulative; loop never advanced by the model)"
            : (show_writes
                ? "polled I/O (cumulative read/write counts)"
                : "polled I/O (cumulative read counts)");
        cells_t io = {0}, dp = {0}, sf = {0}, es = {0}, me = {0};
        for (uint32_t i = 0; i < INTERNAL_IO_WORDS; i++) {
            uint64_t rd = soc->memory.internal_io_read_counts[i];
            uint64_t wr = soc->memory.internal_io_write_counts[i];
            if (!(show_writes ? (rd || wr) : rd)) continue;
            uint32_t a = INTERNAL_IO_START + 2 * i;
            char lbl[48], counts[128];
            cell_label(soc, lbl, sizeof lbl, a, 0);
            rw_token(&P, counts, sizeof counts, rd, wr, show_writes);
            cells_add(&io, "%s %s",
                      pc(&P, t1, sizeof t1, lbl, label_style(soc, a)),
                      counts);
        }
        for (uint32_t i = 0; i < DPRAM_WORDS; i++) {
            uint64_t rd = soc->memory.dpram_read_counts[i];
            uint64_t wr = soc->memory.dpram_write_counts[i];
            if (!(show_writes ? (rd || wr) : rd)) continue;
            uint32_t a = DPRAM_START + 2 * i;
            char lbl[48], counts[128];
            cell_label(soc, lbl, sizeof lbl, a, 0);
            rw_token(&P, counts, sizeof counts, rd, wr, show_writes);
            cells_add(&dp, "%s %s", pc(&P, t1, sizeof t1, lbl, label_style(soc, a)), counts);
        }
        for (uint32_t i = 0; i < SFR_WORDS; i++) {
            uint64_t rd = soc->memory.sfr_read_counts[i];
            uint64_t wr = soc->memory.sfr_write_counts[i];
            if (!(show_writes ? (rd || wr) : rd)) continue;
            uint32_t a = SFR_BASE + 2 * i;
            const char *rg = region_of(a);
            if (skip_core_sfr_in_io(a)) continue;
            char lbl[48], counts[128], cell[256];
            cell_label(soc, lbl, sizeof lbl, a, 1);
            rw_token(&P, counts, sizeof counts, rd, wr, show_writes);
            snprintf(cell, sizeof cell, "%s %s", pc(&P, t1, sizeof t1, lbl, label_style(soc, a)), counts);
            if (!strcmp(rg, "sfr")) cells_add(&sf, "%s", cell);
            else if (!strcmp(rg, "esfr")) cells_add(&es, "%s", cell);
            else cells_add(&me, "%s", cell);
        }
        if (io.n || dp.n || sf.n || es.n || me.n) {
            sb_line(&b, "");
            sb_line(&b, pc(&P, t1, sizeof t1, io_header, "head"));
            struct { const char *name; cells_t *c; } regs[5] = {
                {"internal io", &io}, {"dpram", &dp}, {"sfr", &sf},
                {"esfr", &es}, {"mem", &me} };
            for (int k = 0; k < 5; k++) {
                if (!regs[k].c->n) continue;
                char h[64]; snprintf(h, sizeof h, "  %s:", pc(&P, t1, sizeof t1, regs[k].name, "dim"));
                sb_line(&b, h);
                grid(&b, regs[k].c->v, regs[k].c->n, show_writes ? 6 : 8, "    ", 1);
            }
        }
        cells_free(&io); cells_free(&dp); cells_free(&sf);
        cells_free(&es); cells_free(&me);
    }

    /* ---- XBUS windows -------------------------------------------------- */
    {
        char xb[16][512]; int nxb = 0;
        for (int idx = 1; idx <= 6; idx++) {
            uint64_t hits = soc->memory.xbus_configured_access[idx & 7];
            long start, end; int active, configured; char reason[32];
            cemu_memory_controller_xbus_decode(&soc->memory, idx, &start, &end, &active, &configured, reason, sizeof reason);
            if (!configured) continue;
            char rng[48];
            if (start >= 0) { char s0[16], e0[16]; py08x(s0, sizeof s0, (unsigned long)start); py08x(e0, sizeof e0, (unsigned long)end);
                snprintf(rng, sizeof rng, "%s..%s", s0, e0); }
            else snprintf(rng, sizeof rng, "reserved");
            char state[96];
            if (active) pc(&P, state, sizeof state, "active", "ok");
            else { char cfg[64]; snprintf(cfg, sizeof cfg, "configured (%s)", reason);
                   pc(&P, state, sizeof state, cfg, "dim"); }
            commafmt(n1, sizeof n1, hits);
            char wlbl[16]; snprintf(wlbl, sizeof wlbl, "win%d", idx);
            snprintf(xb[nxb++], 512, "  %s: %s  %s  %s %s",
                     pc(&P, t1, sizeof t1, wlbl, "dim"),
                     pc(&P, t2, sizeof t2, rng, "cyan"), state, n1,
                     pc(&P, t3, sizeof t3, "accesses", "dim"));
        }
        uint64_t idr, str, dbr;
        cemu_soc_xbus_unknown1_counts(soc, &idr, &str, &dbr);
        int handshake = idr || str || dbr;
        if (nxb || handshake) {
            sb_line(&b, "");
            sb_line(&b, pc(&P, t1, sizeof t1, "XBUS peripheral windows", "head"));
            for (int i = 0; i < nxb; i++) sb_line(&b, xb[i]);
            if (handshake) {
                char line[400]; char a[32], c2[32], c3[32];
                commafmt(a, sizeof a, idr); commafmt(c2, sizeof c2, str); commafmt(c3, sizeof c3, dbr);
                snprintf(line, sizeof line, "  %s: %s %s  %s %s  %s %s %s",
                         pc(&P, t1, sizeof t1, "xbus-unknown-1", "dim"),
                         pc(&P, t2, sizeof t2, "id_reads", "dim"), a,
                         pc(&P, t3, sizeof t3, "status_reads", "dim"), c2,
                         pc(&P, t4, sizeof t4, "doorbell_rings", "dim"), c3,
                         pc(&P, t5, sizeof t5, "(minimal handshake)", "dim"));
                sb_line(&b, line);
            }
        }
    }

    /* ---- External bus windows ----------------------------------------- */
    {
        char eb[8][512]; int neb = 0;
        memory_bus_window_info_t w;
        if (cemu_memory_controller_bus_window_decode(&soc->memory, MEMORY_BUS_WINDOW_BUSCON0, 0, &w) && w.configured) {
            char rng[48]; char s0[16], e0[16];
            py08x(s0, sizeof s0, (unsigned long)w.start); py08x(e0, sizeof e0, (unsigned long)w.end);
            snprintf(rng, sizeof rng, "%s..%s", s0, e0);
            char state[96];
            if (w.active) pc(&P, state, sizeof state, "active", "ok");
            else { char cfg[64]; snprintf(cfg, sizeof cfg, "configured (%s)", w.reason); pc(&P, state, sizeof state, cfg, "dim"); }
            commafmt(n1, sizeof n1, soc->memory.buscon0_access);
            snprintf(eb[neb++], 512, "  %s: %s  %s  %s %s",
                     pc(&P, t1, sizeof t1, "BUSCON0", "dim"),
                     pc(&P, t2, sizeof t2, rng, "cyan"), state, n1,
                     pc(&P, t3, sizeof t3, "fallback accesses", "dim"));
        }
        for (int idx = 1; idx <= 4; idx++) {
            if (!cemu_memory_controller_bus_window_decode(&soc->memory, MEMORY_BUS_WINDOW_ADDRSEL, idx, &w) || !w.configured) continue;
            char rng[48];
            if (w.start >= 0) { char s0[16], e0[16]; py08x(s0, sizeof s0, (unsigned long)w.start); py08x(e0, sizeof e0, (unsigned long)w.end);
                snprintf(rng, sizeof rng, "%s..%s", s0, e0); }
            else snprintf(rng, sizeof rng, "reserved");
            char state[96];
            if (w.active) pc(&P, state, sizeof state, "active", "ok");
            else { char cfg[64]; snprintf(cfg, sizeof cfg, "configured (%s)", w.reason); pc(&P, state, sizeof state, cfg, "dim"); }
            commafmt(n1, sizeof n1, soc->memory.addrsel_configured_access[idx]);
            char wlbl[16]; snprintf(wlbl, sizeof wlbl, "ADDRSEL%d", idx);
            snprintf(eb[neb++], 512, "  %s: %s  %s  %s %s",
                     pc(&P, t1, sizeof t1, wlbl, "dim"),
                     pc(&P, t2, sizeof t2, rng, "cyan"), state, n1,
                     pc(&P, t3, sizeof t3, "accesses", "dim"));
        }
        if (neb) {
            sb_line(&b, "");
            sb_line(&b, pc(&P, t1, sizeof t1, "external bus windows", "head"));
            for (int i = 0; i < neb; i++) sb_line(&b, eb[i]);
        }
    }

    /* ---- interrupts ---------------------------------------------------- */
    {
        armed_ic_t armed[MAX_IC]; int na = cemu_soc_armed_interrupts(soc, 0, armed, MAX_IC);
        if (na) {
            uint16_t psw = cemu_memory_controller_peek16(&soc->memory, 0xFF10);
            int cpu_ilvl = r->have_loop ? r->loop.cpu_ilvl : (psw >> 12) & 0xF;
            int cpu_ien = r->have_loop ? r->loop.cpu_ien : (psw >> 11) & 1;
            int fires_any = 0;
            for (int i = 0; i < na; i++) if (cpu_ien && armed[i].ilvl > cpu_ilvl) fires_any = 1;
            char gate[128];
            if (fires_any) pc(&P, gate, sizeof gate, "one+ could FIRE — try delivering it", "bad");
            else { char g[80]; snprintf(g, sizeof g, "none can fire (all ilvl ≤ %d)", cpu_ilvl);
                   pc(&P, gate, sizeof gate, g, "dim"); }
            char line[512];
            sb_line(&b, "");
            snprintf(line, sizeof line, "%s  %s %s %d %s %d  [%s]",
                     pc(&P, t1, sizeof t1, "interrupts", "head"),
                     pc(&P, t2, sizeof t2, "CPU", "dim"),
                     pc(&P, t3, sizeof t3, "ILVL", "dim"), cpu_ilvl,
                     pc(&P, t4, sizeof t4, "IEN", "dim"), cpu_ien, gate);
            sb_line(&b, line);
            for (int i = 0; i < na; i++) {
                char tn[16]; if (armed[i].has_trap) py04x(tn, sizeof tn, armed[i].trap); else snprintf(tn, sizeof tn, "  ?");
                char vec[128]; cemu_soc_describe_vector(soc, armed[i].has_trap ? armed[i].trap : -1, vec, sizeof vec);
                int fires = armed[i].ir && cpu_ien && armed[i].ilvl > cpu_ilvl;
                /* marker (bad ▶ / space), name (bad if fires else cyan), padded to 8 */
                char marker[32]; pc(&P, marker, sizeof marker, fires ? "▶" : " ", "bad");
                char namepad[16]; snprintf(namepad, sizeof namepad, "%-8s", armed[i].name);
                char name[64]; pc(&P, name, sizeof name, namepad, fires ? "bad" : "cyan");
                snprintf(line, sizeof line, "  %s %s %s %-2d %s %d %s %s %s %s",
                         marker, name,
                         pc(&P, t1, sizeof t1, "ilvl", "dim"), armed[i].ilvl,
                         pc(&P, t2, sizeof t2, "ir", "dim"), armed[i].ir,
                         pc(&P, t3, sizeof t3, "trap", "dim"), tn,
                         pc(&P, t4, sizeof t4, "→", "dim"), vec);
                sb_line(&b, line);
            }
        }
    }

    /* ---- serial console ------------------------------------------------ */
    if (soc->serial_tx_len && !r->serial_already_printed) {
        char hdr[96]; snprintf(hdr, sizeof hdr, "serial console output (%zu bytes on ASC0)", soc->serial_tx_len);
        sb_line(&b, "");
        sb_line(&b, pc(&P, t1, sizeof t1, hdr, "ok"));
        char pr[4096]; int p = 0;
        for (size_t i = 0; i < soc->serial_tx_len && p < (int)sizeof pr - 5; i++) {
            uint8_t ch = soc->serial_tx[i];
            if (ch >= 0x20 && ch < 0x7F) pr[p++] = (char)ch;
            else p += snprintf(pr + p, sizeof pr - p, "\\x%02x", ch);
        }
        pr[p] = 0;
        for (int i = 0; i < p; i += 72) {
            char seg[80]; snprintf(seg, sizeof seg, "%.72s", pr + i);
            char line[160]; snprintf(line, sizeof line, "  %s", pc(&P, t1, sizeof t1, seg, "ok"));
            sb_line(&b, line);
        }
    }

    /* ---- halt block ---------------------------------------------------- */
    sb_line(&b, "");
    if (r->elapsed_s > 0) {
        char dur[32]; fmt_duration(dur, sizeof dur, r->elapsed_s);
        double rate = r->steps / r->elapsed_s;
        char line[256]; commafmt(n1, sizeof n1, (uint64_t)(rate + 0.5));
        char rate_s[48]; snprintf(rate_s, sizeof rate_s, "(%s instr/s)", n1);
        snprintf(line, sizeof line, "%s: %s %s",
                 pc(&P, t1, sizeof t1, "time", "dim"),
                 pc(&P, t2, sizeof t2, dur, "warn"),
                 pc(&P, t3, sizeof t3, rate_s, "dim"));
        sb_line(&b, line);
    }
    {
        char line[300]; commafmt(n1, sizeof n1, r->steps);
        char pch[16]; py08x(pch, sizeof pch, r->pc);
        char pcv[64]; pc(&P, pcv, sizeof pcv, pch, "cyan");   /* pc value is cyan */
        snprintf(line, sizeof line, "%s: %s  %s: %s  %s: %s",
                 pc(&P, t1, sizeof t1, "status", "dim"),
                 pc(&P, t2, sizeof t2, r->status, verdict_style(r->status)),
                 pc(&P, t3, sizeof t3, "steps", "dim"),
                 pc(&P, t4, sizeof t4, n1, "ok"),
                 pc(&P, t5, sizeof t5, "pc", "dim"), pcv);
        sb_line(&b, line);
    }
    if (r->reason && r->reason[0]) {
        char line[600];
        snprintf(line, sizeof line, "%s: %s", pc(&P, t1, sizeof t1, "reason", "dim"), r->reason);
        sb_line(&b, line);
    }
    if (b.len < b.cap) b.buf[b.len] = 0; else if (b.cap > 0) b.buf[b.cap - 1] = 0;
    return b.len;
}
