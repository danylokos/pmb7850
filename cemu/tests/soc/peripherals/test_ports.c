/* P3/P6/P7/P8 port surface and explicit edge-injection probes. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "soc.h"
#include "synth.h"

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint8_t *make_flash(size_t len) {
    uint8_t *f = calloc(len, 1);
    memset(f, 0xFF, len);
    f[0] = 0xFA; f[1] = 0x80; f[2] = 0xC4; f[3] = 0x2F;
    return f;
}

static soc_t S;
static uint8_t *FL;
#define FLEN (8u * 1024 * 1024)

static void setup(int autobaud) {
    if (!FL) FL = make_flash(FLEN);
    device_config_t cfg = *cemu_device_by_name("c55");
    cemu_soc_init(&S, FL, FLEN, &cfg, synth_defaults(), autobaud);
}

static void set_cc_mode(int channel, int timer_idx, int mode) {
    uint32_t wa = (channel < 16) ? (0xFF52u + (uint32_t)(channel >> 2) * 2u)
                                 : (0xFF22u + (uint32_t)((channel - 16) >> 2) * 2u);
    int local = channel & 3;
    uint16_t field = (uint16_t)((((timer_idx & 1) << 3) | (mode & 7)) << (local * 4));
    uint16_t mask = (uint16_t)(0xFu << (local * 4));
    uint16_t cur = bus_read16(&S.bus, wa);
    bus_write16(&S.bus, wa, (uint16_t)((cur & ~mask) | field));
}

static void test_port_input_level_preserves_storage_until_driven(void) {
    setup(0);
    bus_write16(&S.bus, 0xFFD0, 0x00B0);      /* P7 output latch/storage */
    CHECK(bus_read16(&S.bus, 0xFFD0) == 0x00B0);

    CHECK(cemu_soc_port_input_level(&S, SOC_PORT_P7, 14, 1) == SOC_PORT_EDGE_RISING);
    CHECK(bus_read16(&S.bus, 0xFFD0) == 0x40B0);  /* DP7.14=0: external input visible */

    bus_write16(&S.bus, 0xFFD2, 0x4000);      /* DP7.14=1: output latch visible */
    CHECK(bus_read16(&S.bus, 0xFFD0) == 0x00B0);
    bus_write16(&S.bus, 0xFFD0, 0x40B0);
    CHECK(bus_read16(&S.bus, 0xFFD0) == 0x40B0);

    CHECK(cemu_soc_port_input_level(&S, SOC_PORT_P7, 14, 1) == SOC_PORT_EDGE_NONE);
    CHECK(cemu_soc_port_input_level(&S, SOC_PORT_P7, 14, 0) == SOC_PORT_EDGE_FALLING);
    bus_write16(&S.bus, 0xFFD2, 0);
    CHECK(bus_read16(&S.bus, 0xFFD0) == 0x00B0);
    cemu_soc_port_input_release(&S, SOC_PORT_P7, 14);
    CHECK(bus_read16(&S.bus, 0xFFD0) == 0x40B0);
    cemu_soc_free(&S);
}

static void test_p6_edge_forwards_to_capcom1_capture(void) {
    setup(0);
    bus_write16(&S.bus, 0xFE50, 0x1234);      /* T0 value */
    set_cc_mode(3, 0, 1);                     /* CC3 capture rising, allocated to T0 */
    bus_write16(&S.bus, 0xFF7E, 0x0000);      /* CC3IC */

    CHECK(cemu_soc_port_input_level(&S, SOC_PORT_P6, 3, 1) == SOC_PORT_EDGE_RISING);
    CHECK(bus_read16(&S.bus, 0xFE86) == 0x1234);   /* CC3 captured T0 */
    CHECK((bus_read16(&S.bus, 0xFF7E) & XIC_IR_BIT) != 0);
    CHECK((bus_read16(&S.bus, 0xFFCC) & 0x0008) != 0);
    cemu_soc_free(&S);
}

static void test_restore_level_does_not_create_capcom_edge(void) {
    setup(0);
    bus_write16(&S.bus, 0xFE50, 0x1234);
    set_cc_mode(3, 0, 1);
    bus_write16(&S.bus, 0xFF7E, 0x0040);

    cemu_soc_port_input_restore_level(&S, SOC_PORT_P6, 3, 1);
    CHECK((bus_read16(&S.bus, 0xFFCC) & 0x0008) != 0);
    CHECK(bus_read16(&S.bus, 0xFE86) == 0);
    CHECK((bus_read16(&S.bus, 0xFF7E) & XIC_IR_BIT) == 0);
    cemu_soc_free(&S);
}

static void test_p3_and_p8_inputs_respect_direction(void) {
    setup(0);
    bus_write16(&S.bus, 0xFFC4, 0x0002);
    bus_write16(&S.bus, 0xFFD4, 0x0000);

    CHECK(cemu_soc_port_input_level(&S, SOC_PORT_P3, 1, 0) ==
          SOC_PORT_EDGE_NONE);
    CHECK(cemu_soc_port_input_level(&S, SOC_PORT_P8, 14, 1) ==
          SOC_PORT_EDGE_RISING);
    CHECK((bus_read16(&S.bus, 0xFFC4) & 0x0002) == 0);
    CHECK((bus_read16(&S.bus, 0xFFD4) & 0x4000) != 0);

    bus_write16(&S.bus, 0xFFC6, 0x0002);
    bus_write16(&S.bus, 0xFFD6, 0x4000);
    CHECK((bus_read16(&S.bus, 0xFFC4) & 0x0002) != 0);
    CHECK((bus_read16(&S.bus, 0xFFD4) & 0x4000) == 0);

    CHECK(S.memory.sfr_hook[memory_controller_sfr_index(0xFFC6)] == NULL);
    cemu_soc_free(&S);
}

static void test_direct_cc5_edge_probe(void) {
    setup(0);
    bus_write16(&S.bus, 0xFE50, 0xABCD);      /* T0 value */
    set_cc_mode(5, 0, 2);                     /* CC5 capture falling, allocated to T0 */
    bus_write16(&S.bus, 0xFF82, 0x0000);      /* CC5IC */

    cemu_soc_capcom_input_edge(&S, 5, 1);
    CHECK((bus_read16(&S.bus, 0xFF82) & XIC_IR_BIT) == 0);
    cemu_soc_capcom_input_edge(&S, 5, 0);
    CHECK(bus_read16(&S.bus, 0xFE8A) == 0xABCD);
    CHECK((bus_read16(&S.bus, 0xFF82) & XIC_IR_BIT) != 0);
    cemu_soc_free(&S);
}

static void test_autobaud_bypass_toggles_p7_through_ports(void) {
    setup(1);
    CHECK(bus_read16(&S.bus, 0xFFD0) == 0x0008);
    CHECK(bus_read16(&S.bus, 0xFFD0) == 0x0000);
    CHECK(S.memory.sfr_hook[memory_controller_sfr_index(0xFFD0)] != NULL);
    CHECK(!strcmp(S.memory.sfr_hook[memory_controller_sfr_index(0xFFD0)]->id, "ports"));
    cemu_soc_free(&S);
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
    {"port_input_level_preserves_storage_until_driven", test_port_input_level_preserves_storage_until_driven},
    {"p6_edge_forwards_to_capcom1_capture", test_p6_edge_forwards_to_capcom1_capture},
    {"restore_level_does_not_create_capcom_edge", test_restore_level_does_not_create_capcom_edge},
    {"p3_and_p8_inputs_respect_direction", test_p3_and_p8_inputs_respect_direction},
    {"direct_cc5_edge_probe", test_direct_cc5_edge_probe},
    {"autobaud_bypass_toggles_p7_through_ports", test_autobaud_bypass_toggles_p7_through_ports},
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
