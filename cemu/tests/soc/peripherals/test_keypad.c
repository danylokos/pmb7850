/* Shared PMB7850 keypad mapping, startup, and persistence tests. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "debugger.h"
#include "keypad.h"
#include "snapshot.h"
#include "soc.h"
#include "state_digest.h"
#include "synth.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define FLEN (8u * 1024u * 1024u)
static uint8_t *flash;

static void ensure_flash(void) {
    if (flash) return;
    flash = malloc(FLEN);
    CHECK(flash != NULL);
    if (!flash) return;
    memset(flash, 0xFF, FLEN);
    memcpy(flash, (uint8_t[]){0xFA, 0x80, 0xC4, 0x2F}, 4);
}

static int setup_device(soc_t *soc, const char *name, unsigned synth_mask) {
    ensure_flash();
    const device_config_t *base = cemu_device_by_name("c55");
    const device_config_t *device = cemu_device_by_name(name);
    CHECK(base != NULL && device != NULL && flash != NULL);
    if (!base || !device || !flash) return 0;
    device_config_t cfg = *base;
    cfg.name = name;
    cfg.keypad = device->keypad;
    cfg.irq55_sources = device->irq55_sources;
    memset(&cfg.lcd, 0, sizeof cfg.lcd);
    cemu_status_t status = cemu_soc_init(soc, flash, FLEN, &cfg,
                                         synth_mask, 0);
    CHECK(status.code == CEMU_STATUS_OK);
    return status.code == CEMU_STATUS_OK;
}

static keypad_state_t *keypad(soc_t *soc) {
    return soc->keypad_periph ? soc->keypad_periph->state : NULL;
}

static uint32_t button_bit(const keypad_state_t *st, const char *name) {
    const keypad_button_t *button = cemu_keypad_button_by_name(st, name);
    CHECK(button != NULL);
    return button ? 1u << (button - st->buttons) : 0;
}

static void consume_startup(soc_t *soc, keypad_state_t *st) {
    bus_write16(&soc->bus, KEYPAD_SCAN_COMMAND, st->scan_command);
    CHECK(bus_read16(&soc->bus, KEYPAD_SCAN_RESULT) ==
          cemu_keypad_startup_power_result(st));
    CHECK(!st->startup_power_pending);
}

static void check_mutable_equal(const keypad_mutable_state_t *a,
                                const keypad_mutable_state_t *b) {
#define SAME(field) CHECK(a->field == b->field)
    SAME(scans); SAME(handled_scans); SAME(results); SAME(startup_releases);
    SAME(pressed); SAME(sampled_pressed); SAME(last_command); SAME(last_result);
    SAME(startup_power_pending); SAME(startup_result_tagged);
    SAME(startup_result_read_mask); SAME(matrix_scan_phase);
    SAME(release_activity_pending);
#undef SAME
}

typedef struct {
    unsigned releases;
    unsigned inputs;
} event_counts_t;

static int keypad_event_filter(void *opaque, const char *kind) {
    (void)opaque;
    return !strncmp(kind, "keypad_", 7);
}

static void count_keypad_events(void *opaque, const cemu_event_t *event) {
    event_counts_t *counts = opaque;
    if (event->type != CEMU_EVENT_PERIPHERAL) return;
    const char *kind = event->as.peripheral.trace->kind;
    if (!strcmp(kind, "keypad_startup_release")) counts->releases++;
    if (!strcmp(kind, "keypad_input")) counts->inputs++;
}

static void test_all_device_keymaps(void) {
    static const struct { const char *name; size_t count; int dp3; } devices[] = {
        {"c55", 18, 0}, {"a52", 18, 0}, {"a55", 18, 0},
        {"a60", 20, 1}, {"a62", 20, 1}, {"a65", 20, 1},
        {"m55", 20, 1}, {"mc60", 18, 1}, {"c60", 20, 1},
        {"cf62", 20, 1}, {"s55", 20, 1}, {"sl55", 20, 1},
    };
    for (size_t d = 0; d < sizeof devices / sizeof devices[0]; d++) {
        soc_t soc;
        if (!setup_device(&soc, devices[d].name, synth_defaults())) continue;
        keypad_state_t *st = keypad(&soc);
        CHECK(st != NULL);
        CHECK(st->nbuttons == devices[d].count);
        CHECK(!!st->matrix_select_addr == devices[d].dp3);
        CHECK(cemu_keypad_startup_power_result(st) == 0x000D);
        CHECK(cemu_keypad_button_by_name(st, "power")->logical_code == 0x0C);
        consume_startup(&soc, st);
        for (size_t i = 0; i < st->nbuttons; i++) {
            const keypad_button_t *button = &st->buttons[i];
            uint16_t raw = button->raw_code;
            CHECK(button->name != NULL);
            CHECK(cemu_keypad_button_by_name(st, button->name) == button);
            CHECK(cemu_keypad_set_button(st, &soc, button->name, 1) == 1);
            if (st->activity_ic_addr)
                CHECK(memory_controller_sfr_get(&soc.memory,
                      st->activity_ic_addr) & XIC_IR_BIT);
            if (st->matrix_select_addr) {
                bus_write16(&soc.bus, st->matrix_select_addr,
                            st->matrix_select_mask);
                bus_write16(&soc.bus, st->matrix_select_addr, 0);
                uint16_t probe = bus_read16(&soc.bus, KEYPAD_SCAN_RESULT);
                if (raw >> 4) {
                    CHECK(probe == st->matrix_idle_result);
                    bus_write16(&soc.bus, st->matrix_select_addr, raw >> 4);
                    CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) ==
                          (uint16_t)((st->matrix_idle_result & 0xFFF0u) |
                          ((st->matrix_idle_result & 0x000Fu) &
                           (raw & 0x000Fu))));
                } else {
                    CHECK(probe ==
                          (uint16_t)((st->matrix_idle_result & 0xFFF0u) |
                          ((st->matrix_idle_result & 0x000Fu) &
                           (raw & 0x000Fu))));
                }
            } else {
                uint16_t command = raw == 0x000D ? st->scan_command :
                    (uint16_t)(~(raw >> 4) & 0x00FDu);
                bus_write16(&soc.bus, KEYPAD_SCAN_COMMAND, command);
                CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) ==
                      (uint16_t)((st->matrix_idle_result & 0xFFF0u) |
                      ((st->matrix_idle_result & 0x000Fu) &
                       (raw & 0x000Fu))));
            }
            CHECK(st->sampled_pressed & (1u << i));
            CHECK(cemu_keypad_set_button(st, &soc, button->name, 0) == 1);
        }
        if (!strcmp(devices[d].name, "cf62")) {
            const keypad_button_t *soft_left =
                cemu_keypad_button_by_name(st, "soft-left");
            CHECK(soft_left && soft_left->raw_code == 0x001E);
            CHECK(soft_left && soft_left->logical_code == 0x01);
        }
        cemu_soc_free(&soc);
    }
}

static void test_startup_consumption_lifecycle(void) {
    soc_t soc;
    if (!setup_device(&soc, "c55", synth_defaults())) return;
    keypad_state_t *st = keypad(&soc);
    event_counts_t counts = {0};
    CHECK(cemu_event_subscribe_filtered(
        &soc.instrumentation, CEMU_EVENT_PERIPHERAL,
        count_keypad_events, keypad_event_filter, &counts) != 0);

    CHECK(st->startup_power_pending);
    for (int i = 0; i < 3; i++) {
        bus_write16(&soc.bus, KEYPAD_SCAN_COMMAND, st->scan_command);
        CHECK(cemu_memory_controller_peek16(&soc.memory,
                                             KEYPAD_SCAN_RESULT) == 0x000D);
        CHECK(st->startup_power_pending);
        CHECK(st->startup_result_read_mask == 0);
    }
    CHECK(bus_read8(&soc.bus, KEYPAD_SCAN_RESULT) == 0x0D);
    CHECK(st->startup_result_read_mask == 1);
    CHECK(cemu_memory_controller_peek8(&soc.memory,
                                       KEYPAD_SCAN_RESULT + 1u) == 0);
    CHECK(st->startup_result_read_mask == 1);

    bus_write16(&soc.bus, 0xFD20, 0xFFFF);
    bus_write16(&soc.bus, 0xFD12, 0x0100);
    CHECK(st->startup_power_pending);
    bus_write16(&soc.bus, KEYPAD_SCAN_COMMAND, st->scan_command);
    CHECK(st->startup_result_read_mask == 0);
    CHECK(bus_read8(&soc.bus, KEYPAD_SCAN_RESULT + 1u) == 0);
    CHECK(st->startup_result_read_mask == 2);
    CHECK(st->startup_power_pending);

    CHECK(cemu_keypad_set_button(st, &soc, "power", 1) == 1);
    CHECK(bus_read8(&soc.bus, KEYPAD_SCAN_RESULT) == 0x0D);
    CHECK(!st->startup_power_pending);
    CHECK(!st->startup_result_tagged);
    CHECK(st->startup_result_read_mask == 3);
    CHECK(st->startup_releases == 1);
    CHECK(counts.releases == 1);

    bus_write16(&soc.bus, KEYPAD_SCAN_COMMAND, st->scan_command);
    CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) == 0x000D);
    CHECK(st->startup_releases == 1 && counts.releases == 1);
    CHECK(cemu_keypad_set_button(st, &soc, "power", 0) == 1);
    bus_write16(&soc.bus, KEYPAD_SCAN_COMMAND, st->scan_command);
    CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) == st->matrix_idle_result);
    cemu_soc_free(&soc);
}

static void test_direct_scanning_simultaneous_and_irq(void) {
    soc_t soc;
    if (!setup_device(&soc, "c55", synth_defaults())) return;
    keypad_state_t *st = keypad(&soc);
    consume_startup(&soc, st);
    bus_write16(&soc.bus, 0xF16E, 0x004C);
    CHECK(cemu_keypad_set_button(st, &soc, "1", 1) == 1);
    CHECK(bus_read16(&soc.bus, 0xF16E) == 0x00CC);
    CHECK(cemu_keypad_set_button(st, &soc, "soft-left", 1) == 1);
    bus_write16(&soc.bus, KEYPAD_SCAN_COMMAND, 0x00ED);
    CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) == (0x0007 & 0x000E));
    CHECK((cemu_keypad_sampled_buttons(st) & button_bit(st, "1")) != 0);
    CHECK((cemu_keypad_sampled_buttons(st) &
           button_bit(st, "soft-left")) != 0);
    CHECK(cemu_keypad_set_button(st, &soc, "1", 0) == 1);
    CHECK(cemu_keypad_set_button(st, &soc, "soft-left", 0) == 1);
    cemu_soc_free(&soc);
}

static void test_dp3_startup_runtime_and_release_sampling(void) {
    soc_t soc;
    if (!setup_device(&soc, "cf62", synth_defaults())) return;
    keypad_state_t *st = keypad(&soc);

    bus_write16(&soc.bus, KEYPAD_SCAN_COMMAND, 0x0000);
    CHECK(cemu_memory_controller_peek16(&soc.memory,
                                         KEYPAD_SCAN_RESULT) == 0x000D);
    bus_write16(&soc.bus, 0xFFC6, 0x0000);
    CHECK(cemu_memory_controller_peek16(&soc.memory,
                                         KEYPAD_SCAN_RESULT) == 0x000D);
    CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) == 0x000D);
    CHECK(!st->startup_power_pending);

    CHECK(cemu_keypad_set_button(st, &soc, "soft-left", 1) == 1);
    bus_write16(&soc.bus, 0xFFC6, 0x003D);
    bus_write16(&soc.bus, 0xFFC6, 0x0000);
    CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) == 0x000F);
    bus_write16(&soc.bus, 0xFFC6, 0x0001);
    CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) == 0x000E);
    CHECK(cemu_keypad_set_button(st, &soc, "soft-left", 0) == 1);
    CHECK(st->release_activity_pending);
    bus_write16(&soc.bus, 0xFFC6, 0x003D);
    bus_write16(&soc.bus, 0xFFC6, 0x0000);
    CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) == st->matrix_idle_result);
    CHECK(!st->release_activity_pending);
    cemu_soc_free(&soc);
}

static void test_response_suppression(void) {
    unsigned suppressed = synth_defaults() |
        (1u << SYN_SUPPRESS_EF_MAILBOX_RESPONSE);
    soc_t direct, dp3;
    if (setup_device(&direct, "c55", suppressed)) {
        keypad_state_t *st = keypad(&direct);
        bus_write16(&direct.bus, KEYPAD_SCAN_COMMAND, st->scan_command);
        CHECK(cemu_memory_controller_peek16(&direct.memory,
                                             KEYPAD_SCAN_RESULT) == 0);
        CHECK(st->startup_power_pending && st->results == 0);
        cemu_soc_free(&direct);
    }
    if (setup_device(&dp3, "cf62", suppressed)) {
        keypad_state_t *st = keypad(&dp3);
        bus_write16(&dp3.bus, 0xFFC6, 0x0000);
        CHECK(cemu_memory_controller_peek16(&dp3.memory,
                                             KEYPAD_SCAN_RESULT) == 0);
        CHECK(st->startup_power_pending && st->results == 0);
        cemu_soc_free(&dp3);
    }
}

static int replace_snapshot_text(const char *directory, const char *old,
                                 const char *replacement) {
    char path[512];
    snprintf(path, sizeof path, "%s/snapshot.json", directory);
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        return 0;
    }
    long end = ftell(file);
    if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return 0;
    }
    size_t size = (size_t)end;
    char *text = malloc(size + 1u);
    if (!text) { fclose(file); return 0; }
    int ok = fread(text, 1, size, file) == size;
    fclose(file);
    text[size] = 0;
    char *match = ok ? strstr(text, old) : NULL;
    if (!match) { free(text); return 0; }
    size_t before = (size_t)(match - text);
    size_t old_len = strlen(old), new_len = strlen(replacement);
    char *updated = malloc(size - old_len + new_len + 1u);
    if (!updated) { free(text); return 0; }
    memcpy(updated, text, before);
    memcpy(updated + before, replacement, new_len);
    memcpy(updated + before + new_len, match + old_len,
           size - before - old_len + 1u);
    free(text);
    file = fopen(path, "wb");
    ok = file && fwrite(updated, 1, size - old_len + new_len, file) ==
                  size - old_len + new_len;
    if (file) ok &= fclose(file) == 0;
    free(updated);
    return ok;
}

static void remove_snapshot_dir(const char *directory) {
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char path[768];
        snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
        unlink(path);
    }
    closedir(dir);
    rmdir(directory);
}

static void round_trip_state(soc_t *soc, cpu_t *cpu, debugger_t *debugger,
                             event_counts_t *events) {
    keypad_state_t *st = keypad(soc);
    keypad_mutable_state_t expected, actual;
    cemu_keypad_capture_mutable(st, &expected);
    uint64_t digest = cemu_state_digest(cpu, soc);

    cemu_debugger_checkpoint(debugger);
    st->scans += 101;
    st->pressed = 0;
    st->sampled_pressed = 0;
    unsigned releases = events->releases, inputs = events->inputs;
    CHECK(cemu_debugger_restore(debugger));
    cemu_keypad_capture_mutable(st, &actual);
    check_mutable_equal(&expected, &actual);
    CHECK(cemu_state_digest(cpu, soc) == digest);
    CHECK(events->releases == releases && events->inputs == inputs);

    char directory[] = "/tmp/cemu-keypad-state-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    CHECK(cemu_snapshot_write_dir(directory, cpu, soc, NULL) == 0);
    st->results += 99;
    st->matrix_scan_phase ^= 1;
    CHECK(cemu_snapshot_read_dir(directory, cpu, soc) == 0);
    cemu_keypad_capture_mutable(st, &actual);
    check_mutable_equal(&expected, &actual);
    CHECK(cemu_state_digest(cpu, soc) == digest);
    CHECK(events->releases == releases && events->inputs == inputs);
    remove_snapshot_dir(directory);
}

static void test_checkpoint_and_snapshot_round_trips(void) {
    soc_t soc;
    if (!setup_device(&soc, "cf62", synth_defaults())) return;
    cpu_t cpu;
    cemu_cpu_init(&cpu, &soc.bus);
    cemu_soc_attach_cpu(&soc, &cpu);
    cemu_cpu_reset(&cpu);
    debugger_t debugger;
    cemu_debugger_init(&debugger, &cpu, &soc, 0);
    event_counts_t events = {0};
    CHECK(cemu_event_subscribe_filtered(
        &soc.instrumentation, CEMU_EVENT_PERIPHERAL,
        count_keypad_events, keypad_event_filter, &events) != 0);
    keypad_state_t *st = keypad(&soc);

    bus_write16(&soc.bus, KEYPAD_SCAN_COMMAND, st->scan_command);
    CHECK(bus_read8(&soc.bus, KEYPAD_SCAN_RESULT) == 0x0D);
    round_trip_state(&soc, &cpu, &debugger, &events);

    CHECK(bus_read8(&soc.bus, KEYPAD_SCAN_RESULT + 1u) == 0);
    CHECK(cemu_keypad_set_button(st, &soc, "soft-left", 1) == 1);
    bus_write16(&soc.bus, st->matrix_select_addr, st->matrix_select_mask);
    bus_write16(&soc.bus, st->matrix_select_addr, 0);
    CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) == st->matrix_idle_result);
    bus_write16(&soc.bus, st->matrix_select_addr, 1);
    CHECK(bus_read16(&soc.bus, KEYPAD_SCAN_RESULT) == 0x000E);
    CHECK((st->sampled_pressed & button_bit(st, "soft-left")) != 0);
    CHECK(st->matrix_scan_phase == 2);
    round_trip_state(&soc, &cpu, &debugger, &events);

    CHECK(cemu_keypad_set_button(st, &soc, "soft-left", 0) == 1);
    CHECK(st->release_activity_pending);
    round_trip_state(&soc, &cpu, &debugger, &events);

    cemu_debugger_detach(&debugger);
    cemu_debugger_free(&debugger);
    cemu_soc_free(&soc);
}

static void test_legacy_snapshot_fallback(void) {
    for (unsigned schema = 32; schema <= 36; schema++) {
        soc_t soc;
        if (!setup_device(&soc, "c55", synth_defaults())) continue;
        cpu_t cpu;
        cemu_cpu_init(&cpu, &soc.bus);
        cemu_soc_attach_cpu(&soc, &cpu);
        cemu_cpu_reset(&cpu);
        keypad_state_t *st = keypad(&soc);
        bus_write16(&soc.bus, KEYPAD_SCAN_COMMAND, st->scan_command);
        CHECK(cemu_keypad_set_button(st, &soc, "soft-left", 1) == 1);
        char directory[] = "/tmp/cemu-keypad-legacy-XXXXXX";
        CHECK(mkdtemp(directory) != NULL);
        CHECK(cemu_snapshot_write_dir(directory, &cpu, &soc, NULL) == 0);
        char replacement[32];
        snprintf(replacement, sizeof replacement, "\"schema\": %u", schema);
        CHECK(replace_snapshot_text(directory, "\"schema\": 39",
                                    replacement));
        CHECK(cemu_snapshot_read_dir(directory, &cpu, &soc) == 0);
        CHECK(!st->startup_power_pending);
        CHECK(!st->startup_result_tagged);
        CHECK(st->startup_result_read_mask == 0);
        CHECK(st->pressed == 0 && st->sampled_pressed == 0);
        CHECK(st->matrix_scan_phase == 0);
        CHECK(!st->release_activity_pending);
        remove_snapshot_dir(directory);
        cemu_soc_free(&soc);
    }
}

static void test_digest_covers_all_mutable_state(void) {
    soc_t soc;
    if (!setup_device(&soc, "c55", synth_defaults())) return;
    cpu_t cpu;
    cemu_cpu_init(&cpu, &soc.bus);
    cemu_soc_attach_cpu(&soc, &cpu);
    cemu_cpu_reset(&cpu);
    keypad_state_t *st = keypad(&soc);
    keypad_mutable_state_t base, changed;
    cemu_keypad_capture_mutable(st, &base);
    uint64_t digest = cemu_state_digest(&cpu, &soc);
#define DIFFERENT(statement) do { \
    CHECK(cemu_keypad_restore_mutable(st, &base)); \
    statement; \
    CHECK(cemu_state_digest(&cpu, &soc) != digest); \
} while (0)
    DIFFERENT(st->scans++);
    DIFFERENT(st->handled_scans++);
    DIFFERENT(st->results++);
    DIFFERENT(st->startup_releases++);
    DIFFERENT(st->pressed = 1);
    DIFFERENT(st->pressed = st->sampled_pressed = 1);
    DIFFERENT(st->last_command++);
    DIFFERENT(st->last_result++);
    DIFFERENT(st->startup_power_pending = 0);
    DIFFERENT(st->startup_result_tagged = 1);
    DIFFERENT(st->startup_result_read_mask = 1);
    DIFFERENT(st->matrix_scan_phase = 1);
    DIFFERENT(st->release_activity_pending = 1);
#undef DIFFERENT
    CHECK(cemu_keypad_restore_mutable(st, &base));
    cemu_keypad_capture_mutable(st, &changed);
    check_mutable_equal(&base, &changed);
    cemu_soc_free(&soc);
}

typedef struct { const char *name; void (*fn)(void); } test_entry_t;
static const test_entry_t tests[] = {
    {"all_device_keymaps", test_all_device_keymaps},
    {"startup_consumption_lifecycle", test_startup_consumption_lifecycle},
    {"direct_scanning_simultaneous_and_irq",
     test_direct_scanning_simultaneous_and_irq},
    {"dp3_startup_runtime_and_release_sampling",
     test_dp3_startup_runtime_and_release_sampling},
    {"response_suppression", test_response_suppression},
    {"checkpoint_and_snapshot_round_trips",
     test_checkpoint_and_snapshot_round_trips},
    {"legacy_snapshot_fallback", test_legacy_snapshot_fallback},
    {"digest_covers_all_mutable_state", test_digest_covers_all_mutable_state},
};

int main(void) {
    int failed_tests = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = failures;
        tests[i].fn();
        if (failures != before) {
            failed_tests++;
            printf("[FAIL] %s\n", tests[i].name);
        }
    }
    free(flash);
    printf("%zu tests, %d failed\n",
           sizeof tests / sizeof tests[0], failed_tests);
    return failures ? 1 : 0;
}
