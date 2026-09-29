/* Synthetic-behavior registry — see synth.h. */
#include "synth.h"
#include <stdio.h>
#include <string.h>

const synth_behavior_t synth_registry[SYN_COUNT] = {
    [SYN_SUPPRESS_EF_MAILBOX_RESPONSE] = {
        "suppress-ef-mailbox-response",
        "diagnostic pre-mailbox path: do not answer EF1C=0x003D",
        0 },
    [SYN_GSM] = {
        "gsm",
        "measured C55 DSP responses and registration/signal publication",
        0 },
    [SYN_XBUS_MAILBOX_COMPLETE_BIT1] = {
        "xbus-mailbox-complete-bit1",
        "diagnostic XBUS mailbox: add synchronous completion bit EC12.1",
        0 },
    [SYN_XBUS_MAILBOX_NO_COMPLETE_BIT2] = {
        "xbus-mailbox-no-complete-bit2",
        "diagnostic XBUS mailbox: suppress asynchronous completion bit EC12.2",
        0 },
    [SYN_XBUS_MAILBOX_NO_IRQ80] = {
        "xbus-mailbox-no-irq80",
        "diagnostic XBUS mailbox: suppress completion IRQ80",
        0 },
    [SYN_XBUS_MAILBOX_IMMEDIATE] = {
        "xbus-mailbox-immediate",
        "diagnostic XBUS mailbox: reproduce the old immediate responder",
        0 },
};

unsigned synth_defaults(void) {
    unsigned m = 0;
    for (int i = 0; i < SYN_COUNT; i++)
        if (synth_registry[i].dflt) m |= (1u << i);
    return m;
}

int synth_id_by_name(const char *name) {
    for (int i = 0; i < SYN_COUNT; i++)
        if (strcmp(name, synth_registry[i].name) == 0) return i;
    return -1;
}

int synth_parse(unsigned *mask, const char *csv, const char **bad) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", csv);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ' || *tok == '\t') tok++;
        int enable = 1;
        const char *nm = tok;
        if (strncmp(tok, "no-", 3) == 0) { enable = 0; nm = tok + 3; }
        int id = synth_id_by_name(nm);
        if (id < 0) { if (bad) *bad = tok; return -1; }
        if (enable) *mask |= (1u << id);
        else        *mask &= ~(1u << id);
    }
    return 0;
}

int synth_active_names(unsigned mask, char *buf, size_t cap) {
    size_t n = 0; int count = 0;
    if (cap) buf[0] = 0;
    for (int i = 0; i < SYN_COUNT; i++) {
        if (!((mask >> i) & 1)) continue;
        int w = snprintf(buf + n, n < cap ? cap - n : 0, "%s%s",
                         count ? "," : "", synth_registry[i].name);
        if (w > 0) n += (size_t)w;
        count++;
    }
    return count;
}
