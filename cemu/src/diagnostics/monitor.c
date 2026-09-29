/* Loop / progress detector — see monitor.h. Ported from
 * pemu/src/harness/monitor.py (RunMonitor). Model-specific progress checks
 * delegate to the SoC awaited-timer and finite-PEC predicates. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "monitor.h"
#include "names.h"

/* Core SFR word addresses read as normal housekeeping every instruction — not
 * firmware peripheral polling (mirror monitor.CORE_SFRS). */
static int is_core_sfr(uint32_t a) {
    switch (a) {
        case 0xFE00: case 0xFE02: case 0xFE04: case 0xFE06:  /* DPP0-3 */
        case 0xFE10: case 0xFE12: case 0xFE14: case 0xFE16:  /* CP,SP,STKOV,STKUN */
        case 0xFF10:                                          /* PSW */
            return 1;
    }
    return 0;
}

/* ---- 64-bit mix for fingerprint / digest hashing ---------------------- */
static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

/* ---- addrset (open addressing over 24-bit addrs) ---------------------- */
static void aset_init(addrset_t *s) { s->keys = NULL; s->hits = NULL; s->cap = 0; s->n = 0; }
static void aset_free(addrset_t *s) { free(s->keys); free(s->hits); aset_init(s); }

static void aset_grow(addrset_t *s) {
    size_t ncap = s->cap ? s->cap * 2 : 1024;
    uint32_t *nk = malloc(ncap * sizeof(uint32_t));
    uint64_t *nh = calloc(ncap, sizeof(uint64_t));
    for (size_t i = 0; i < ncap; i++) nk[i] = 0xFFFFFFFFu;
    for (size_t i = 0; i < s->cap; i++) {
        if (s->keys[i] == 0xFFFFFFFFu) continue;
        size_t j = (size_t)mix64(s->keys[i]) & (ncap - 1);
        while (nk[j] != 0xFFFFFFFFu) j = (j + 1) & (ncap - 1);
        nk[j] = s->keys[i]; nh[j] = s->hits[i];
    }
    free(s->keys); free(s->hits);
    s->keys = nk; s->hits = nh; s->cap = ncap;
}

/* Add addr (or bump its hit count); returns the post-increment hit count. */
static uint64_t aset_bump(addrset_t *s, uint32_t addr) {
    if ((s->n + 1) * 10 >= s->cap * 7) aset_grow(s);
    size_t j = (size_t)mix64(addr) & (s->cap - 1);
    while (s->keys[j] != 0xFFFFFFFFu) {
        if (s->keys[j] == addr) return ++s->hits[j];
        j = (j + 1) & (s->cap - 1);
    }
    s->keys[j] = addr; s->hits[j] = 1; s->n++;
    return 1;
}

/* ---- back-edge checkpoint table (open addressing) --------------------- */
#define BE_EMPTY 0xFFFFFFFFu
static void be_rehash(monitor_t *m, size_t ncap) {
    backedge_t *old = m->be; size_t ocap = m->be_cap;
    m->be = malloc(ncap * sizeof(backedge_t));
    for (size_t i = 0; i < ncap; i++) m->be[i].target = BE_EMPTY;
    m->be_cap = ncap;
    for (size_t i = 0; i < ocap; i++) {
        if (old[i].target == BE_EMPTY) continue;
        size_t j = (size_t)mix64(old[i].target) & (ncap - 1);
        while (m->be[j].target != BE_EMPTY) j = (j + 1) & (ncap - 1);
        m->be[j] = old[i];
    }
    free(old);
}
static backedge_t *be_find(monitor_t *m, uint32_t target, int create) {
    if (m->be_cap == 0) {
        if (!create) return NULL;
        be_rehash(m, 256);
    }
    /* Grow before insertion if load would exceed ~70% — an open-addressing
     * table that fills completely would loop forever probing for an empty slot.
     * (Python bounds this set to block_cap=64 by evicting the least-hit target;
     * we grow instead — same verdicts, a little more memory.) */
    if (create && (m->be_n + 1) * 10 >= m->be_cap * 7) be_rehash(m, m->be_cap * 2);
    size_t j = (size_t)mix64(target) & (m->be_cap - 1);
    while (m->be[j].target != BE_EMPTY) {
        if (m->be[j].target == target) return &m->be[j];
        j = (j + 1) & (m->be_cap - 1);
    }
    if (!create) return NULL;
    backedge_t *e = &m->be[j];
    memset(e, 0, sizeof *e);
    e->target = target;
    m->be_n++;
    return e;
}

/* ---- lifecycle -------------------------------------------------------- */
static void monitor_instruction_event(void *ctx, const cemu_event_t *event);

void monitor_init(monitor_t *m, soc_t *soc, cpu_t *cpu,
                  uint64_t loop_threshold, uint64_t stall_window) {
    memset(m, 0, sizeof *m);
    m->soc = soc; m->cpu = cpu;
    m->loop_threshold = loop_threshold ? loop_threshold : 256;
    m->max_interval = 65536;
    m->block_cap = 64;
    m->stall_window = stall_window ? stall_window : 2000000;
    aset_init(&m->branch_targets);
    aset_init(&m->exec_blocks);
    m->sp_min = -1; m->sp_max = -1;
    m->ip0 = cpu->ip; m->csp0 = cpu->csp;
    m->prev_irq = cpu->interrupts_delivered;
    m->prev_traps = cpu->traps_taken;
    m->stall_footprint = soc->memory.mem_write_footprint;
    m->stall_external_ram_mutations =
        cemu_memory_controller_external_ram_mutation_seq(&soc->memory);
    m->instrumentation_subscription = cemu_event_subscribe(
        &soc->instrumentation, CEMU_EVENT_INSTRUCTION,
        monitor_instruction_event, m);
}

void monitor_free(monitor_t *m) {
    if (m->instrumentation_subscription) {
        cemu_event_unsubscribe(&m->soc->instrumentation,
                                    m->instrumentation_subscription);
        m->instrumentation_subscription = 0;
    }
    aset_free(&m->branch_targets);
    aset_free(&m->exec_blocks);
    for (size_t i = 0; i < m->be_cap; i++) {
        if (m->be[i].target == BE_EMPTY) continue;
        free(m->be[i].sfr_base); free(m->be[i].dpram_base);
    }
    free(m->be);
    free(m->stall_read_counts);
    m->be = NULL; m->be_cap = m->be_n = 0; m->stall_read_counts = NULL;
}

/* ---- fingerprint (event-free CPU state) ------------------------------- */
static uint64_t snapshot_fp(monitor_t *m) {
    soc_t *s = m->soc;
    uint16_t cp = cemu_memory_controller_peek16(&s->memory, 0xFE10);
    uint64_t h = 0;
    for (int n = 0; n < 16; n++)
        h = mix64(h ^ cemu_memory_controller_peek16(&s->memory, (cp + 2 * n) & 0xFFFF));
    for (int n = 0; n < 4; n++)
        h = mix64(h ^ cemu_memory_controller_peek16(&s->memory, 0xFE00 + 2 * n)); /* DPP0..DPP3 */
    h = mix64(h ^ cemu_memory_controller_peek16(&s->memory, 0xFF10));   /* PSW */
    h = mix64(h ^ cemu_memory_controller_peek16(&s->memory, 0xFE12));   /* SP */
    return h;
}

/* Total non-core SFR/ESFR and internal-I/O reads. */
static uint64_t peripheral_reads(monitor_t *m) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < SFR_WORDS; i++) {
        uint64_t c = m->soc->memory.sfr_read_counts[i];
        if (!c) continue;
        uint32_t a = SFR_BASE + 2 * i;
        if (!is_core_sfr(a)) total += c;
    }
    for (uint32_t i = 0; i < INTERNAL_IO_WORDS; i++)
        total += m->soc->memory.internal_io_read_counts[i];
    return total;
}

/* Value-digest of non-core SFRs and internal-I/O cells polled since `base`.
 * Scoped to cells whose read count ROSE vs base[] this window (base==NULL => all
 * currently-read count as polled). Returns the digest; fills `polled` (addrs
 * polled this window, for the awaited-timer check) and, if snap!=NULL, snapshots
 * the current counts as the next window's baseline. */
static uint64_t polled_digest(monitor_t *m, const uint32_t *base, uint32_t *snap,
                              uint32_t *polled, int *n_polled, int max_polled) {
    uint64_t d = 0;
    int np = 0;
    for (uint32_t i = 0; i < SFR_WORDS; i++) {
        uint32_t c = m->soc->memory.sfr_read_counts[i];
        uint32_t prev = base ? base[i] : 0;   /* read baseline BEFORE snap clobbers it (they may alias) */
        if (snap) snap[i] = c;
        if (!c) continue;
        uint32_t a = SFR_BASE + 2 * i;
        if (is_core_sfr(a)) continue;
        if (c > prev) {   /* polled in this window */
            d ^= ((uint64_t)a << 16) | (memory_controller_sfr_get(&m->soc->memory, a) & 0xFFFF);
            if (np < max_polled) polled[np++] = a;
        }
    }
    for (uint32_t i = 0; i < INTERNAL_IO_WORDS; i++) {
        uint32_t bi = SFR_WORDS + i;
        uint32_t c = m->soc->memory.internal_io_read_counts[i];
        uint32_t prev = base ? base[bi] : 0;
        if (snap) snap[bi] = c;
        if (!c) continue;
        uint32_t a = INTERNAL_IO_START + 2 * i;
        if (c > prev) {
            d ^= ((uint64_t)a << 16) | (cemu_memory_controller_peek16(&m->soc->memory, a) & 0xFFFF);
            if (np < max_polled) polled[np++] = a;
        }
    }
    *n_polled = np;
    return d;
}

/* DPRAM hot-cell exclusion (mirror _hot_dpram_addrs): the live GPR bank
 * [CP, CP+32) as 16 word cells, plus the core cells (CP/SP/DPP...). */
static int is_hot_dpram(uint32_t a, uint16_t cp) {
    for (int n = 0; n < 16; n++) if (a == (uint32_t)((cp + 2 * n) & 0xFFFF)) return 1;
    return is_core_sfr(a);
}

/* DPRAM poll signal (mirror _dpram_poll_signal): total reads excl. hot cells +
 * value digest over DPRAM cells polled since `base` (rose this window). Snaps
 * current (non-hot) counts into `snap` if non-NULL for the next baseline. */
static uint64_t dpram_poll_signal(monitor_t *m, const uint32_t *base, uint32_t *snap,
                                  uint64_t *total_out) {
    uint16_t cp = cemu_memory_controller_peek16(&m->soc->memory, 0xFE10);
    uint64_t total = 0, digest = 0;
    for (uint32_t i = 0; i < DPRAM_WORDS; i++) {
        uint32_t c = m->soc->memory.dpram_read_counts[i];
        uint32_t prev = base ? base[i] : 0;   /* read before snap clobbers (may alias) */
        uint32_t a = DPRAM_START + 2 * i;
        int hot = is_hot_dpram(a, cp);
        if (snap) snap[i] = hot ? 0 : c;
        if (!c || hot) continue;
        total += c;
        if (c > prev)
            digest ^= ((uint64_t)a << 16) | (cemu_memory_controller_peek16(&m->soc->memory, a) & 0xFFFF);
    }
    *total_out = total;
    return digest;
}

/* ---- back-edge handler (mirror _on_back_edge) ------------------------- */
static int on_back_edge(monitor_t *m, uint32_t target, loop_report_t *out) {
    backedge_t *e = be_find(m, target, 1);
    e->hits++;
    uint64_t n = e->hits;

    uint64_t due = e->next_checkpoint ? e->next_checkpoint : m->loop_threshold;
    if (n < due) return 0;

    uint64_t fp = snapshot_fp(m);
    uint64_t writes = m->soc->memory.mem_write_seq;
    uint64_t write_blocks = m->soc->memory.mem_write_footprint;
    uint64_t external_ram_mutations =
        cemu_memory_controller_external_ram_mutation_seq(&m->soc->memory);
    uint64_t reads = peripheral_reads(m);
    /* Lazily allocate this target's per-address read-count baselines; first
     * checkpoint reads NULL (everything counts as polled), then snapshots. */
    if (!e->sfr_base)
        e->sfr_base = calloc(SFR_WORDS + INTERNAL_IO_WORDS,
                             sizeof(uint32_t));
    if (!e->dpram_base) e->dpram_base = calloc(DPRAM_WORDS, sizeof(uint32_t));
    uint32_t polled[256]; int n_polled = 0;
    /* base and snap alias the same array: polled_digest reads base[i] before
     * writing snap[i] per index, so the in-place update is safe. On the first
     * checkpoint has_fp is 0 so the (all-polled) digest is only stored, never
     * compared, matching Python's since=None first pass. */
    uint64_t sfr_digest = polled_digest(m, e->has_fp ? e->sfr_base : NULL,
                                        e->sfr_base, polled, &n_polled, 256);
    uint64_t dpram_total = 0;
    uint64_t dpram_digest = dpram_poll_signal(m, e->has_fp ? e->dpram_base : NULL,
                                              e->dpram_base, &dpram_total);

    const char *verdict = NULL;
    int idle_wait = m->cpu->idle;
    if (e->has_fp && !idle_wait) {
        int no_write = write_blocks == e->write_blocks &&
            external_ram_mutations == e->external_ram_mutations;
        int no_irq = e->irq == m->cpu->interrupts_delivered;
        int no_polled_change = sfr_digest == e->sfr_digest;
        int no_dpram_change = dpram_digest == e->dpram_digest;
        int timed_wait = cemu_soc_awaited_timer_will_fire(m->soc, polled, n_polled);
        int finite_pec = cemu_soc_finite_pec_will_progress(m->soc);
        if (fp == e->fp && no_write && no_irq && no_polled_change &&
            no_dpram_change && !timed_wait && !finite_pec) {
            int sfr_polled = reads > e->reads;
            int dpram_polled = dpram_total > e->dpram_reads;
            if (sfr_polled) verdict = "waiting_io";
            else if (dpram_polled) verdict = "waiting_dpram";
            else verdict = "spin";
        }
    }
    e->has_fp = 1;
    e->fp = fp; e->writes = writes; e->write_blocks = write_blocks;
    e->external_ram_mutations = external_ram_mutations;
    e->irq = m->cpu->interrupts_delivered; e->reads = reads;
    e->sfr_digest = sfr_digest; e->dpram_reads = dpram_total; e->dpram_digest = dpram_digest;
    uint64_t interval = n < m->max_interval ? n : m->max_interval;
    e->next_checkpoint = n + interval;

    if (!verdict) return 0;
    uint16_t psw = cemu_memory_controller_peek16(&m->soc->memory, 0xFF10);
    out->verdict = verdict;
    out->back_edge_pc = target;
    out->iterations = n;
    out->cpu_ilvl = (psw >> 12) & 0xF;
    out->cpu_ien = (psw >> 11) & 1;
    return 1;
}

/* ---- global stall watchdog (mirror _check_stall / _stall_report) ------ */
static int check_stall(monitor_t *m, loop_report_t *out) {
    if (m->stall_window == 0) return 0;
    m->stall_steps++;
    if (m->stall_steps < m->stall_window) return 0;
    uint64_t blocks = m->branch_targets.n;
    uint64_t footprint = m->soc->memory.mem_write_footprint;
    uint64_t external_ram_mutations =
        cemu_memory_controller_external_ram_mutation_seq(&m->soc->memory);
    int progressed = blocks > m->stall_blocks ||
        footprint > m->stall_footprint ||
        external_ram_mutations > m->stall_external_ram_mutations;
    if (!m->stall_read_counts)
        m->stall_read_counts =
            calloc(SFR_WORDS + INTERNAL_IO_WORDS, sizeof(uint32_t));
    uint32_t polled[256]; int n_polled = 0;
    int had_base = m->stall_blocks || m->stall_footprint || m->stall_steps;  /* not first window */
    (void)polled_digest(m, had_base ? m->stall_read_counts : NULL,
                        m->stall_read_counts, polled, &n_polled, 256);
    m->stall_steps = 0;
    m->stall_blocks = blocks;
    m->stall_footprint = footprint;
    m->stall_external_ram_mutations = external_ram_mutations;
    if (progressed) return 0;
    if (m->cpu->idle) return 0;
    if (cemu_soc_awaited_timer_will_fire(m->soc, polled, n_polled)) return 0;
    if (cemu_soc_finite_pec_will_progress(m->soc)) return 0;
    /* wedged: classify like a back-edge spin */
    uint32_t target = ((uint32_t)m->cpu->csp << 16) | m->cpu->ip;
    uint64_t reads = peripheral_reads(m);
    uint64_t dpram_total = 0; dpram_poll_signal(m, NULL, NULL, &dpram_total);
    const char *verdict = reads > 0 ? "waiting_io" : (dpram_total > 0 ? "waiting_dpram" : "spin");
    uint16_t psw = cemu_memory_controller_peek16(&m->soc->memory, 0xFF10);
    out->verdict = verdict;
    out->back_edge_pc = target;
    out->iterations = m->stall_window;
    out->cpu_ilvl = (psw >> 12) & 0xF;
    out->cpu_ien = (psw >> 11) & 1;
    return 1;
}

/* ---- per-step hot path (mirror observe) ------------------------------- */
int monitor_observe(monitor_t *m, loop_report_t *out) {
    cpu_t *cpu = m->cpu;
    uint8_t csp1 = cpu->csp; uint32_t ip1 = cpu->ip;
    uint64_t irq = cpu->interrupts_delivered, traps = cpu->traps_taken;

    uint32_t prev_block = ((uint32_t)m->csp0 << 16) | m->ip0;
    aset_bump(&m->exec_blocks, prev_block);
    m->seg_exec[m->csp0]++;

    int sp = cemu_memory_controller_peek16(&m->soc->memory, 0xFE12);
    if (m->sp_min < 0 || sp < m->sp_min) m->sp_min = sp;
    if (m->sp_max < 0 || sp > m->sp_max) m->sp_max = sp;

    int vectored = irq != m->prev_irq || traps != m->prev_traps;
    int same_seg = csp1 == m->csp0;
    int seq = same_seg && (ip1 == ((m->ip0 + 2) & 0xFFFF) || ip1 == ((m->ip0 + 4) & 0xFFFF));
    int fired = 0;

    if (!seq) {
        uint32_t p1 = ((uint32_t)csp1 << 16) | ip1;
        aset_bump(&m->branch_targets, p1);
        int back_edge = same_seg && ip1 <= m->ip0;
        if (back_edge && !vectored) {
            fired = on_back_edge(m, p1, out);
        } else if (!back_edge) {
            /* Forward transfer through this target resets its loop counter
             * (mirror _forget). Reset in place rather than removing the slot —
             * clearing it to BE_EMPTY would punch a hole in the probe chain and
             * strand later-inserted colliding keys. */
            backedge_t *e = be_find(m, p1, 0);
            if (e) { e->hits = 0; e->next_checkpoint = 0; e->has_fp = 0; }
        }
    }

    m->ip0 = ip1; m->csp0 = csp1;
    m->prev_irq = irq; m->prev_traps = traps;
    if (!fired) fired = check_stall(m, out);
    return fired;
}

/* ---- metric accessors ------------------------------------------------- */
size_t monitor_branch_targets(const monitor_t *m) { return m->branch_targets.n; }
size_t monitor_basic_blocks(const monitor_t *m) { return m->exec_blocks.n; }

int monitor_hot_blocks(const monitor_t *m, int n, uint32_t *out_addr, uint64_t *out_hits) {
    /* selection: walk the branch_targets map, keep the top-n by hits. */
    int found = 0;
    const addrset_t *s = &m->branch_targets;
    for (size_t i = 0; i < s->cap; i++) {
        if (s->keys[i] == 0xFFFFFFFFu) continue;
        uint32_t a = s->keys[i]; uint64_t h = s->hits[i];
        /* insert into the sorted top-n (descending by hits, then addr asc) */
        int pos = found;
        while (pos > 0 && (out_hits[pos - 1] < h ||
               (out_hits[pos - 1] == h && out_addr[pos - 1] > a))) pos--;
        if (pos >= n) continue;
        int last = found < n ? found : n - 1;
        for (int k = last; k > pos; k--) { out_hits[k] = out_hits[k - 1]; out_addr[k] = out_addr[k - 1]; }
        out_hits[pos] = h; out_addr[pos] = a;
        if (found < n) found++;
    }
    return found;
}

static void monitor_instruction_event(void *ctx, const cemu_event_t *event) {
    (void)event;
    monitor_t *m = (monitor_t *)ctx;
    loop_report_t report;
    if (!m->verdict_pending && monitor_observe(m, &report)) {
        m->pending_verdict = report;
        m->verdict_pending = 1;
    }
}

int monitor_take_verdict(monitor_t *m, loop_report_t *out) {
    if (!m->verdict_pending) return 0;
    if (out) *out = m->pending_verdict;
    m->verdict_pending = 0;
    return 1;
}
