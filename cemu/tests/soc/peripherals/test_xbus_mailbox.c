/* Firmware-calibrated mailbox modes, preemption and persistence. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cpu.h"
#include "debugger.h"
#include "snapshot.h"
#include "soc.h"
#include "state_digest.h"
#include "synth.h"
#include "xbus_unknown1.h"

static int failures;
#define CHECK(x) do { if (!(x)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
static uint8_t flash[8u * 1024u * 1024u];

static void setup(soc_t *s, cpu_t *cpu) {
    CHECK(cemu_soc_init(s, flash, sizeof flash, cemu_device_by_name("c55"),
                        synth_defaults(), 0).code == CEMU_STATUS_OK);
    cemu_cpu_init(cpu, &s->bus);
    cemu_soc_attach_cpu(s, cpu);
    cemu_cpu_reset(cpu);
    bus_write16(&s->bus, 0xec14, 2);
    bus_write16(&s->bus, 0xec80, 0x162);
    bus_write16(&s->bus, 0xec82, 0xa);
    bus_write16(&s->bus, 0xec84, 8);
    bus_write16(&s->bus, 0xf140, 0x51);
}

static void test_contract(unsigned mode) {
    soc_t s;
    cpu_t cpu;
    setup(&s, &cpu);
    peripheral_t *p = s.xbus_unknown1_periph;
    xbus_unknown1_state_t *st = p->state;
    bus_write16(&s.bus, 0xec10, 0x1700);
    bus_write16(&s.bus, 0xec12, 1);
    const unsigned delays[] = {31, 1, 1, 1967, 1000000};
    for (unsigned i = 0; i < sizeof delays / sizeof delays[0]; i++) {
        cemu_soc_tick(&s, delays[i]);
        CHECK(bus_read16(&s.bus, 0xec12) == 2);
        CHECK(bus_read16(&s.bus, 0xec16) == 0x10);
        CHECK(bus_read16(&s.bus, 0xf140) == 0x51);
        CHECK(p->next_event_ticks(p, &s) == UINT64_MAX);
        CHECK(!p->ic_will_fire(p, &s, 0xf140));
    }
    bus_write8(&s.bus, 0xec10, mode);
    bus_write16(&s.bus, 0xec82, 0);
    bus_write16(&s.bus, 0xec84, 0x163);
    bus_write8(&s.bus, 0xec12, 3);
    bus_write8(&s.bus, 0xec13, 0x80);
    cemu_soc_tick(&s, 2000);
    CHECK(st->control == 0x1700 && st->transaction_seq == 1);
    CHECK(bus_read16(&s.bus, 0xec12) == 0x8002);
    CHECK(bus_read16(&s.bus, 0xec16) == 0x10);
    bus_write16(&s.bus, 0xf140, 0x11); /* Masked IR still latches. */
    bus_write16(&s.bus, 0xec12, 1); /* Acknowledge and resubmit. */
    bus_write16(&s.bus, 0xec10, 0x1700);
    CHECK(st->control == (0x1700 | mode));
    CHECK(p->next_event_ticks(p, &s) == 32);
    CHECK(p->ic_will_fire(p, &s, 0xf140));
    CHECK(bus_read16(&s.bus, 0xec16) == 1);
    cemu_soc_tick(&s, 31);
    CHECK(bus_read16(&s.bus, 0xec12) == 2);
    CHECK(bus_read16(&s.bus, 0xf140) == 0x11);
    cemu_soc_tick(&s, 1);
    CHECK(bus_read16(&s.bus, 0xec12) == 4);
    CHECK(bus_read16(&s.bus, 0xf140) == 0x91);
    bus_write16(&s.bus, 0xec12, 1);
    cemu_soc_tick(&s, 2000);
    bus_write8(&s.bus, 0xec12, 0);
    CHECK(bus_read16(&s.bus, 0xf140) == 0x91); /* Unrelated IR survives. */
    bus_write16(&s.bus, 0xf140, 0x51);
    bus_write16(&s.bus, 0xec10, 0x1700 | mode);
    bus_write16(&s.bus, 0xec12, 1);
    bus_write8(&s.bus, 0xec12, 0);
    cemu_soc_tick(&s, 2000);
    CHECK(bus_read16(&s.bus, 0xec12) == 0);
    CHECK(bus_read16(&s.bus, 0xf140) == 0x51);
    CHECK(p->next_event_ticks(p, &s) == UINT64_MAX);
    cemu_soc_free(&s);
}

/* Same real guest sequence as QEMU's mailbox regression. M166 MOV/RETI;
 * TDMA priority 7, optional nested T3 priority 8, IRQ80 worker priority 4. */
static void test_cpu_preemption(unsigned mode, int nested) {
    const uint8_t main_code[] = {
        0xe6,0xf4,1,0, 0xf6,0xf4,0x12,0xec,
        0xe6,0xf4,0xdc,0, 0xf6,0xf4,0x86,0xf1,
        0xf2,0xf5,0x12,0xec, 0xf2,0xf6,0x16,0xec,
        0xf2,0xf7,0x40,0xf1, 0xe6,0xf4,0,0, 0xf6,0xf4,0x12,0xec,
    };
    const uint8_t worker[] = {
        0xe6,0xf9,1,0, 0xf2,0xf8,0x16,0xec,
        0xe6,0xf4,0,0, 0xf6,0xf4,0x12,0xec, 0xfb,0x88,
    };
    const uint8_t trigger[] = {0xe6,0xfb,0xe0,0, 0xf6,0xfb,0x62,0xff};
    memcpy(flash + 0x1000, main_code, sizeof main_code);
    memcpy(flash + 0x100, (uint8_t[]){0xfa,0x80,0,0x20}, 4);
    memcpy(flash + 0x8c, (uint8_t[]){0xfa,0x80,0,0x21}, 4);
    memcpy(flash + 0x140, (uint8_t[]){0xfa,0x80,0,0x22}, 4);
    size_t at = 0x2000;
    for (unsigned i = 0; i < 80; i++) {
        if (nested && i == 40) {
            memcpy(flash + at, trigger, sizeof trigger); at += sizeof trigger;
        }
        flash[at++] = 0xcc; flash[at++] = 0;
    }
    flash[at++] = 0xfb; flash[at] = 0x88;
    for (at = 0x2100; at < 0x21a0; at += 2) {
        flash[at] = 0xcc; flash[at + 1] = 0;
    }
    flash[0x21a0] = 0xfb; flash[0x21a1] = 0x88;
    memcpy(flash + 0x2200, worker, sizeof worker);
    soc_t s;
    cpu_t cpu;
    setup(&s, &cpu);
    bus_write16(&s.bus, 0xec10, mode);
    cemu_cpu_set_psw(&cpu, 0x800);
    cemu_cpu_set_gpr(&cpu, 9, 0);
    cpu.csp = 0x80; cpu.ip = 0x1000;
    for (unsigned i = 0; i < 400 && cpu_pc(&cpu) != 0x801024; i++)
        CHECK(cemu_cpu_step(&cpu) == STEP_OK);
    CHECK(cpu_pc(&cpu) == 0x801024);
    CHECK(cpu.icount > 80);
    CHECK(cpu.interrupts_delivered == 1u + nested + !!(mode & 5));
    CHECK(cemu_cpu_gpr(&cpu, 5) == (mode & 5 ? 0 : 2));
    CHECK(cemu_cpu_gpr(&cpu, 6) == 0x10);
    CHECK(cemu_cpu_gpr(&cpu, 7) == 0x51);
    CHECK(cemu_cpu_gpr(&cpu, 9) == !!(mode & 5));
    CHECK(bus_read16(&s.bus, 0xec12) == 0);
    CHECK(bus_read16(&s.bus, 0xf140) == 0x51);
    cemu_soc_free(&s);
}

static void replace_text(const char *dir, const char *old, const char *new) {
    char path[512];
    snprintf(path, sizeof path, "%s/snapshot.json", dir);
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL && strlen(old) == strlen(new));
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    char *text = calloc((size_t)size + 1, 1);
    rewind(f);
    CHECK(fread(text, 1, size, f) == (size_t)size);
    char *match = strstr(text, old);
    CHECK(match != NULL);
    if (match) {
        fseek(f, match - text, SEEK_SET);
        CHECK(fwrite(new, 1, strlen(new), f) == strlen(new));
    }
    free(text);
    fclose(f);
}

static void test_persistence(unsigned mode) {
    soc_t s;
    cpu_t cpu;
    setup(&s, &cpu);
    xbus_unknown1_state_t *st = s.xbus_unknown1_periph->state;
    bus_write16(&s.bus, 0xec10, mode);
    bus_write16(&s.bus, 0xec12, 1);
    cemu_soc_tick(&s, 7);
    bus_write16(&s.bus, 0xec10, mode ^ 5); /* Live mode differs at capture. */
    debugger_t d;
    cemu_debugger_init(&d, &cpu, &s, 0);
    uint64_t digest = cemu_state_digest(&cpu, &s);
    cemu_debugger_checkpoint(&d);
    st->control ^= 5;
    CHECK(cemu_state_digest(&cpu, &s) != digest);
    CHECK(cemu_debugger_restore(&d));
    CHECK(st->control == mode && cemu_state_digest(&cpu, &s) == digest);
    char dir[] = "/tmp/cemu-mailbox-state-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    /* Flip completion class, not merely one valid IRQ mode into the other. */
    st->control = mode & 5 ? mode & ~5u : mode | 1u;
    CHECK(cemu_snapshot_write_dir(dir, &cpu, &s, NULL) == 0);
    CHECK(cemu_snapshot_read_dir(dir, &cpu, &s) != 0);
    st->control = mode;
    CHECK(cemu_snapshot_write_dir(dir, &cpu, &s, NULL) == 0);
    bus_write16(&s.bus, 0xec12, 0);
    CHECK(cemu_snapshot_read_dir(dir, &cpu, &s) == 0);
    CHECK(st->control == mode && cemu_state_digest(&cpu, &s) == digest);
    cemu_soc_tick(&s, 25);
    CHECK(bus_read16(&s.bus, 0xec12) == (mode & 5 ? 4 : 2));
    CHECK(bus_read16(&s.bus, 0xf140) == (mode & 5 ? 0xd1 : 0x51));
    replace_text(dir, "\"control\"", "\"ignored\"");
    CHECK(cemu_snapshot_read_dir(dir, &cpu, &s) != 0);
    replace_text(dir, "\"schema\": 39", "\"schema\": 38");
    CHECK(cemu_snapshot_read_dir(dir, &cpu, &s) != 0);
    bus_write16(&s.bus, 0xec12, 0);
    CHECK(cemu_snapshot_write_dir(dir, &cpu, &s, NULL) == 0);
    replace_text(dir, "\"schema\": 39", "\"schema\": 38");
    CHECK(cemu_snapshot_read_dir(dir, &cpu, &s) == 0);
    CHECK(st->phase == XBUS_MAILBOX_IDLE && st->deadline == 0);
    DIR *directory = opendir(dir);
    struct dirent *entry;
    while (directory && (entry = readdir(directory))) {
        char path[768];
        snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        if (entry->d_name[0] != '.') unlink(path);
    }
    if (directory) closedir(directory);
    rmdir(dir);
    cemu_debugger_detach(&d);
    cemu_debugger_free(&d);
    cemu_soc_free(&s);
}

int main(int argc, char **argv) {
    memset(flash, 0xff, sizeof flash);
    memcpy(flash, (uint8_t[]){0xfa,0x80,0xc4,0x2f}, 4);
    const unsigned modes[] = {0x1700, 0x1701, 0x1704};
    for (unsigned i = 0; i < 3; i++) {
        for (int nested = 0; nested < 2; nested++)
            test_cpu_preemption(modes[i], nested);
        if (argc == 1) test_persistence(modes[i]);
    }
    if (argc == 1) { test_contract(1); test_contract(4); }
    (void)argv;
    printf("XBUS mailbox preemption/persistence: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
