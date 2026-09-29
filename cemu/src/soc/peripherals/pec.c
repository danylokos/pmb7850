/* C166S Peripheral Event Controller, short and supported long transfers. */
#include <string.h>
#include "pec.h"
#include "soc.h"
#include "cemu_event.h"

#define EOPIC     0xF180u
#define PECISNC   0xFFA8u
#define PECXISNC  0xFFBAu

#define PECC_PT      (1u << 15)
#define PECC_EOPINT  (1u << 14)
#define PECC_PLEV_MASK 0x3000u
#define PECC_CL      (1u << 11)
#define PECC_INC_MASK 0x0600u
#define PECC_BWT     (1u << 8)
#define PECC_COUNT_MASK 0x00FFu

static const uint32_t PECC_ADDR[16] = {
    0xFEC0u, 0xFEC2u, 0xFEC4u, 0xFEC6u,
    0xFEC8u, 0xFECAu, 0xFECCu, 0xFECEu,
    0xFEE8u, 0xFEEAu, 0xFEECu, 0xFEEEu,
    0xFEF8u, 0xFEFAu, 0xFEFCu, 0xFEFEu,
};

static const uint32_t PECSN_ADDR[16] = {
    0xFED0u, 0xFED2u, 0xFED4u, 0xFED6u,
    0xFED8u, 0xFEDAu, 0xFEDCu, 0xFEDEu,
    0xFEE0u, 0xFEE2u, 0xFEE4u, 0xFEE6u,
    0xFEB8u, 0xFEBAu, 0xFEBCu, 0xFEBEu,
};

static const uint32_t SRCP_ADDR[16] = {
    0xFCE0u, 0xFCE4u, 0xFCE8u, 0xFCECu,
    0xFCF0u, 0xFCF4u, 0xFCF8u, 0xFCFCu,
    0xFCD0u, 0xFCD4u, 0xFCD8u, 0xFCDCu,
    0xFCC0u, 0xFCC4u, 0xFCC8u, 0xFCCCu,
};

static const uint32_t DSTP_ADDR[16] = {
    0xFCE2u, 0xFCE6u, 0xFCEAu, 0xFCEEu,
    0xFCF2u, 0xFCF6u, 0xFCFAu, 0xFCFEu,
    0xFCD2u, 0xFCD6u, 0xFCDAu, 0xFCDEu,
    0xFCC2u, 0xFCC6u, 0xFCCAu, 0xFCCEu,
};

/* PMB7850 exposes the optional long counter only for channels 0 and 2. */
static uint32_t pecxc_addr(int ch) {
    if (ch == 0) return 0xFEF0u;
    if (ch == 2) return 0xFEF2u;
    return 0;
}

static const uint32_t PEC_SFR_WORDS[] = {
    EOPIC,
    PECISNC, PECXISNC,
    0xFEB8u, 0xFEBAu, 0xFEBCu, 0xFEBEu,
    0xFEC0u, 0xFEC2u, 0xFEC4u, 0xFEC6u, 0xFEC8u, 0xFECAu, 0xFECCu, 0xFECEu,
    0xFED0u, 0xFED2u, 0xFED4u, 0xFED6u, 0xFED8u, 0xFEDAu, 0xFEDCu, 0xFEDEu,
    0xFEE0u, 0xFEE2u, 0xFEE4u, 0xFEE6u,
    0xFEE8u, 0xFEEAu, 0xFEECu, 0xFEEEu,
    0xFEF0u, 0xFEF2u,
    0xFEF8u, 0xFEFAu, 0xFEFCu, 0xFEFEu,
};

static const addr_range_t PEC_RANGES[] = {
    { 0xFCC0u, 0xFCFFu },
};

static const ic_node_t PEC_IC_NODES[] = {
    { EOPIC, 0x4C, 0 },
};

static const reg_name_t PEC_REG_NAMES[] = {
    { EOPIC, "EOPIC" },
    { PECISNC, "PECISNC" }, { PECXISNC, "PECXISNC" },
    { 0xFEB8u, "PECSN12" }, { 0xFEBAu, "PECSN13" },
    { 0xFEBCu, "PECSN14" }, { 0xFEBEu, "PECSN15" },
    { 0xFEC0u, "PECC0" }, { 0xFEC2u, "PECC1" },
    { 0xFEC4u, "PECC2" }, { 0xFEC6u, "PECC3" },
    { 0xFEC8u, "PECC4" }, { 0xFECAu, "PECC5" },
    { 0xFECCu, "PECC6" }, { 0xFECEu, "PECC7" },
    { 0xFED0u, "PECSN0" }, { 0xFED2u, "PECSN1" },
    { 0xFED4u, "PECSN2" }, { 0xFED6u, "PECSN3" },
    { 0xFED8u, "PECSN4" }, { 0xFEDAu, "PECSN5" },
    { 0xFEDCu, "PECSN6" }, { 0xFEDEu, "PECSN7" },
    { 0xFEE0u, "PECSN8" }, { 0xFEE2u, "PECSN9" },
    { 0xFEE4u, "PECSN10" }, { 0xFEE6u, "PECSN11" },
    { 0xFEE8u, "PECC8" }, { 0xFEEAu, "PECC9" },
    { 0xFEECu, "PECC10" }, { 0xFEEEu, "PECC11" },
    { 0xFEF0u, "PECXC0" }, { 0xFEF2u, "PECXC2" },
    { 0xFEF8u, "PECC12" }, { 0xFEFAu, "PECC13" },
    { 0xFEFCu, "PECC14" }, { 0xFEFEu, "PECC15" },
    { 0xFCC0u, "SRCP12" }, { 0xFCC2u, "DSTP12" },
    { 0xFCC4u, "SRCP13" }, { 0xFCC6u, "DSTP13" },
    { 0xFCC8u, "SRCP14" }, { 0xFCCAu, "DSTP14" },
    { 0xFCCCu, "SRCP15" }, { 0xFCCEu, "DSTP15" },
    { 0xFCD0u, "SRCP8" }, { 0xFCD2u, "DSTP8" },
    { 0xFCD4u, "SRCP9" }, { 0xFCD6u, "DSTP9" },
    { 0xFCD8u, "SRCP10" }, { 0xFCDAu, "DSTP10" },
    { 0xFCDCu, "SRCP11" }, { 0xFCDEu, "DSTP11" },
    { 0xFCE0u, "SRCP0" }, { 0xFCE2u, "DSTP0" },
    { 0xFCE4u, "SRCP1" }, { 0xFCE6u, "DSTP1" },
    { 0xFCE8u, "SRCP2" }, { 0xFCEAu, "DSTP2" },
    { 0xFCECu, "SRCP3" }, { 0xFCEEu, "DSTP3" },
    { 0xFCF0u, "SRCP4" }, { 0xFCF2u, "DSTP4" },
    { 0xFCF4u, "SRCP5" }, { 0xFCF6u, "DSTP5" },
    { 0xFCF8u, "SRCP6" }, { 0xFCFAu, "DSTP6" },
    { 0xFCFCu, "SRCP7" }, { 0xFCFEu, "DSTP7" },
};

static int pec_channel_ilvl(int ch, uint16_t pecc) {
    int plev = (pecc & PECC_PLEV_MASK) >> 12;
    return 0x8 | ((((~plev) >> 1) & 1) << 2) |
           (((~plev) & 1) << 1) | ((ch >> 2) & 1);
}

static int pec_channel_xglvl(int ch) {
    return (((ch >> 3) & 1) << 2) | (((ch >> 1) & 1) << 1) | (ch & 1);
}

static int pec_channel_for_request(soc_t *s, uint32_t ic_addr, int ilvl) {
    uint16_t ic = memory_controller_sfr_get(&s->memory, ic_addr);
    int xglvl = (((ic >> 8) & 1) << 2) | (ic & 0x3);
    for (int ch = 0; ch < 16; ch++) {
        uint16_t pecc = memory_controller_sfr_get(&s->memory, PECC_ADDR[ch]);
        if (pec_channel_ilvl(ch, pecc) == ilvl && pec_channel_xglvl(ch) == xglvl)
            return ch;
    }
    return -1;
}

static int pec_channel_has_finite_count(soc_t *s, int ch, uint16_t pecc) {
    if (pecc & PECC_CL) return 0;
    if (pecc & PECC_PT) {
        uint32_t counter_addr = pecxc_addr(ch);
        if (!counter_addr || (pecc & PECC_COUNT_MASK)) return 0;
        return memory_controller_sfr_get(&s->memory, counter_addr) != 0;
    }
    uint16_t count = pecc & PECC_COUNT_MASK;
    return count != 0 && count != PECC_COUNT_MASK;
}

int cemu_pec_channel_has_finite_transfer(
    pec_engine_t *pec, soc_t *s, uint32_t ic_addr, int ilvl) {
    (void)pec;
    int ch = pec_channel_for_request(s, ic_addr, ilvl);
    if (ch < 0) return 0;
    uint16_t pecc = memory_controller_sfr_get(&s->memory, PECC_ADDR[ch]);
    return pec_channel_has_finite_count(s, ch, pecc);
}

static void pec_refresh_eopic_from_subnodes(soc_t *s, uint32_t subnode_addr) {
    uint16_t sub = memory_controller_sfr_get(&s->memory, subnode_addr);
    for (int i = 0; i < 8; i++) {
        uint16_t ie = (uint16_t)(1u << (2 * i));
        uint16_t ir = (uint16_t)(1u << (2 * i + 1));
        if ((sub & ie) && (sub & ir)) {
            cemu_memory_controller_sfr_put(&s->memory, EOPIC, memory_controller_sfr_get(&s->memory, EOPIC) | XIC_IR_BIT);
            return;
        }
    }
}

static void pec_set_eop_subnode(soc_t *s, int ch) {
    uint32_t subnode_addr = ch < 8 ? PECISNC : PECXISNC;
    int local = ch & 7;
    uint16_t ir = (uint16_t)(1u << (2 * local + 1));
    uint16_t old = memory_controller_sfr_get(&s->memory, subnode_addr);
    cemu_memory_controller_sfr_put(&s->memory, subnode_addr, old | ir);
    if (old & (1u << (2 * local)))
        cemu_memory_controller_sfr_put(&s->memory, EOPIC, memory_controller_sfr_get(&s->memory, EOPIC) | XIC_IR_BIT);
}

static void pec_trace_transfer(soc_t *s, int ch, uint32_t ic_addr,
                               uint32_t src, uint32_t dst, int size,
                               uint32_t value, int count_before,
                               int count_after, const char *eop,
                               int is_long, uint32_t counter_addr) {
    if (!cemu_event_native_trace_active(&s->instrumentation, "pec_transfer"))
        return;
    cemu_native_trace_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = "pec_transfer";
    ev.icount = s->cpu ? s->cpu->icount : 0;
    ev.pc = s->cpu ? cpu_pc(s->cpu) : 0;
    ev.has_addr = 1; ev.addr = dst & 0xFFFFFFu;
    ev.has_size = 1; ev.size = size;
    ev.has_value = 1; ev.value = value;
    ev.detail = "PEC";
    cemu_event_field_i64(&ev.info, "channel", ch);
    cemu_event_field_i64(&ev.info, "count_after", count_after);
    cemu_event_field_i64(&ev.info, "count_before", count_before);
    cemu_event_field_i64(&ev.info, "counter", (long)(counter_addr & 0xFFFFFFu));
    cemu_event_field_i64(&ev.info, "dst", (long)(dst & 0xFFFFFFu));
    cemu_event_field_string(&ev.info, "eop", eop);
    cemu_event_field_i64(&ev.info, "ic", (long)(ic_addr & 0xFFFFFFu));
    cemu_event_field_string(&ev.info, "mode", is_long ? "long" : "short");
    cemu_event_field_i64(&ev.info, "src", (long)(src & 0xFFFFFFu));
    cemu_event_emit_native_trace(&s->instrumentation, &ev, CEMU_EVENT_PERIPHERAL);
}

static void pec_on_write(peripheral_t *self, soc_t *s,
                         uint32_t word_addr, uint16_t stored) {
    (void)self;
    (void)stored;
    if (word_addr == PECISNC || word_addr == PECXISNC)
        pec_refresh_eopic_from_subnodes(s, word_addr);
}

int cemu_pec_prepare_interrupt(
    pec_engine_t *pec, soc_t *s, uint32_t ic_addr, int ilvl,
    pec_transfer_t *transfer) {
    (void)pec;

    int ch = pec_channel_for_request(s, ic_addr, ilvl);
    if (ch < 0) return 0;

    uint16_t pecc = memory_controller_sfr_get(&s->memory, PECC_ADDR[ch]);
    if (pecc & PECC_CL) return 0;

    int is_long = (pecc & PECC_PT) != 0;
    uint32_t counter_addr = PECC_ADDR[ch];
    int count_before;
    if (is_long) {
        counter_addr = pecxc_addr(ch);
        /* COUNT must be zero in long mode; unsupported channels have no
         * product-specific extended counter and therefore fall through. */
        if (!counter_addr || (pecc & PECC_COUNT_MASK)) return 0;
        count_before = memory_controller_sfr_get(&s->memory, counter_addr);
    } else {
        count_before = pecc & PECC_COUNT_MASK;
    }
    if (count_before == 0) return 0;

    int size = (pecc & PECC_BWT) ? 1 : 2;
    uint16_t seg = memory_controller_sfr_get(&s->memory, PECSN_ADDR[ch]);
    uint16_t src_off = cemu_memory_controller_peek16(&s->memory, SRCP_ADDR[ch]);
    uint16_t dst_off = cemu_memory_controller_peek16(&s->memory, DSTP_ADDR[ch]);
    uint32_t src = (((uint32_t)(seg & 0x00FFu)) << 16) | src_off;
    uint32_t dst = (((uint32_t)((seg >> 8) & 0x00FFu)) << 16) | dst_off;

    int count_after = count_before;
    int clear_source_ir = 1;
    if (!is_long && count_before == 0xFF) {
        count_after = 0xFF;
    } else if (count_before > 1) {
        count_after = count_before - 1;
    } else {
        count_after = 0;
        if (!(pecc & PECC_EOPINT)) clear_source_ir = 0;
    }

    transfer->channel = ch;
    transfer->is_long = is_long;
    transfer->size = size;
    transfer->count_before = count_before;
    transfer->count_after = count_after;
    transfer->clear_source_ir = clear_source_ir;
    transfer->ic_addr = ic_addr;
    transfer->counter_addr = counter_addr;
    transfer->src = src;
    transfer->dst = dst;
    transfer->pecc = pecc;
    transfer->src_off = src_off;
    transfer->dst_off = dst_off;
    return 1;
}

void cemu_pec_execute_transfer(
    pec_engine_t *pec, soc_t *s, const pec_transfer_t *transfer) {
    (void)pec;
    int ch = transfer->channel;
    int size = transfer->size;
    int count_before = transfer->count_before;
    int count_after = transfer->count_after;
    int is_long = transfer->is_long;
    uint32_t counter_addr = transfer->counter_addr;
    uint32_t src = transfer->src;
    uint32_t dst = transfer->dst;
    uint16_t pecc = transfer->pecc;
    uint16_t src_off = transfer->src_off;
    uint16_t dst_off = transfer->dst_off;
    uint32_t value;
    if (size == 2) {
        value = bus_read16(&s->bus, src);
        bus_write16(&s->bus, dst, (uint16_t)value);
    } else {
        value = bus_read8(&s->bus, src);
        bus_write8(&s->bus, dst, (uint8_t)value);
    }

    int inc = (pecc & PECC_INC_MASK) >> 9;
    uint16_t step = (uint16_t)size;
    if (inc == 1) {
        bus_write16(&s->bus, DSTP_ADDR[ch], (uint16_t)(dst_off + step));
    } else if (inc == 2 || inc == 3) {
        bus_write16(&s->bus, SRCP_ADDR[ch], (uint16_t)(src_off + step));
    }

    if (is_long)
        cemu_memory_controller_sfr_put(&s->memory, counter_addr, (uint16_t)count_after);
    else
        cemu_memory_controller_sfr_put(&s->memory, PECC_ADDR[ch],
                    (uint16_t)((pecc & ~PECC_COUNT_MASK) | count_after));
    if (count_before == 1 && (pecc & PECC_EOPINT))
        pec_set_eop_subnode(s, ch);

    const char *eop = count_before == 1
                    ? ((pecc & PECC_EOPINT) ? "eopic" : "source")
                    : "none";
    pec_trace_transfer(s, ch, transfer->ic_addr, src, dst, size, value,
                       count_before, count_after, eop, is_long, counter_addr);
}

void cemu_pec_endpoint_init(peripheral_t *p, pec_engine_t *pec) {
    memset(pec, 0, sizeof(*pec));
    p->id = "pec";
    p->state = pec;
    p->sfr_words = PEC_SFR_WORDS;    p->n_sfr_words = (int)(sizeof(PEC_SFR_WORDS) / sizeof(PEC_SFR_WORDS[0]));
    p->byte_ranges = PEC_RANGES;     p->n_byte_ranges = (int)(sizeof(PEC_RANGES) / sizeof(PEC_RANGES[0]));
    p->ic_nodes = PEC_IC_NODES;      p->n_ic_nodes = (int)(sizeof(PEC_IC_NODES) / sizeof(PEC_IC_NODES[0]));
    PERIPHERAL_REG_NAMES(p, PEC_REG_NAMES, (int)(sizeof(PEC_REG_NAMES) / sizeof(PEC_REG_NAMES[0])));
    p->read8 = NULL; p->peek8 = NULL; p->write8 = NULL;
    p->read_sfr_word = NULL;
    p->on_sfr_poll = NULL;
    p->on_sfr_write = pec_on_write;
    p->tick = NULL; p->timer_running = NULL; p->ic_will_fire = NULL;
}
