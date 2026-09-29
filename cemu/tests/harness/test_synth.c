/* Synthetic-behavior registry tests (src/harness/synth.c). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "synth.h"
#include "soc.h"

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define FLEN (8u * 1024 * 1024)

static void test_defaults(void) {
    unsigned d = synth_defaults();
    CHECK(d == 0);
    /* defaults == exactly the per-entry dflt bits */
    unsigned expect = 0;
    for (int i = 0; i < SYN_COUNT; i++) if (synth_registry[i].dflt) expect |= (1u << i);
    CHECK(d == expect);
}

static void test_id_by_name(void) {
    CHECK(SYN_XBUS_MAILBOX_COMPLETE_BIT1 == 2);
    CHECK(SYN_XBUS_MAILBOX_IMMEDIATE == 5);
    CHECK(synth_id_by_name("suppress-ef-mailbox-response") == SYN_SUPPRESS_EF_MAILBOX_RESPONSE);
    CHECK(synth_id_by_name("gsm") == SYN_GSM);
    CHECK(synth_id_by_name("xbus-mailbox-complete-bit1") == SYN_XBUS_MAILBOX_COMPLETE_BIT1);
    CHECK(synth_id_by_name("xbus-mailbox-no-complete-bit2") == SYN_XBUS_MAILBOX_NO_COMPLETE_BIT2);
    CHECK(synth_id_by_name("xbus-mailbox-no-irq80") == SYN_XBUS_MAILBOX_NO_IRQ80);
    CHECK(synth_id_by_name("xbus-mailbox-immediate") == SYN_XBUS_MAILBOX_IMMEDIATE);
    CHECK(synth_id_by_name("nope") == -1);
}

static void test_parse_toggles(void) {
    unsigned m = synth_defaults();
    const char *bad = NULL;
    CHECK(synth_parse(&m, "suppress-ef-mailbox-response", &bad) == 0);
    CHECK((m >> SYN_SUPPRESS_EF_MAILBOX_RESPONSE) & 1);
    CHECK(synth_parse(&m, "no-suppress-ef-mailbox-response", &bad) == 0);
    CHECK(((m >> SYN_SUPPRESS_EF_MAILBOX_RESPONSE) & 1) == 0);
    CHECK(synth_parse(&m, "gsm", &bad) == 0);
    CHECK((m >> SYN_GSM) & 1);
    CHECK(synth_parse(&m, "no-gsm", &bad) == 0);
    CHECK(((m >> SYN_GSM) & 1) == 0);
    CHECK(synth_parse(&m, "xbus-mailbox-complete-bit1,xbus-mailbox-no-irq80", &bad) == 0);
    CHECK((m >> SYN_XBUS_MAILBOX_COMPLETE_BIT1) & 1);
    CHECK((m >> SYN_XBUS_MAILBOX_NO_IRQ80) & 1);
    CHECK(synth_parse(&m, "xbus-mailbox-no-complete-bit2", &bad) == 0);
    CHECK((m >> SYN_XBUS_MAILBOX_NO_COMPLETE_BIT2) & 1);
    CHECK(synth_parse(&m, "xbus-mailbox-immediate", &bad) == 0);
    CHECK((m >> SYN_XBUS_MAILBOX_IMMEDIATE) & 1);
}

static void test_parse_unknown_rejected(void) {
    unsigned m = synth_defaults();
    const char *bad = NULL;
    CHECK(synth_parse(&m, "bogus", &bad) == -1);
    CHECK(bad != NULL && strcmp(bad, "bogus") == 0);
    CHECK(m == synth_defaults());   /* mask unchanged on error path (first token) */
}

static void test_active_names(void) {
    char buf[128];
    int n = synth_active_names(synth_defaults(), buf, sizeof buf);
    CHECK(n == 0);
    CHECK(buf[0] == 0);
    unsigned enabled = (1u << SYN_SUPPRESS_EF_MAILBOX_RESPONSE) |
                       (1u << SYN_GSM);
    n = synth_active_names(enabled, buf, sizeof buf);
    CHECK(n == 2);
    CHECK(strstr(buf, "suppress-ef-mailbox-response") != NULL);
    CHECK(strstr(buf, "gsm") != NULL);
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
    {"defaults", test_defaults},
    {"id_by_name", test_id_by_name},
    {"parse_toggles", test_parse_toggles},
    {"parse_unknown_rejected", test_parse_unknown_rejected},
    {"active_names", test_active_names},
};

int main(void) {
    int n = (int)(sizeof(TESTS) / sizeof(TESTS[0]));
    for (int i = 0; i < n; i++) {
        g_fail = 0; TESTS[i].fn(); g_total_run++;
        if (g_fail) { g_total_fail++; printf("[FAIL] %s (%d)\n", TESTS[i].name, g_fail); }
    }
    printf("\n%d tests, %d failed\n", g_total_run, g_total_fail);
    return g_total_fail ? 1 : 0;
}
