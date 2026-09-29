/* Interactive debugger engine — see debugger.h. C port of debugger.py +
 * the tools/debug.py session front-end. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "debugger.h"
#include "keypad.h"
#include "disasm.h"

/* Core SFR addresses used for register inspection (C166S). */
#define A_SP  0xFE12u
#define A_CP  0xFE10u
#define A_PSW 0xFF10u
#define DBG_SERIAL_RX_MAX 240u
static const uint32_t A_DPP[4] = { 0xFE00u, 0xFE02u, 0xFE04u, 0xFE06u };


/* Hub consumers for CPU/SoC events (installed on attach). */
static void dbg_flow_call(void *ctx, uint32_t caller_pc, uint32_t target_pc, const char *kind);
static void dbg_flow_return(void *ctx, const char *kind);
static void dbg_on_access(void *ctx, const bus_transaction_t *txn);
static void dbg_on_event(void *ctx, const cemu_event_t *event);
static void ensure_monitor(debugger_t *d);

/* ---- attach / detach --------------------------------------------------- */
static void watch_baselines(debugger_t *d, int after);

void debugger_init(debugger_t *d, cpu_t *cpu, soc_t *soc, int capture_disasm) {
    memset(d, 0, sizeof *d);
    d->cpu = cpu;
    d->soc = soc;
    d->parked_pc = -1;
    d->capture_disasm = capture_disasm;
    d->detect_loops = 0;   /* monitoring is opt-in */

    d->bus_subscription = cemu_event_subscribe(
        &soc->instrumentation, CEMU_EVENT_BUS, dbg_on_event, d);
    d->control_subscription = cemu_event_subscribe(
        &soc->instrumentation, CEMU_EVENT_CONTROL, dbg_on_event, d);
}

void debugger_set_after_step(debugger_t *d, void (*fn)(void *ctx), void *ctx) {
    d->after_step = fn;
    d->after_step_ctx = ctx;
}

void debugger_set_batch_idle(debugger_t *d, int enabled) {
    d->batch_idle = enabled ? 1 : 0;
}

void debugger_set_extension(
        debugger_t *d, debugger_extension_command_fn command,
        debugger_extension_help_fn help, debugger_extension_restore_fn restore,
        void *opaque) {
    d->extension_command = command;
    d->extension_help = help;
    d->extension_restore = restore;
    d->extension_opaque = opaque;
}

void debugger_request_external_stop(
        debugger_t *d, const char *kind, uint32_t pc, uint64_t icount,
        const char *reason) {
    if (!d || d->have_watch_hit) return;
    memset(&d->watch_hit, 0, sizeof d->watch_hit);
    d->watch_hit.kind = kind ? kind : "external";
    d->watch_hit.pc = pc;
    d->watch_hit.icount = icount;
    snprintf(d->watch_hit.reason, sizeof d->watch_hit.reason, "%s",
             reason ? reason : "external debugger stop");
    d->have_watch_hit = 1;
}

void debugger_set_monitor(debugger_t *d, int enabled) {
    if (d->mon) {
        monitor_free(d->mon);
        free(d->mon);
        d->mon = NULL;
    }
    d->mon_ready = 0;
    d->detect_loops = enabled ? 1 : 0;
    cemu_soc_disable_stats(d->soc);

    if (d->detect_loops)
        ensure_monitor(d);
}

int debugger_monitor_enabled(const debugger_t *d) {
    return d->detect_loops;
}

void debugger_detach(debugger_t *d) {
    if (d->bus_subscription)
        cemu_event_unsubscribe(&d->soc->instrumentation, d->bus_subscription);
    if (d->control_subscription)
        cemu_event_unsubscribe(&d->soc->instrumentation, d->control_subscription);
    d->bus_subscription = d->control_subscription = 0;
}

int debugger_free(debugger_t *d) {
    if (d->mon) { monitor_free(d->mon); free(d->mon); d->mon = NULL; }
    d->mon_ready = 0;
    debugger_clear_watches(d);
    free(d->checkpoint.ram);
    free(d->checkpoint.present);
    free(d->checkpoint.lm);
    for (int i = 0; i < MAX_EXTERNAL_RAM_DEVICES; i++)
        free(d->checkpoint.external_ram[i]);
    free(d->checkpoint.serial_tx);
    free(d->checkpoint.serial_rx);
    for (int i = 0; i < MAX_FLASH_CHIPS; i++)
        cemu_flash_state_free(&d->checkpoint.flash[i]);
    d->checkpoint.ram = d->checkpoint.present = d->checkpoint.lm =
        d->checkpoint.serial_tx = d->checkpoint.serial_rx = NULL;
    d->checkpoint.valid = 0;
    return 0;
}

/* ---- hooks (fired from the CPU/SoC) ------------------------------------ */
static void dbg_on_event(void *ctx, const cemu_event_t *event) {
    debugger_t *d = (debugger_t *)ctx;
    if (event->type == CEMU_EVENT_BUS) {
        if (event->as.bus.transaction)
            dbg_on_access(d, event->as.bus.transaction);
        return;
    }
    if (event->type != CEMU_EVENT_CONTROL) return;
    const cemu_control_event_t *flow = &event->as.control;
    if (flow->is_return)
        dbg_flow_return(d, flow->kind);
    else
        dbg_flow_call(d, flow->caller_pc, flow->target_pc, flow->kind);
}

static void dbg_flow_call(void *ctx, uint32_t caller_pc, uint32_t target_pc, const char *kind) {
    debugger_t *d = (debugger_t *)ctx;
    if (d->stack_depth < DBG_MAX_STACK) {
        frame_t *f = &d->stack[d->stack_depth++];
        f->callee_pc = target_pc; f->caller_pc = caller_pc;
        f->kind = kind; f->sp = cemu_memory_controller_peek16(&d->soc->memory, A_SP);
    }
}
static void dbg_flow_return(void *ctx, const char *kind) {
    (void)kind;
    debugger_t *d = (debugger_t *)ctx;
    if (d->stack_depth > 0) d->stack_depth--;
    else d->stack_underflows++;
}

static const char *txn_kind_name(const bus_transaction_t *txn) {
    if (txn->kind == BUS_ACCESS_FETCH) return "fetch";
    if (txn->kind == BUS_ACCESS_WRITE) {
        if (txn->route == BUS_ROUTE_SFR) return "sfr_write";
        return "mem_write";
    }
    if (txn->route == BUS_ROUTE_SFR) return "sfr_read";
    if (txn->route == BUS_ROUTE_UNMAPPED) return "unmapped_read";
    return "mem_read";
}

static unsigned txn_kind_bits(const bus_transaction_t *txn) {
    unsigned bits = 0;
    switch (txn->kind) {
        case BUS_ACCESS_FETCH:
            bits |= WK_FETCH;
            break;
        case BUS_ACCESS_READ:
            if (txn->route == BUS_ROUTE_SFR) bits |= WK_SFR_READ;
            else if (txn->route == BUS_ROUTE_UNMAPPED) bits |= WK_UNMAPPED;
            else bits |= WK_MEM_READ;
            break;
        case BUS_ACCESS_WRITE:
            if (txn->route == BUS_ROUTE_SFR) bits |= WK_SFR_WRITE;
            else bits |= WK_MEM_WRITE;
            break;
    }
    if (txn->device && !strcmp(txn->device, "flash")) {
        if (txn->kind == BUS_ACCESS_WRITE) bits |= WK_FLASH_WRITE;
        if (txn->kind == BUS_ACCESS_READ && txn->subtype && !strcmp(txn->subtype, "id"))
            bits |= WK_FLASH_ID;
    }
    return bits;
}

static unsigned kind_token_bits(const char *tok) {
    if (!strcmp(tok, "r")) return WK_MEM_READ;
    if (!strcmp(tok, "w")) return WK_MEM_WRITE;
    if (!strcmp(tok, "rw")) return WK_MEM_READ | WK_MEM_WRITE;
    if (!strcmp(tok, "sfr")) return WK_SFR_READ | WK_SFR_WRITE;
    if (!strcmp(tok, "sfrr")) return WK_SFR_READ;
    if (!strcmp(tok, "sfrw")) return WK_SFR_WRITE;
    if (!strcmp(tok, "fetch")) return WK_FETCH;
    if (!strcmp(tok, "mem_override")) return WK_MEM_OVERRIDE;
    if (!strcmp(tok, "unmapped_read") || !strcmp(tok, "unmapped")) return WK_UNMAPPED;
    if (!strcmp(tok, "flash") || !strcmp(tok, "flashw")) return WK_FLASH_WRITE;
    if (!strcmp(tok, "flashid")) return WK_FLASH_ID;
    if (!strcmp(tok, "mem_read")) return WK_MEM_READ;
    if (!strcmp(tok, "mem_write")) return WK_MEM_WRITE;
    if (!strcmp(tok, "sfr_read")) return WK_SFR_READ;
    if (!strcmp(tok, "sfr_write")) return WK_SFR_WRITE;
    return 0;
}

static void dbg_on_access(void *ctx, const bus_transaction_t *txn) {
    debugger_t *d = (debugger_t *)ctx;
    const char *kind = txn_kind_name(txn);
    unsigned kb = txn_kind_bits(txn);
    int sz = txn->size ? txn->size : 1;
    uint32_t a0 = txn->addr & 0xFFFFFF;
    uint32_t a1 = (a0 + (uint32_t)sz - 1) & 0xFFFFFF;
    for (int i = 0; i < d->n_watches; i++) {
        dbg_watch_t *w = &d->watches[i];
        if (w->kinds && !(w->kinds & kb)) continue;
        if (a1 < a0 ? (w->start > a1 && w->end < a0)
                    : (a1 < w->start || a0 > w->end)) continue;
        if (w->baseline) {
            if (txn->kind == BUS_ACCESS_WRITE) w->dirty = 1;
            continue;
        }
        if (w->log) {
            FILE *lo = d->log_out ? d->log_out : stderr;
            fprintf(lo, "[wlog] icount=%llu pc=0x%08x %s",
                    (unsigned long long)d->cpu->icount, cpu_pc(d->cpu), kind);
            if (txn->device) fprintf(lo, "[%s", txn->device);
            if (txn->subtype) fprintf(lo, txn->device ? ":%s" : "[%s", txn->subtype);
            if (txn->device || txn->subtype) fprintf(lo, "]");
            fprintf(lo, " 0x%06x=0x%x size=%d\n", a0, txn->value, sz);
            fflush(lo);
            d->watch_log_count++;
            continue;
        }
        if (d->have_watch_hit) continue;
        stop_reason_t *h = &d->watch_hit;
        memset(h, 0, sizeof *h);
        h->kind = "watch";
        h->trigger_pc = d->instruction_pc;
        h->pc = cpu_pc(d->cpu);
        h->icount = d->cpu->icount;
        snprintf(h->reason, sizeof h->reason, "watchpoint 0x%06x", a0);
        h->has_addr = 1; h->addr = a0;
        h->access_kind = kind;
        h->access_device = txn->device;
        h->access_subtype = txn->subtype;
        h->has_value = 1; h->value = txn->value;
        h->has_size = 1; h->size = sz;
        d->have_watch_hit = 1;
    }
}

/* ---- breakpoints / watchpoints ---------------------------------------- */
void debugger_add_break(debugger_t *d, uint32_t pc) {
    pc &= 0xFFFFFF;
    for (int i = 0; i < d->n_breaks; i++) if (d->breaks[i] == pc) return;
    if (d->n_breaks < DBG_MAX_BREAKS) d->breaks[d->n_breaks++] = pc;
}
void debugger_remove_break(debugger_t *d, uint32_t pc) {
    pc &= 0xFFFFFF;
    for (int i = 0; i < d->n_breaks; i++)
        if (d->breaks[i] == pc) { d->breaks[i] = d->breaks[--d->n_breaks]; return; }
}
void debugger_clear_breaks(debugger_t *d) { d->n_breaks = 0; }

int debugger_add_watch_ex(debugger_t *d, uint32_t addr, uint32_t end,
                          unsigned kinds, int log, int value_change) {
    if (d->n_watches >= DBG_MAX_WATCHES) return 0;
    uint32_t start = addr & 0xFFFFFF, stop = end & 0xFFFFFF;
    if (stop < start) stop = start;
    memset(&d->watches[d->n_watches], 0, sizeof(dbg_watch_t));
    d->watches[d->n_watches].start = start;
    d->watches[d->n_watches].end = stop;
    d->watches[d->n_watches].kinds = kinds;
    d->watches[d->n_watches].log = log;
    if (value_change) {
        d->watches[d->n_watches].baseline = malloc((size_t)stop - start + 1);
        if (!d->watches[d->n_watches].baseline) return 0;
    }
    d->n_watches++;
    watch_baselines(d, 0);
    return 1;
}
void debugger_add_watch(debugger_t *d, uint32_t addr, uint32_t end,
                        unsigned kinds, int log) {
    (void)debugger_add_watch_ex(d, addr, end, kinds, log, 0);
}
void debugger_clear_watches(debugger_t *d) {
    for (int i = 0; i < d->n_watches; i++) free(d->watches[i].baseline);
    d->n_watches = 0;
}

/* Capture immediately before execution: debugger edits, restored checkpoints,
 * and event-free inspection never become CPU write events. */
static void watch_baselines(debugger_t *d, int after) {
    for (int i = 0; i < d->n_watches; i++) {
        dbg_watch_t *w = &d->watches[i];
        if (!w->baseline) continue;
        if (after && !w->dirty) continue;
        for (uint32_t off = 0; off <= w->end - w->start; off++) {
            uint8_t value = cemu_memory_controller_peek8(&d->soc->memory, w->start + off);
            if (after && value != w->baseline[off] && !d->have_watch_hit) {
                stop_reason_t *h = &d->watch_hit;
                memset(h, 0, sizeof *h);
                h->kind = "watch";
                h->trigger_pc = d->instruction_pc;
                h->icount = d->cpu->icount;
                h->has_addr = h->has_size = h->has_value = h->value_change = 1;
                h->addr = w->start + off;
                h->size = 1;
                h->old_value = w->baseline[off];
                h->new_value = h->value = value;
                h->access_kind = "value_change";
                snprintf(h->reason, sizeof h->reason, "watchpoint %d 0x%06x..0x%06x", i + 1, w->start, w->end);
                d->have_watch_hit = 1;
            }
            w->baseline[off] = value;
        }
        w->dirty = 0;
    }
}

static int is_break(debugger_t *d, uint32_t pc) {
    for (int i = 0; i < d->n_breaks; i++) if (d->breaks[i] == pc) return 1;
    return 0;
}

/* ---- run control ------------------------------------------------------- */
static void capture_disasm(debugger_t *d, uint32_t pc0, const char *text, int size) {
    if (!d->capture_disasm) return;
    d->have_last_exec = 1;
    d->last_exec_pc = pc0;
    snprintf(d->last_exec_text, sizeof d->last_exec_text, "%s", text);
    d->last_exec_size = size;
}

/* Non-breakpoint stop conditions after a step; kind==NULL => keep going. */
static int post_step_stop(debugger_t *d, stop_reason_t *out) {
    if (d->have_watch_hit) {
        *out = d->watch_hit;
        if (!strcmp(out->kind, "watch")) out->pc = cpu_pc(d->cpu);
        d->have_watch_hit = 0;
        return 1;
    }
    if (d->cpu->halted) {
        memset(out, 0, sizeof *out); out->kind = "halted";
        out->pc = cpu_pc(d->cpu); out->icount = d->cpu->icount;
        snprintf(out->reason, sizeof out->reason, "cpu halted"); return 1;
    }
    if (d->cpu->idle && !cemu_soc_idle_wake_possible(d->soc)) {
        memset(out, 0, sizeof *out); out->kind = "idle";
        out->pc = cpu_pc(d->cpu); out->icount = d->cpu->icount;
        snprintf(out->reason, sizeof out->reason,
                 "cpu entered IDLE with no wake source"); return 1;
    }
    return 0;
}

/* The host owns scheduling and all guest access remains on this thread. */
static int cooperate(debugger_t *d, int phase) {
    if (d->service_result < 0 || d->service_result == 2)
        return d->service_result;
    if (!d->service) return 0;
    if (phase == 1 && d->service_countdown) {
        d->service_countdown--;
        return 0;
    }
    d->service_countdown = phase == 1 ? 1023 : 0;
    d->service_result = d->service(d->service_opaque, phase,
                                  d->executed_ticks, d->executed_guest);
    return d->service_result;
}

static int cooperative_stop(debugger_t *d, stop_reason_t *stop) {
    int result = cooperate(d, 1);
    if (!result) return 0;
    memset(stop, 0, sizeof *stop);
    stop->kind = result < 0 ? "error" : "interrupted";
    stop->pc = cpu_pc(d->cpu);
    stop->icount = d->cpu->icount;
    d->service_countdown = 0;
    return 1;
}

/* Execute exactly one instruction; 1 + fill out on fault, else 0. */
static int step_once(debugger_t *d, stop_reason_t *out) {
    d->have_watch_hit = 0;
    uint32_t pc0 = cpu_pc(d->cpu);
    char dis[128]; int have_dis = 0;
    int trace_exec = cemu_event_native_trace_active(&d->soc->instrumentation, "exec");
    if (d->capture_disasm || trace_exec) {
        cpu_disasm(d->cpu, d->soc, dis, sizeof dis);
        have_dis = 1;
    }
    d->instruction_pc = pc0;
    watch_baselines(d, 0);
    step_result_t r = cemu_cpu_step(d->cpu);
    d->executed_ticks++;
    if (d->cpu->last_ran) d->executed_guest++;
    watch_baselines(d, 1);
    if (have_dis && d->cpu->last_ran) capture_disasm(d, pc0, dis, d->cpu->fetch_len);
    cemu_soc_instrument_instruction(d->soc, pc0, have_dis ? dis : "",
                               d->cpu->last_ran ? d->cpu->fetch_len : 0);
    if (d->after_step)
        d->after_step(d->after_step_ctx);
    if (r == STEP_UNIMPL) {
        memset(out, 0, sizeof *out); out->kind = "unimplemented";
        out->pc = cpu_pc(d->cpu); out->icount = d->cpu->icount;
        snprintf(out->reason, sizeof out->reason, "unimplemented opcode 0x%x @ 0x%08x",
                 d->cpu->unimpl_op, d->cpu->unimpl_pc);
        return 1;
    }
    return 0;
}

stop_reason_t debugger_step(debugger_t *d, uint64_t n) {
    stop_reason_t stop; int have = 0;
    if (n < 1) n = 1;
    for (uint64_t i = 0; i < n; i++) {
        if (cooperative_stop(d, &stop)) { have = 1; break; }
        if (step_once(d, &stop)) { have = 1; break; }
        if (post_step_stop(d, &stop)) { have = 1; break; }
    }
    if (!have) {
        memset(&stop, 0, sizeof stop); stop.kind = "step";
        stop.pc = cpu_pc(d->cpu); stop.icount = d->cpu->icount;
    }
    d->parked_pc = (long)cpu_pc(d->cpu);
    return stop;
}

/* Lazily stand up the loop monitor (+ the observe counters it reads). Called on
 * the first cont() with detect_loops on; cheap to keep once built. */
static void ensure_monitor(debugger_t *d) {
    if (d->mon_ready || !d->detect_loops) return;
    if (!cemu_event_statistics_enabled(&d->soc->instrumentation)) cemu_soc_enable_stats(d->soc);
    d->mon = calloc(1, sizeof *d->mon);
    if (!d->mon) { d->detect_loops = 0; return; }   /* out of memory: skip detection */
    monitor_init(d->mon, d->soc, d->cpu, 0, 0);      /* 0 => Python defaults */
    d->mon_ready = 1;
}

/* Fill a stop_reason_t from a monitor verdict (mirror the driver's reason text). */
static void loop_stop(stop_reason_t *stop, const loop_report_t *lp, uint64_t icount) {
    memset(stop, 0, sizeof *stop);
    stop->kind = lp->verdict;
    stop->pc = lp->back_edge_pc; stop->icount = icount;
    if (!strcmp(lp->verdict, "waiting_io"))
        snprintf(stop->reason, sizeof stop->reason,
                 "loop at 0x%06x polling SFR(s) the model never updates "
                 "(likely unmodeled hardware) after %llu iterations",
                 lp->back_edge_pc, (unsigned long long)lp->iterations);
    else if (!strcmp(lp->verdict, "waiting_dpram"))
        snprintf(stop->reason, sizeof stop->reason,
                 "loop at 0x%06x depends on DPRAM with no currently modeled producer "
                 "after %llu iterations",
                 lp->back_edge_pc, (unsigned long long)lp->iterations);
    else
        snprintf(stop->reason, sizeof stop->reason,
                 "stuck spin at 0x%06x: no state change after %llu iterations",
                 lp->back_edge_pc, (unsigned long long)lp->iterations);
}

stop_reason_t debugger_cont(debugger_t *d, uint64_t max_steps) {
    stop_reason_t stop; int have = 0;
    uint64_t used = 0;
    ensure_monitor(d);
    while (max_steps == 0 || used < max_steps) {
        if (cooperative_stop(d, &stop)) { have = 1; break; }
        uint32_t pc = cpu_pc(d->cpu);
        int at_break = is_break(d, pc);
        if (at_break && !(used == 0 && (long)pc == d->parked_pc)) {
            memset(&stop, 0, sizeof stop); stop.kind = "break";
            stop.pc = pc; stop.icount = d->cpu->icount;
            snprintf(stop.reason, sizeof stop.reason, "breakpoint @ 0x%08x", pc);
            have = 1; break;
        }
        if (d->batch_idle && !d->mon_ready && !at_break) {
            uint64_t allowance = max_steps ? max_steps - used : UINT64_MAX;
            if (d->service && allowance > 1024) allowance = 1024;
            char dis[128] = "";
            int trace_exec = cemu_event_native_trace_active(&d->soc->instrumentation, "exec");
            if (trace_exec) cpu_disasm(d->cpu, d->soc, dis, sizeof dis);
            uint64_t first = d->cpu->icount + 1;
            uint64_t advanced = cemu_soc_batch_idle(d->soc, allowance);
            if (advanced) {
                cemu_soc_instrument_idle_span(d->soc, pc, dis, first, advanced);
                if (d->after_step) d->after_step(d->after_step_ctx);
                used += advanced;
                d->executed_ticks += advanced;
                d->service_countdown = 0;
                continue;
            }
        }
        if (step_once(d, &stop)) { have = 1; break; }
        used++;
        if (post_step_stop(d, &stop)) { have = 1; break; }
        if (d->mon_ready) {
            loop_report_t lp;
            if (monitor_take_verdict(d->mon, &lp)) {
                loop_stop(&stop, &lp, d->cpu->icount); have = 1; break;
            }
        }
    }
    if (!have) {
        memset(&stop, 0, sizeof stop); stop.kind = "limit";
        stop.pc = cpu_pc(d->cpu); stop.icount = d->cpu->icount;
        snprintf(stop.reason, sizeof stop.reason, "reached max_steps %llu",
                 (unsigned long long)max_steps);
    }
    d->parked_pc = (long)cpu_pc(d->cpu);
    return stop;
}

/* ---- inspection -------------------------------------------------------- */
void debugger_regs(debugger_t *d, dbg_regs_t *o) {
    memset(o, 0, sizeof *o);
    uint16_t cp = cemu_memory_controller_peek16(&d->soc->memory, A_CP);
    o->pc = cpu_pc(d->cpu);
    o->icount = d->cpu->icount;
    o->csp = d->cpu->csp & 0xFF;
    o->ip = d->cpu->ip & 0xFFFF;
    o->psw = cemu_memory_controller_peek16(&d->soc->memory, A_PSW);
    o->sp = cemu_memory_controller_peek16(&d->soc->memory, A_SP);
    o->cp = cp;
    for (int i = 0; i < 4; i++) o->dpp[i] = cemu_memory_controller_peek16(&d->soc->memory, A_DPP[i]);
    for (int n = 0; n < 16; n++) o->gpr[n] = cemu_memory_controller_peek16(&d->soc->memory, (cp + 2u * n) & 0xFFFF);
    o->halted = d->cpu->halted;
    o->idle = d->cpu->idle;
}
void debugger_read_mem(debugger_t *d, uint32_t addr, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) out[i] = cemu_memory_controller_peek8(&d->soc->memory, (addr + i) & 0xFFFFFF);
}
uint16_t debugger_read_word(debugger_t *d, uint32_t addr) {
    return cemu_memory_controller_peek16(&d->soc->memory, addr & 0xFFFFFF);
}
uint16_t debugger_read_sfr(debugger_t *d, uint32_t addr) {
    return cemu_memory_controller_peek16(&d->soc->memory, addr & 0xFFFFFF);
}
void debugger_disasm_last(debugger_t *d, char *buf, int cap) {
    if (d->have_last_exec) {
        snprintf(buf, cap, "0x%08x: %s", d->last_exec_pc, d->last_exec_text);
        return;
    }
    uint32_t pc = cpu_pc(d->cpu);
    uint8_t raw[4]; debugger_read_mem(d, pc, raw, 4);
    snprintf(buf, cap, "0x%08x: %02x %02x %02x %02x (no disasm)",
             pc, raw[0], raw[1], raw[2], raw[3]);
}
int debugger_disasm_at(debugger_t *d, uint32_t addr, char *buf, int cap) {
    return cpu_disasm_at(d->cpu, d->soc, addr, buf, cap);
}

/* ---- mutation (event-free poke) --------------------------------------- */
void debugger_write_mem(debugger_t *d, uint32_t addr, const uint8_t *bytes, int n) {
    for (int i = 0; i < n; i++) cemu_memory_controller_poke8(&d->soc->memory, (addr + i) & 0xFFFFFF, bytes[i]);
    watch_baselines(d, 0);
}
void debugger_write_word(debugger_t *d, uint32_t addr, uint16_t val) {
    cemu_memory_controller_poke16(&d->soc->memory, addr & 0xFFFFFF, val);
    watch_baselines(d, 0);
}
void debugger_write_sfr(debugger_t *d, uint32_t addr, uint16_t val) {
    cemu_memory_controller_poke16(&d->soc->memory, addr & 0xFFFFFF, val);
    watch_baselines(d, 0);
}
void debugger_set_gpr(debugger_t *d, int n, uint16_t val) {
    uint16_t cp = cemu_memory_controller_peek16(&d->soc->memory, A_CP);
    cemu_memory_controller_poke16(&d->soc->memory, (cp + 2u * (n & 0xF)) & 0xFFFF, val);
    watch_baselines(d, 0);
}
int debugger_set_reg(debugger_t *d, const char *name, uint32_t val) {
    if (!strcmp(name, "pc")) { d->cpu->csp = (val >> 16) & 0xFF; d->cpu->ip = val & 0xFFFF; return 1; }
    if (!strcmp(name, "ip"))  { d->cpu->ip = val & 0xFFFF; return 1; }
    if (!strcmp(name, "csp")) { d->cpu->csp = val & 0xFF; return 1; }
    if (!strcmp(name, "sp"))  { debugger_write_sfr(d, A_SP, (uint16_t)val); return 1; }
    if (!strcmp(name, "cp"))  { debugger_write_sfr(d, A_CP, (uint16_t)val); return 1; }
    if (!strcmp(name, "psw")) { debugger_write_sfr(d, A_PSW, (uint16_t)val); return 1; }
    for (int i = 0; i < 4; i++) {
        char nm[8]; snprintf(nm, sizeof nm, "dpp%d", i);
        if (!strcmp(name, nm)) { debugger_write_sfr(d, A_DPP[i], (uint16_t)val); return 1; }
    }
    if ((name[0] == 'r' || name[0] == 'R') && isdigit((unsigned char)name[1])) {
        int n = atoi(name + 1);
        if (n >= 0 && n < 16) { debugger_set_gpr(d, n, (uint16_t)val); return 1; }
    }
    return 0;
}

/* ---- backtrace --------------------------------------------------------- */
int debugger_backtrace(debugger_t *d, frame_t *out, int cap) {
    int n = 0;
    for (int i = d->stack_depth - 1; i >= 0 && n < cap; i--) out[n++] = d->stack[i];
    return n;
}

/* ---- checkpoint / rewind ---------------------------------------------- */
void debugger_checkpoint(debugger_t *d) {
    dbg_checkpoint_t *cp = &d->checkpoint;
    if (!cp->ram) cp->ram = malloc(ADDR_SPACE);
    if (!cp->present) cp->present = malloc(ADDR_SPACE);
    if (!cp->lm) cp->lm = malloc(d->soc->memory.lm_size);
    memcpy(cp->ram, d->soc->memory.ram, ADDR_SPACE);
    memcpy(cp->present, d->soc->memory.present, ADDR_SPACE);
    memcpy(cp->lm, d->soc->memory.lm, d->soc->memory.lm_size);
    cp->n_external_ram = d->soc->memory.n_external_ram;
    for (int i = 0; i < cp->n_external_ram; i++) {
        external_ram_state_t *ram = cemu_memory_controller_external_ram_state(&d->soc->memory, i);
        if (cp->external_ram_size[i] != ram->chip_size) {
            free(cp->external_ram[i]);
            cp->external_ram[i] = malloc(ram->chip_size);
            cp->external_ram_size[i] = ram->chip_size;
        }
        memcpy(cp->external_ram[i], ram->bytes, ram->chip_size);
    }
    memcpy(cp->sfr, d->soc->memory.sfr, sizeof cp->sfr);
    free(cp->serial_tx);
    cp->serial_tx_len = d->soc->serial_tx_len;
    cp->serial_tx = malloc(cp->serial_tx_len ? cp->serial_tx_len : 1);
    memcpy(cp->serial_tx, d->soc->serial_tx, cp->serial_tx_len);
    free(cp->serial_rx);
    cp->serial_rx_len = d->soc->serial_rx_len - d->soc->serial_rx_head;
    cp->serial_rx = malloc(cp->serial_rx_len ? cp->serial_rx_len : 1);
    if (cp->serial_rx_len)
        memcpy(cp->serial_rx,
               d->soc->serial_rx + d->soc->serial_rx_head,
               cp->serial_rx_len);
    cp->serial = *(serial_state_t *)d->soc->serial_periph->state;
    cp->n_flash_chips = d->soc->memory.n_flash_chips;
    for (int i = 0; i < cp->n_flash_chips; i++)
        cemu_flash_state_copy(&cp->flash[i], cemu_memory_controller_flash_state(&d->soc->memory, i));
    cp->ssc0 = *(ssc0_state_t *)d->soc->ssc0_periph->state;
    cemu_keypad_capture_mutable(d->soc->keypad_periph->state, &cp->keypad);
    cp->xbus_unknown1 = *(xbus_unknown1_state_t *)
        d->soc->xbus_unknown1_periph->state;
    cp->has_twi_gpio = d->soc->twi_gpio_periph != NULL;
    if (cp->has_twi_gpio)
        cp->twi_gpio = *(twi_gpio_state_t *)d->soc->twi_gpio_periph->state;
    cp->has_battery = cemu_battery_available(d->soc->battery_periph);
    if (cp->has_battery)
        cp->battery = *(battery_state_t *)d->soc->battery_periph->state;
    cp->has_lcd = d->soc->lcd_periph != NULL;
    if (cp->has_lcd) {
        cp->lcd_state_size = cemu_lcd_state_size(d->soc->lcd_periph);
        memcpy(&cp->lcd, d->soc->lcd_periph->state, cp->lcd_state_size);
    }
    cp->csp = d->cpu->csp; cp->ip = d->cpu->ip;
    cp->ext_kind = d->cpu->ext_kind; cp->ext_val = d->cpu->ext_val;
    cp->ext_count = d->cpu->ext_count; cp->extr = d->cpu->extr;
    cp->halted = d->cpu->halted; cp->idle = d->cpu->idle;
    cp->icount = d->cpu->icount;
    cp->interrupts_delivered = d->cpu->interrupts_delivered;
    cp->traps_taken = d->cpu->traps_taken;
    cp->ticks = d->soc->ticks;
    cp->init_locked = d->soc->init_locked;
    cp->rstout = d->soc->rstout;
    cp->debug_irq_valid = d->soc->interrupts.debug_valid;
    cp->debug_irq_addr = d->soc->interrupts.debug_addr;
    cp->debug_irq_trap = d->soc->interrupts.debug_trap;
    cp->debug_irq_ilvl = d->soc->interrupts.debug_ilvl;
    memcpy(cp->stack, d->stack, sizeof(frame_t) * d->stack_depth);
    cp->stack_depth = d->stack_depth;
    cp->stack_underflows = d->stack_underflows;
    cp->valid = 1;
}
int debugger_restore(debugger_t *d) {
    dbg_checkpoint_t *cp = &d->checkpoint;
    if (!cp->valid) return 0;
    memcpy(d->soc->memory.ram, cp->ram, ADDR_SPACE);
    memcpy(d->soc->memory.present, cp->present, ADDR_SPACE);
    memcpy(d->soc->memory.lm, cp->lm, d->soc->memory.lm_size);
    if (cp->n_external_ram != d->soc->memory.n_external_ram) return 0;
    for (int i = 0; i < cp->n_external_ram; i++) {
        external_ram_state_t *ram = cemu_memory_controller_external_ram_state(&d->soc->memory, i);
        if (cp->external_ram_size[i] != ram->chip_size) return 0;
        memcpy(ram->bytes, cp->external_ram[i], ram->chip_size);
    }
    memcpy(d->soc->memory.sfr, cp->sfr, sizeof cp->sfr);
    d->soc->serial_tx_len = cp->serial_tx_len;
    if (cp->serial_tx_len > d->soc->serial_tx_cap) {
        d->soc->serial_tx_cap = cp->serial_tx_len;
        d->soc->serial_tx = realloc(d->soc->serial_tx, d->soc->serial_tx_cap);
    }
    memcpy(d->soc->serial_tx, cp->serial_tx, cp->serial_tx_len);
    if (cp->serial_rx_len > d->soc->serial_rx_cap) {
        d->soc->serial_rx_cap = cp->serial_rx_len;
        d->soc->serial_rx = realloc(
            d->soc->serial_rx, d->soc->serial_rx_cap);
    }
    if (cp->serial_rx_len)
        memcpy(d->soc->serial_rx, cp->serial_rx, cp->serial_rx_len);
    d->soc->serial_rx_head = 0;
    d->soc->serial_rx_len = cp->serial_rx_len;
    *(serial_state_t *)d->soc->serial_periph->state = cp->serial;
    if (cp->n_flash_chips != d->soc->memory.n_flash_chips) return 0;
    for (int i = 0; i < cp->n_flash_chips; i++)
        cemu_flash_state_copy(cemu_memory_controller_flash_state(&d->soc->memory, i), &cp->flash[i]);
    d->cpu->csp = cp->csp; d->cpu->ip = cp->ip;
    *(ssc0_state_t *)d->soc->ssc0_periph->state = cp->ssc0;
    if (!cemu_keypad_restore_mutable(d->soc->keypad_periph->state,
                                     &cp->keypad))
        return 0;
    *(xbus_unknown1_state_t *)d->soc->xbus_unknown1_periph->state =
        cp->xbus_unknown1;
    if (cp->has_twi_gpio != (d->soc->twi_gpio_periph != NULL))
        return 0;
    if (cp->has_twi_gpio) {
        *(twi_gpio_state_t *)d->soc->twi_gpio_periph->state = cp->twi_gpio;
        cemu_twi_gpio_finish_restore(d->soc->twi_gpio_periph, d->soc);
    }
    if (cp->has_battery != cemu_battery_available(d->soc->battery_periph))
        return 0;
    if (cp->has_battery)
        *(battery_state_t *)d->soc->battery_periph->state = cp->battery;
    if (cp->has_lcd && d->soc->lcd_periph) {
        if (cp->lcd_state_size != cemu_lcd_state_size(d->soc->lcd_periph))
            return 0;
        cemu_lcd_prepare_restore(d->soc->lcd_periph, d->soc);
        memcpy(d->soc->lcd_periph->state, &cp->lcd, cp->lcd_state_size);
        cemu_lcd_finish_restore(d->soc->lcd_periph, d->soc);
    }
    d->cpu->ext_kind = cp->ext_kind; d->cpu->ext_val = cp->ext_val;
    d->cpu->ext_count = cp->ext_count; d->cpu->extr = cp->extr;
    d->cpu->halted = cp->halted; d->cpu->idle = cp->idle;
    d->cpu->icount = cp->icount;
    d->cpu->interrupts_delivered = cp->interrupts_delivered;
    d->cpu->traps_taken = cp->traps_taken;
    d->soc->ticks = cp->ticks;
    d->soc->init_locked = cp->init_locked;
    d->soc->rstout = cp->rstout;
    cemu_interrupt_subsystem_debug_restore(
        &d->soc->interrupts, cp->debug_irq_valid, cp->debug_irq_addr,
        cp->debug_irq_trap, cp->debug_irq_ilvl);
    memcpy(d->stack, cp->stack, sizeof(frame_t) * cp->stack_depth);
    d->stack_depth = cp->stack_depth;
    d->stack_underflows = cp->stack_underflows;
    d->have_watch_hit = 0;
    d->parked_pc = -1;
    d->have_last_exec = 0;
    watch_baselines(d, 0);
    if (d->service) d->serial_output_offset = d->soc->serial_tx_len;
    (void)cooperate(d, 2);
    return 1;
}

/* ---- stop-reason formatting (mirror StopReason.__str__) --------------- */
void stop_reason_str(const stop_reason_t *s, char *buf, int cap) {
    int n = snprintf(buf, cap, "[%s] pc=0x%08x icount=%llu",
                     s->kind, s->pc, (unsigned long long)s->icount);
    if (!strcmp(s->kind, "watch") && n < cap) {
        n += snprintf(buf + n, cap - n, " trigger_pc=0x%08x", s->trigger_pc);
        if (s->value_change && n < cap)
            n += snprintf(buf + n, cap - n, " old=0x%02x new=0x%02x", s->old_value, s->new_value);
        if (n < cap)
            n += snprintf(buf + n, cap - n, " %s",
                          s->access_kind ? s->access_kind : "?");
        if ((s->access_device || s->access_subtype) && n < cap) {
            n += snprintf(buf + n, cap - n, "[");
            if (n < cap && s->access_device)
                n += snprintf(buf + n, cap - n, "%s", s->access_device);
            if (n < cap && s->access_subtype)
                n += snprintf(buf + n, cap - n, "%s%s",
                              s->access_device ? ":" : "", s->access_subtype);
            if (n < cap) n += snprintf(buf + n, cap - n, "]");
        }
        if (n < cap) n += snprintf(buf + n, cap - n, " 0x%08x=", s->addr);
        if (n < cap) {
            if (s->has_value) n += snprintf(buf + n, cap - n, "0x%x", s->value);
            else              n += snprintf(buf + n, cap - n, "?");
        }
        if (n < cap) n += snprintf(buf + n, cap - n, " size=%d", s->size);
    }
    if (s->reason[0] && n < cap) snprintf(buf + n, cap - n, "  (%s)", s->reason);
}

static void debugger_print_serial(debugger_t *d, FILE *out) {
    if (d->soc->serial_tx_len < d->serial_output_offset)
        d->serial_output_offset = 0;
    size_t length = d->soc->serial_tx_len - d->serial_output_offset;
    if (!length) return;
    char *escaped = malloc(length * 4u + 1u);
    if (!escaped) return;
    size_t escaped_len = 0;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = d->soc->serial_tx[d->serial_output_offset + i];
        if (byte >= 0x20 && byte < 0x7f && byte != '\\')
            escaped[escaped_len++] = (char)byte;
        else {
            escaped[escaped_len++] = '\\';
            escaped[escaped_len++] = 'x';
            escaped[escaped_len++] = hex[byte >> 4];
            escaped[escaped_len++] = hex[byte & 15];
        }
    }
    escaped[escaped_len] = 0;
    fprintf(out, "serial: %s\n", escaped);
    free(escaped);
    d->serial_output_offset = d->soc->serial_tx_len;
}

/* ======================================================================= */
/* Session front-end (script + REPL), mirror tools/debug.py.               */
/* ======================================================================= */

/* Parse an address/int like boot._parse_int (base-0, masked to 24 bits). */
static uint32_t parse_int24(const char *s) { return (uint32_t)strtoul(s, NULL, 0) & 0xFFFFFF; }
/* Parse a count with optional k/m/g suffix like boot._parse_count. */
static uint64_t parse_count(const char *s) {
    size_t len = strlen(s);
    if (!len) return 0;
    char suf = tolower((unsigned char)s[len - 1]);
    uint64_t mult = (suf == 'k') ? 1000 : (suf == 'm') ? 1000000 : (suf == 'g') ? 1000000000 : 0;
    if (mult) {
        char buf[64]; snprintf(buf, sizeof buf, "%.*s", (int)(len - 1), s);
        double base = strtod(buf, NULL);
        return (uint64_t)(base * (double)mult);
    }
    return strtoull(s, NULL, 0);
}

/* Resolve a `watch ... KINDS` token to a WK_* mask (mirror _resolve_kinds). */
static unsigned resolve_kinds(const char *tok) {
    if (!tok || !*tok) return WK_ALL_DEFAULT;
    unsigned mask = 0;
    char buf[64]; snprintf(buf, sizeof buf, "%s", tok);
    for (char *p = strtok(buf, "+"); p; p = strtok(NULL, "+")) {
        if      (!strcmp(p, "r"))    mask |= WK_MEM_READ;
        else if (!strcmp(p, "w"))    mask |= WK_MEM_WRITE;
        else if (!strcmp(p, "rw"))   mask |= WK_MEM_READ | WK_MEM_WRITE;
        else if (!strcmp(p, "sfr"))  mask |= WK_SFR_READ | WK_SFR_WRITE;
        else if (!strcmp(p, "sfrr")) mask |= WK_SFR_READ;
        else if (!strcmp(p, "sfrw")) mask |= WK_SFR_WRITE;
        else mask |= kind_token_bits(p);
    }
    return mask ? mask : WK_ALL_DEFAULT;
}

static int parse_port_pin(const char *tok, int *port, int *bit) {
    if (!tok || toupper((unsigned char)tok[0]) != 'P') return 0;
    if (tok[1] != '3' && tok[1] != '6' &&
        tok[1] != '7' && tok[1] != '8') return 0;
    if (tok[2] != '.') return 0;
    char *end = NULL;
    long b = strtol(tok + 3, &end, 0);
    if (!end || *end || b < 0 || b > 15) return 0;
    *port = tok[1] - '0';
    *bit = (int)b;
    return 1;
}

static int parse_cc_channel(const char *tok, int *channel) {
    if (!tok || toupper((unsigned char)tok[0]) != 'C' || toupper((unsigned char)tok[1]) != 'C') return 0;
    char *end = NULL;
    long ch = strtol(tok + 2, &end, 0);
    if (!end || *end || ch < 0 || ch > 31) return 0;
    *channel = (int)ch;
    return 1;
}

static int parse_rising_token(const char *tok, int *rising) {
    if (!tok) return 0;
    char buf[16];
    int i = 0;
    for (; tok[i] && i < (int)sizeof(buf) - 1; i++) buf[i] = (char)tolower((unsigned char)tok[i]);
    buf[i] = 0;
    if (!strcmp(buf, "rise") || !strcmp(buf, "rising") || !strcmp(buf, "1")) { *rising = 1; return 1; }
    if (!strcmp(buf, "fall") || !strcmp(buf, "falling") || !strcmp(buf, "0")) { *rising = 0; return 1; }
    return 0;
}

static int parse_product_irq(const char *tok, int *irq, uint32_t *addr, int *trap) {
    if (!tok) return 0;
    if (!strcmp(tok, "IRQ45") || !strcmp(tok, "irq45")) {
        *irq = 45;
        *addr = 0xF190u;
        *trap = 0x4D;
        return 1;
    }
    if (!strcmp(tok, "XP2") || !strcmp(tok, "xp2")) {
        *irq = 38;
        *addr = 0xF196u;
        *trap = 0x42;
        return 1;
    }
    const char *p = tok;
    if ((p[0] == 'I' || p[0] == 'i') &&
        (p[1] == 'R' || p[1] == 'r') &&
        (p[2] == 'Q' || p[2] == 'q')) p += 3;
    char *end = NULL;
    long n = strtol(p, &end, 0);
    if (!end || *end || n < 64 || n > 79) return 0;
    *irq = (int)n;
    *addr = 0xF120u + (uint32_t)(n - 64) * 2u;
    *trap = 0x50 + (int)(n - 64);
    return 1;
}

static const char *port_edge_name(int edge) {
    if (edge == SOC_PORT_EDGE_RISING) return "rising";
    if (edge == SOC_PORT_EDGE_FALLING) return "falling";
    if (edge == SOC_PORT_EDGE_NONE) return "unchanged";
    return "invalid";
}

static void fmt_regs(debugger_t *d, FILE *out) {
    dbg_regs_t r; debugger_regs(d, &r);
    fprintf(out, "pc=0x%08x  csp=0x%02x ip=0x%04x  icount=%llu\n",
            r.pc, r.csp, r.ip, (unsigned long long)r.icount);
    fprintf(out, "psw=0x%04x  sp=0x%04x  cp=0x%04x  dpp=0x%04x 0x%04x 0x%04x 0x%04x\n",
            r.psw, r.sp, r.cp, r.dpp[0], r.dpp[1], r.dpp[2], r.dpp[3]);
    for (int row = 0; row < 16; row += 8) {
        for (int n = row; n < row + 8; n++)
            fprintf(out, "r%-2d=0x%04x%s", n, r.gpr[n], n == row + 7 ? "" : " ");
        fputc('\n', out);
    }
    if (r.halted || r.idle) fprintf(out, "halted=%d idle=%d\n", r.halted, r.idle);
}

static void fmt_hexdump(debugger_t *d, uint32_t addr, int n, FILE *out) {
    uint8_t buf[4096]; if (n > (int)sizeof buf) n = sizeof buf;
    debugger_read_mem(d, addr, buf, n);
    for (int off = 0; off < n; off += 16) {
        int cnt = (n - off < 16) ? n - off : 16;
        fprintf(out, "0x%08x: ", addr + off);
        char asc[17];
        for (int i = 0; i < 16; i++) {
            if (i < cnt) { fprintf(out, "%02x ", buf[off + i]);
                           uint8_t c = buf[off + i]; asc[i] = (c >= 32 && c < 127) ? c : '.'; }
            else { fprintf(out, "   "); asc[i] = ' '; }
        }
        asc[16] = 0;
        fprintf(out, " %s\n", asc);
    }
}

static void fmt_backtrace(debugger_t *d, FILE *out) {
    frame_t fr[DBG_MAX_STACK];
    int n = debugger_backtrace(d, fr, DBG_MAX_STACK);
    if (!n) { fprintf(out, "  (empty — no CALL observed since attach)\n"); return; }
    for (int i = 0; i < n; i++)
        fprintf(out, "  #%d 0x%08x  <- %s @ 0x%08x  (sp=0x%04x)\n",
                i, fr[i].callee_pc, fr[i].kind, fr[i].caller_pc, fr[i].sp);
}

/* Split a line into whitespace tokens (in place). Returns token count. */
static int tokenize(char *line, char **tok, int max) {
    int n = 0;
    for (char *p = strtok(line, " \t"); p && n < max; p = strtok(NULL, " \t")) tok[n++] = p;
    return n;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode an entire batch before exposing any of it to ASC0.  This keeps a
 * malformed debugger command atomic: the firmware either receives every byte
 * through the canonical timed queue or observes no change. */
static int decode_serial_hex(const char *hex, uint8_t *bytes, size_t *count) {
    size_t len = strlen(hex);
    if (!len || (len & 1u) || len > DBG_SERIAL_RX_MAX * 2u) return 0;
    for (size_t i = 0; i < len; i += 2u) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1u]);
        if (hi < 0 || lo < 0) return 0;
        bytes[i / 2u] = (uint8_t)((hi << 4) | lo);
    }
    *count = len / 2u;
    return 1;
}

/* Command reference, shown by `help` / `?` (and on an unknown command). */
static void dbg_help(FILE *out) {
    fprintf(out,
"commands (ADDR/VALUE are base-0: 0x.., decimal; counts accept k/m/g):\n"
"  break|b ADDR              set a PC breakpoint\n"
"  delete|d ADDR             remove a PC breakpoint\n"
"  watch|w ADDR[..END]       break on value change (one byte if no END)\n"
"  rwatch/awatch ADDR[..END] break on reads / reads or writes, no fetch\n"
"  watch ADDR[..END] K      legacy access filter; r,w,rw are memory-only;\n"
"                            K = sfr,sfrr,sfrw for SFR/ESFR; default is rw+sfr\n"
"  wlog|wl ADDR[..END] [K]   log EVERY access and keep running (no stop)\n"
"  cont|c [MAX]              run until break/watch/halt/idle/fault (or monitor verdict)\n"
"                            MAX is an optional step cap; omitted means no cap\n"
"  monitor [on|off]          show or change loop/stall monitoring (default off)\n"
"  step|si [N]               single-step N instructions (default 1)\n"
"  regs|p                    dump registers\n"
"  peripherals|periph       list logical peripheral roles and models\n"
"  bt                        backtrace (shadow call stack)\n"
"  x ADDR [N]                hexdump N bytes (default 16)\n"
"  sfr ADDR [VALUE]          read (or, with VALUE, write) an SFR word\n"
"  set ADDR VALUE [size]     poke memory (size 1/2, default 2)\n"
"  setr REG VALUE            set a register (r0..r15, pc/sp/cp/psw/ip/csp/dpp0..3)\n"
"  twi [REG [VALUE]]         inspect TWI state/register or poke a register byte\n"
"  trace PATH [SELECTORS]    write Parquet trace from now on (selectors as --trace)\n"
"  pin P3.N|P6.N|P7.N|P8.N 0|1\n"
"                            drive an external port pin level\n"
"  serial-rx HEX             queue raw bytes through timed ASC0 receive\n"
"  edge CCN rise|fall        inject a CAPCOM capture edge (N=0..31)\n"
"  key NAME down|up          drive a configured key (0-9, star/hash, arrows, softkeys)\n"
"  irq IRQ45|IRQ64..IRQ79|XP2 [ILVL] force a debugger-only product IRQ source\n"
"  dis                       last executed instruction\n"
"  disasm|di ADDR [N]        disassemble N instructions from ADDR (default 1)\n"
"  checkpoint|cp             save an in-memory checkpoint\n"
"  restore                   restore the last checkpoint\n"
"  help|?                    show this list\n"
"  quit|q                    exit (REPL only)\n");
}

/* Run one command line. Returns 0 normally, 1 if it was 'quit'. */
static int run_command(debugger_t *d, char *line, FILE *out) {
    /* trim leading ws */
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return 0;
    char *tok[8]; int nt = tokenize(line, tok, 8);
    if (nt == 0) return 0;
    const char *cmd = tok[0];
    char sb[256];

    if (!strcmp(cmd, "quit") || !strcmp(cmd, "q")) return 1;
    else if (!strcmp(cmd, "break") || !strcmp(cmd, "b")) {
        if (nt < 2) { fprintf(out, "usage: break ADDR\n"); return 0; }
        uint32_t a = parse_int24(tok[1]); debugger_add_break(d, a);
        fprintf(out, "breakpoint set @ 0x%08x\n", a);
    } else if (!strcmp(cmd, "delete") || !strcmp(cmd, "d")) {
        uint32_t a = parse_int24(tok[1]); debugger_remove_break(d, a);
        fprintf(out, "breakpoint cleared @ 0x%08x\n", a);
    } else if (!strcmp(cmd, "watch") || !strcmp(cmd, "w") ||
               !strcmp(cmd, "rwatch") || !strcmp(cmd, "awatch") ||
               !strcmp(cmd, "wlog") || !strcmp(cmd, "wl")) {
        int log = (cmd[1] == 'l' || !strcmp(cmd, "wlog"));  /* wl / wlog => log mode */
        if (nt < 2) { fprintf(out, "usage: %s ADDR[..END] [K]\n", cmd); return 0; }
        char *rng = tok[1]; uint32_t start, end;
        char *dots = strstr(rng, "..");
        if (dots) { *dots = 0; start = parse_int24(rng); end = parse_int24(dots + 2); }
        else start = end = parse_int24(rng);
        unsigned kinds = resolve_kinds(nt > 2 ? tok[2] : NULL);
        int change = !log && nt == 2 && (!strcmp(cmd, "watch") || !strcmp(cmd, "w"));
        if (!strcmp(cmd, "rwatch") || !strcmp(cmd, "awatch")) {
            kinds = WK_MEM_READ | WK_SFR_READ | WK_UNMAPPED | WK_FLASH_ID;
            if (cmd[0] == 'a') kinds |= WK_MEM_WRITE | WK_SFR_WRITE | WK_FLASH_WRITE;
        }
        if (!debugger_add_watch_ex(d, start, end, kinds, log, change)) {
            fprintf(out, "cannot allocate watchpoint (capacity or memory limit)\n");
            return 0;
        }
        if (log) d->log_out = out;   /* log-watches print to the session stream */
        fprintf(out, "%swatchpoint set @ 0x%08x..0x%08x\n",
                log ? "logging " : "", start, end);
    } else if (!strcmp(cmd, "monitor")) {
        if (nt == 1) {
            fprintf(out, "monitor: %s\n", debugger_monitor_enabled(d) ? "on" : "off");
        } else if (nt == 2 && !strcmp(tok[1], "on")) {
            debugger_set_monitor(d, 1);
            fprintf(out, "monitor: on\n");
        } else if (nt == 2 && !strcmp(tok[1], "off")) {
            debugger_set_monitor(d, 0);
            fprintf(out, "monitor: off\n");
        } else {
            fprintf(out, "usage: monitor [on|off]\n");
        }
    } else if (!strcmp(cmd, "cont") || !strcmp(cmd, "c")) {
        uint64_t ms = nt > 1 ? parse_count(tok[1]) : 0;
        uint64_t before = d->watch_log_count;
        stop_reason_t s = debugger_cont(d, ms);
        debugger_print_serial(d, out);
        stop_reason_str(&s, sb, sizeof sb); fprintf(out, "%s\n", sb);
        /* However the run ended (stall/limit/break/halt), report any log-watch
         * tally so a `wlog; c` session always tells you what it captured. */
        if (d->watch_log_count > before)
            fprintf(out, "[wlog] %llu access(es) logged this run (%llu total)\n",
                    (unsigned long long)(d->watch_log_count - before),
                    (unsigned long long)d->watch_log_count);
    } else if (!strcmp(cmd, "step") || !strcmp(cmd, "si")) {
        uint64_t n = nt > 1 ? parse_count(tok[1]) : 1;
        stop_reason_t s = debugger_step(d, n);
        debugger_print_serial(d, out);
        stop_reason_str(&s, sb, sizeof sb); fprintf(out, "%s\n", sb);
        debugger_disasm_last(d, sb, sizeof sb); fprintf(out, "%s\n", sb);
    } else if (!strcmp(cmd, "regs") || !strcmp(cmd, "p")) {
        fmt_regs(d, out);
    } else if (!strcmp(cmd, "peripherals") || !strcmp(cmd, "periph")) {
        for (int i = 0; i < d->soc->n_peripherals; i++) {
            const peripheral_t *p = d->soc->peripherals[i];
            fprintf(out, "  %s", p->id);
            if (p->model) fprintf(out, " (%s)", p->model);
            fputc('\n', out);
        }
    } else if (!strcmp(cmd, "bt")) {
        fmt_backtrace(d, out);
    } else if (!strcmp(cmd, "x")) {
        uint32_t a = parse_int24(tok[1]);
        int n = nt > 2 ? (int)parse_count(tok[2]) : 16;
        fmt_hexdump(d, a, n, out);
    } else if (!strcmp(cmd, "sfr")) {
        uint32_t a = parse_int24(tok[1]);
        if (nt > 2) {   /* write form: sfr ADDR VALUE */
            uint16_t v = (uint16_t)strtoul(tok[2], NULL, 0);
            debugger_write_sfr(d, a, v);
            fprintf(out, "0x%06x = 0x%04x\n", a, debugger_read_sfr(d, a));
        } else {
            const char *nm = cemu_soc_sfr_name(d->soc, a & 0xFFFFFE);
            fprintf(out, "0x%06x (%s) = 0x%04x\n", a, nm ? nm : "?", debugger_read_sfr(d, a));
        }
    } else if (!strcmp(cmd, "set")) {   /* set ADDR VALUE [size] */
        uint32_t a = parse_int24(tok[1]);
        uint32_t v = (uint32_t)strtoul(tok[2], NULL, 0);
        int size = nt > 3 ? (int)strtol(tok[3], NULL, 0) : 2;
        if (size == 1) { uint8_t b = (uint8_t)v; debugger_write_mem(d, a, &b, 1);
                         fprintf(out, "set 0x%06x = 0x%02x\n", a, cemu_memory_controller_peek8(&d->soc->memory, a)); }
        else           { debugger_write_word(d, a, (uint16_t)v);
                         fprintf(out, "set 0x%06x = 0x%04x\n", a, debugger_read_word(d, a)); }
    } else if (!strcmp(cmd, "setr")) {  /* setr REG VALUE */
        uint32_t v = (uint32_t)strtoul(tok[2], NULL, 0);
        if (debugger_set_reg(d, tok[1], v)) fprintf(out, "setr %s = 0x%x\n", tok[1], v);
        else fprintf(out, "unknown register: %s\n", tok[1]);
    } else if (!strcmp(cmd, "twi")) {
        if (!d->soc->twi_gpio_periph) {
            fprintf(out, "TWI register-file device is not configured\n");
            return 0;
        }
        twi_gpio_state_t *st = d->soc->twi_gpio_periph->state;
        if (nt == 1) {
            fprintf(out,
                    "twi model=%s address=0x%02x P%u.%u/P%u.%u "
                    "registers=%u pointer=0x%02x reads=%llu writes=%llu\n",
                    d->soc->twi_gpio_periph->model, st->address,
                    st->port, st->scl_bit, st->port, st->sda_bit,
                    st->register_count, st->register_pointer,
                    (unsigned long long)st->register_reads,
                    (unsigned long long)st->register_writes);
            return 0;
        }
        char *end = NULL;
        unsigned long reg = strtoul(tok[1], &end, 0);
        if (!end || *end || reg > 0xFFu || reg >= st->register_count) {
            fprintf(out, "TWI register out of range: %s (count=%u)\n",
                    tok[1], st->register_count);
            return 0;
        }
        if (nt == 2) {
            fprintf(out, "twi[0x%02lx] = 0x%02x\n",
                    reg, st->registers[reg]);
            return 0;
        }
        end = NULL;
        unsigned long value = strtoul(tok[2], &end, 0);
        if (!end || *end || value > 0xFFu) {
            fprintf(out, "invalid TWI byte: %s\n", tok[2]);
            return 0;
        }
        cemu_twi_gpio_debug_write(st, d->soc, (unsigned)reg, (uint8_t)value);
        fprintf(out, "twi[0x%02lx] = 0x%02lx\n", reg, value);
    } else if (!strcmp(cmd, "trace")) {
        fprintf(out, "trace output is controlled by the host command line\n");
    } else if (!strcmp(cmd, "pin")) {
        if (nt < 3) { fprintf(out, "usage: pin P3.N|P6.N|P7.N|P8.N 0|1\n"); return 0; }
        int port, bit;
        if (!parse_port_pin(tok[1], &port, &bit)) { fprintf(out, "invalid pin: %s\n", tok[1]); return 0; }
        char *end = NULL;
        long level = strtol(tok[2], &end, 0);
        if (!end || *end || (level != 0 && level != 1)) { fprintf(out, "invalid level: %s\n", tok[2]); return 0; }
        int edge = cemu_soc_port_input_level(d->soc, port, bit, (int)level);
        fprintf(out, "pin P%d.%d = %ld (%s)\n", port, bit, level, port_edge_name(edge));
    } else if (!strcmp(cmd, "serial-rx")) {
        uint8_t bytes[DBG_SERIAL_RX_MAX];
        size_t count = 0;
        if (nt != 2 || !decode_serial_hex(nt > 1 ? tok[1] : "", bytes, &count)) {
            fprintf(out, "usage: serial-rx HEX (nonempty even-length hex, max %u bytes)\n",
                    DBG_SERIAL_RX_MAX);
            return 0;
        }
        cemu_soc_feed_serial(d->soc, bytes, count);
        serial_state_t *serial = (serial_state_t *)d->soc->serial_periph->state;
        size_t queued = d->soc->serial_rx_len - d->soc->serial_rx_head;
        fprintf(out, "serial-rx: fed=%zu queued=%zu in_flight=%d\n",
                count, queued, serial->rx_active ? 1 : 0);
    } else if (!strcmp(cmd, "edge")) {
        if (nt < 3) { fprintf(out, "usage: edge CCN rise|fall\n"); return 0; }
        int channel, rising;
        if (!parse_cc_channel(tok[1], &channel)) { fprintf(out, "invalid CAPCOM channel: %s\n", tok[1]); return 0; }
        if (!parse_rising_token(tok[2], &rising)) { fprintf(out, "invalid edge: %s\n", tok[2]); return 0; }
        cemu_soc_capcom_input_edge(d->soc, channel, rising);
        fprintf(out, "edge CC%d %s\n", channel, rising ? "rising" : "falling");
    } else if (!strcmp(cmd, "key")) {
        if (nt < 3) { fprintf(out, "usage: key NAME down|up\n"); return 0; }
        int pressed;
        if (!strcmp(tok[2], "down") || !strcmp(tok[2], "press")) pressed = 1;
        else if (!strcmp(tok[2], "up") || !strcmp(tok[2], "release")) pressed = 0;
        else { fprintf(out, "invalid key state: %s\n", tok[2]); return 0; }
        keypad_state_t *st = NULL;
        for (int i = 0; i < d->soc->n_peripherals; i++)
            if (!strcmp(d->soc->peripherals[i]->id, "keypad"))
                st = (keypad_state_t *)d->soc->peripherals[i]->state;
        const keypad_button_t *button = cemu_keypad_button_by_name(st, tok[1]);
        if (!button) { fprintf(out, "unknown key: %s\n", tok[1]); return 0; }
        if (!st || !st->nbuttons) {
            fprintf(out, "keypad input is not configured for this device\n");
            return 0;
        }
        cemu_keypad_set_button(st, d->soc, button->name, pressed);
        fprintf(out, "key %s %s raw=0x%04x logical=0x%02x\n",
                button->name, pressed ? "down" : "up",
                button->raw_code, button->logical_code);
    } else if (!strcmp(cmd, "irq")) {
        if (nt < 2) { fprintf(out, "usage: irq IRQ45|IRQ64..IRQ79|XP2 [ILVL]\n"); return 0; }
        int irq, trap; uint32_t addr;
        if (!parse_product_irq(tok[1], &irq, &addr, &trap)) { fprintf(out, "invalid product IRQ: %s\n", tok[1]); return 0; }
        int ilvl = nt > 2 ? (int)strtol(tok[2], NULL, 0) : 15;
        cemu_soc_debug_inject_irq(d->soc, addr, trap, ilvl);
        fprintf(out, "irq IRQ%d addr=0x%04x trap=0x%x ilvl=%d value=0x%04x\n",
                irq, addr, trap, ilvl < 0 ? 0 : ilvl > 15 ? 15 : ilvl, debugger_read_sfr(d, addr));
    } else if (!strcmp(cmd, "dis")) {
        debugger_disasm_last(d, sb, sizeof sb); fprintf(out, "%s\n", sb);
    } else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "di")) {   /* disasm ADDR [N] */
        if (nt < 2) { fprintf(out, "usage: disasm ADDR [N]\n"); return 0; }
        uint32_t a = parse_int24(tok[1]);
        int n = nt > 2 ? (int)parse_count(tok[2]) : 1;
        if (n < 1) n = 1;
        for (int i = 0; i < n; i++) {
            int len = debugger_disasm_at(d, a, sb, sizeof sb);
            fprintf(out, "0x%08x  %s\n", a, sb);
            if (len < 1) len = 1;
            a = (a & 0xFF0000) | ((a + len) & 0xFFFF);   /* advance within segment */
        }
    } else if (!strcmp(cmd, "checkpoint") || !strcmp(cmd, "cp")) {
        debugger_checkpoint(d);
        fprintf(out, "checkpoint saved @ icount=%llu\n", (unsigned long long)d->cpu->icount);
    } else if (!strcmp(cmd, "restore")) {
        if (debugger_restore(d)) {
            if (d->extension_restore)
                d->extension_restore(d->extension_opaque);
            fprintf(out, "restored to icount=%llu\n", (unsigned long long)d->cpu->icount);
        } else fprintf(out, "no checkpoint to restore\n");
    } else if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        dbg_help(out);
        if (d->extension_help)
            d->extension_help(d->extension_opaque, out);
    } else if (d->extension_command &&
               d->extension_command(d->extension_opaque, nt, tok, out)) {
        /* handled by the host extension */
    } else {
        fprintf(out, "unknown command: %s (try 'help')\n", cmd);
    }
    return 0;
}

int debugger_run_script(debugger_t *d, const char *script, FILE *out) {
    char *copy = strdup(script);
    if (!copy) return 0;
    /* Commands separated by ';' or newline. */
    int quit = 0;
    char *save = NULL;
    for (char *seg = strtok_r(copy, ";\n", &save); seg; seg = strtok_r(NULL, ";\n", &save)) {
        char line[512]; snprintf(line, sizeof line, "%s", seg);
        if (cooperate(d, 0)) { quit = 1; break; }
        if (run_command(d, line, out)) { quit = 1; break; }
    }
    if (cooperate(d, 0) < 0 || d->service_result == 2) quit = 1;
    free(copy);
    return quit;
}

void debugger_repl(debugger_t *d, FILE *in, FILE *out) {
    fprintf(out, "c55 debugger — 'help' lists commands, 'quit' exits\n");
    char line[512];
    for (;;) {
        int control = cooperate(d, 0);
        if (control < 0 || control == 2) break;
        fprintf(out, "(dbg pc=0x%08x) ", cpu_pc(d->cpu));
        fflush(out);
        if (!d->input_wait) {
            if (!fgets(line, sizeof line, in)) { fprintf(out, "\n"); break; }
        } else {
            size_t used = 0;
            int eof = 0;
            for (;;) {
                control = cooperate(d, 0);
                if (control) break;
                int ready = d->input_wait(d->input_wait_opaque, 10);
                if (ready < 0) { d->service_result = -1; return; }
                if (!ready) continue;
                int ch = fgetc(in);
                if (ch == EOF) {
                    if (ferror(in) && (errno == EAGAIN || errno == EINTR)) {
                        clearerr(in);
                        continue;
                    }
                    eof = 1; break;
                }
                if (ch == '\n') break;
                if (used < sizeof line - 1) line[used++] = (char)ch;
            }
            line[used] = 0;
            if (control < 0 || control == 2) break;
            if (control == 1) { fprintf(out, "\n"); continue; }
            if (eof && !used) { fprintf(out, "\n"); break; }
        }
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') line[len - 1] = 0;
        if (run_command(d, line, out)) break;
    }
    (void)cooperate(d, 0);
}
