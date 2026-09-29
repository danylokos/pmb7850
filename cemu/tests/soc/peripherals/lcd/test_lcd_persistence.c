/* Atomic LCD snapshot and debugger-checkpoint persistence tests. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "debugger.h"
#include "battery.h"
#include "lcd.h"
#include "snapshot.h"
#include "soc.h"
#include "state_digest.h"
#include "synth.h"

static int g_fail, g_total_fail, g_total_run;
#define CHECK(cond) do { if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define FLEN (8u * 1024u * 1024u)

static const battery_curve_point_t BATTERY_CURVE[] = {
    {0, 3200},
    {100, 4200},
};

static device_config_t config(const char *model) {
    device_config_t cfg = {
        .name = "lcd-persistence-test",
        .flash = {
            .chips = {{
                .name = "flash",
                .model = "m58lw064d",
                .chip_size = FLEN,
            }},
            .nchips = 1,
            .windows = {{
                .cpu_base = 0,
                .cpu_size = FLEN,
                .chip_index = 0,
                .chip_base = 0,
                .mirror_period = FLEN,
                .command_visible = 1,
            }},
            .nwindows = 1,
        },
        .lcd = {
            .select_addr = 0xFFC4,
            .select_bit = 11,
            .select_active_low = 1,
            .dc_addr = 0xFFD0,
            .dc_bit = 6,
            .reset_capcom_channel = -1,
        },
        .battery = {
            .available = 1,
            .curve = BATTERY_CURVE,
            .n_curve = sizeof BATTERY_CURVE / sizeof BATTERY_CURVE[0],
            .adc_raw_high = -1628,
            .adc_mv_high = 4178,
            .adc_raw_low = 4499,
            .adc_mv_low = 3177,
            .battery_channel = 1,
            .reference_channel = 3,
            .adc_result_word = DEVICE_BATTERY_RESULT0,
        },
    };
    cfg.lcd.model = model;
    if (!model) {
        return cfg;
    } else if (!strcmp(model, "pcf8813")) {
        cfg.lcd.panel_width = PCF8813_WIDTH;
        cfg.lcd.panel_height = PCF8813_HEIGHT;
    } else if (!strcmp(model, "hm17cm256")) {
        cfg.lcd.panel_width = HM17CM256_WIDTH;
        cfg.lcd.panel_height = HM17CM256_GRAPHIC_HEIGHT;
    } else if (!strcmp(model, "hm17cm4096")) {
        cfg.lcd.panel_width = HM17CM4096_WIDTH;
        cfg.lcd.panel_height = 80;
    } else if (!strcmp(model, "pcf8833-4wire")) {
        cfg.lcd.panel_width = PCF8833_4WIRE_WIDTH;
        cfg.lcd.panel_height = PCF8833_4WIRE_HEIGHT;
    } else if (!strcmp(model, "s6b33bx")) {
        cfg.lcd.panel_width = S6B33BX_WIDTH;
        cfg.lcd.panel_height = S6B33BX_HEIGHT;
    }
    return cfg;
}

static void seed_active_sweep(peripheral_t *p) {
    if (!strcmp(p->model, "pcf8813")) {
        pcf8813_state_t *st = p->state;
        st->ddram[0] = 0xA5;
        st->presented_ddram[0] = 0x5A;
        st->common.transaction_data_bytes = 17;
        st->common.frame_seq = 3;
        st->transaction_count = 11;
        st->transaction_started = 1;
        st->transaction_start_x = 1;
        st->transaction_start_y = 4;
        st->transaction_sweep_member = 1;
        st->transaction_shape_valid = 1;
        st->sweep_active = 1;
        st->sweep_next_y = 4;
        st->sweep_start_x = 1;
        st->sweep_x_max = 101;
        st->sweep_y_max = 8;
        st->sweep_data_bytes = 408;
        st->sweep.active = 1;
        st->sweep.origin_x = 1;
        st->sweep.expected_x = 1;
        st->sweep.expected_y = 4;
        st->sweep.data_bytes = 408;
        st->sweep.storage_writes = 408;
    } else if (!strcmp(p->model, "hm17cm256")) {
        hm17cm256_state_t *st = p->state;
        st->gram[0] = 0xA5;
        st->presented_gram[0] = 0x5A;
        st->common.transaction_data_bytes = 19;
        st->common.frame_seq = 4;
        st->transaction_count = 23;
        st->transaction_started = 1;
        st->transaction_start_x = 14;
        st->transaction_start_y = 23;
        st->transaction_increment_x = 1;
        st->transaction_sweep_member = 1;
        st->transaction_shape_valid = 1;
        st->sweep_active = 1;
        st->sweep_next_y = 23;
        st->sweep_start_x = 14;
        st->sweep_increment_x = 1;
        st->sweep_end_x = 127;
        st->sweep_end_y = 81;
        st->sweep_row_bytes = 101;
        st->sweep_data_bytes = 2323;
        st->sweep.active = 1;
        st->sweep.origin_x = 14;
        st->sweep.expected_x = 14;
        st->sweep.expected_y = 23;
        st->sweep.data_bytes = 2323;
        st->sweep.storage_writes = 2323;
    } else if (!strcmp(p->model, "hm17cm4096")) {
        hm17cm4096_state_t *st = p->state;
        st->gram[0] = 0xABC;
        st->presented_gram[0] = 0x123;
        st->sweep.active = 1;
        st->sweep.pack_phase = 1;
        st->sweep.data_bytes = 17;
        st->sweep.storage_writes = 8;
    } else if (!strcmp(p->model, "pcf8833-4wire")) {
        pcf8833_4wire_state_t *st = p->state;
        st->gram[0] = 0xABC;
        st->presented_gram[0] = 0x123;
        st->sweep.active = 1;
        st->sweep.data_bytes = st->sweep.storage_writes = 17;
    } else if (!strcmp(p->model, "s6b33bx")) {
        s6b33bx_state_t *st = p->state;
        st->gram[0] = 0xABCD;
        st->presented_gram[0] = 0x1234;
        st->sweep.active = 1;
        st->sweep.data_bytes = 34;
        st->sweep.storage_writes = 17;
    }
}

static void mutate_sweep(peripheral_t *p) {
    if (!strcmp(p->model, "pcf8813")) {
        pcf8813_state_t *st = p->state;
        st->ddram[0] ^= 0xFF;
        st->presented_ddram[0] ^= 0xFF;
        st->sweep_active = 0;
        st->sweep_data_bytes = 0;
        st->sweep.active = 0;
    } else if (!strcmp(p->model, "hm17cm256")) {
        hm17cm256_state_t *st = p->state;
        st->gram[0] ^= 0xFF;
        st->presented_gram[0] ^= 0xFF;
        st->sweep_active = 0;
        st->sweep_data_bytes = 0;
        st->sweep.active = 0;
    } else if (!strcmp(p->model, "hm17cm4096")) {
        hm17cm4096_state_t *st = p->state;
        st->gram[0] ^= 0xFFF;
        st->presented_gram[0] ^= 0xFFF;
        st->sweep.active = 0;
    } else if (!strcmp(p->model, "pcf8833-4wire")) {
        pcf8833_4wire_state_t *st = p->state;
        st->gram[0] ^= 0xFFF;
        st->presented_gram[0] ^= 0xFFF;
        st->sweep.active = 0;
    } else if (!strcmp(p->model, "s6b33bx")) {
        s6b33bx_state_t *st = p->state;
        st->gram[0] ^= 0xFFFF;
        st->presented_gram[0] ^= 0xFFFF;
        st->sweep.active = 0;
    }
}

static int snapshot_schema_is(const char *directory, unsigned schema) {
    char path[512];
    snprintf(path, sizeof path, "%s/snapshot.json", directory);
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    char text[128] = {0};
    size_t got = fread(text, 1, sizeof text - 1u, file);
    fclose(file);
    text[got] = 0;
    char expected[32];
    snprintf(expected, sizeof expected, "\"schema\": %u", schema);
    return strstr(text, expected) != NULL;
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
        fclose(file);
        return 0;
    }
    size_t size = (size_t)end;
    char *text = malloc(size + 1u);
    if (!text) {
        fclose(file);
        return 0;
    }
    int ok = fread(text, 1, size, file) == size;
    fclose(file);
    text[size] = 0;
    char *match = ok ? strstr(text, old) : NULL;
    if (!match) {
        free(text);
        return 0;
    }
    size_t old_len = strlen(old), replacement_len = strlen(replacement);
    size_t prefix = (size_t)(match - text);
    size_t new_size = size - old_len + replacement_len;
    char *updated = malloc(new_size + 1u);
    if (!updated) {
        free(text);
        return 0;
    }
    memcpy(updated, text, prefix);
    memcpy(updated + prefix, replacement, replacement_len);
    memcpy(updated + prefix + replacement_len, match + old_len,
           size - prefix - old_len);
    updated[new_size] = 0;
    free(text);
    file = fopen(path, "wb");
    if (!file) {
        free(updated);
        return 0;
    }
    ok = fwrite(updated, 1, new_size, file) == new_size;
    ok &= fclose(file) == 0;
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

static void check_model(const char *model) {
    uint8_t *flash = malloc(FLEN);
    CHECK(flash != NULL);
    if (!flash) return;
    memset(flash, 0xFF, FLEN);
    device_config_t cfg = config(model);
    soc_t soc;
    cemu_status_t status = cemu_soc_init(&soc, flash, FLEN, &cfg,
                                         synth_defaults(), 0);
    CHECK(status.code == CEMU_STATUS_OK);
    if (status.code != CEMU_STATUS_OK) {
        free(flash);
        return;
    }
    cpu_t cpu;
    cemu_cpu_init(&cpu, &soc.bus);
    cemu_soc_attach_cpu(&soc, &cpu);
    cemu_cpu_reset(&cpu);
    seed_active_sweep(soc.lcd_periph);
    CHECK(cemu_battery_set_state(soc.battery_periph, &soc, 37, 1,
                                 "test"));

    size_t state_size = cemu_lcd_state_size(soc.lcd_periph);
    uint8_t *saved = malloc(state_size);
    CHECK(saved != NULL);
    memcpy(saved, soc.lcd_periph->state, state_size);
    uint64_t digest = state_digest(&cpu, &soc);

    debugger_t debugger;
    debugger_init(&debugger, &cpu, &soc, 0);
    debugger_checkpoint(&debugger);
    mutate_sweep(soc.lcd_periph);
    CHECK(state_digest(&cpu, &soc) != digest);
    CHECK(debugger_restore(&debugger));
    CHECK(!memcmp(saved, soc.lcd_periph->state, state_size));
    CHECK(cemu_battery_level(soc.battery_periph) == 37);
    CHECK(cemu_battery_charging(soc.battery_periph));
    CHECK(state_digest(&cpu, &soc) == digest);

    char directory[] = "/tmp/cemu-lcd-persistence-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snapshot_write_dir(directory, &cpu, &soc, NULL) == 0);
    CHECK(snapshot_schema_is(directory, 39));
    mutate_sweep(soc.lcd_periph);
    cemu_battery_restore_state(soc.battery_periph, 99, 0);
    CHECK(snapshot_read_dir(directory, &cpu, &soc) == 0);
    CHECK(!memcmp(saved, soc.lcd_periph->state, state_size));
    CHECK(cemu_battery_level(soc.battery_periph) == 37);
    CHECK(cemu_battery_charging(soc.battery_periph));
    CHECK(state_digest(&cpu, &soc) == digest);

    CHECK(replace_snapshot_text(directory, "\"schema\": 39",
                                "\"schema\": 37"));
    CHECK(snapshot_read_dir(directory, &cpu, &soc) != 0);

    remove_snapshot_dir(directory);

    debugger_detach(&debugger);
    debugger_free(&debugger);
    free(saved);
    cemu_soc_free(&soc);
    free(flash);
}

static void test_pcf8813_persistence(void) { check_model("pcf8813"); }
static void test_hm17cm256_persistence(void) { check_model("hm17cm256"); }
static void test_hm17cm4096_persistence(void) { check_model("hm17cm4096"); }
static void test_pcf8833_persistence(void) { check_model("pcf8833-4wire"); }
static void test_s6b33bx_persistence(void) { check_model("s6b33bx"); }

static void test_legacy_null_battery_schemas(void) {
    for (unsigned schema = 32; schema <= 34; schema++) {
        uint8_t *flash = malloc(FLEN);
        CHECK(flash != NULL);
        if (!flash) return;
        memset(flash, 0xFF, FLEN);
        device_config_t cfg = config(NULL);
        soc_t soc;
        cemu_status_t status = cemu_soc_init(&soc, flash, FLEN, &cfg,
                                             synth_defaults(), 0);
        CHECK(status.code == CEMU_STATUS_OK);
        if (status.code != CEMU_STATUS_OK) {
            free(flash);
            return;
        }
        cpu_t cpu;
        cemu_cpu_init(&cpu, &soc.bus);
        cemu_soc_attach_cpu(&soc, &cpu);
        cemu_cpu_reset(&cpu);
        CHECK(cemu_battery_set_state(soc.battery_periph, &soc, 37, 1,
                                     "test"));

        char directory[] = "/tmp/cemu-snapshot-legacy-XXXXXX";
        CHECK(mkdtemp(directory) != NULL);
        CHECK(snapshot_write_dir(directory, &cpu, &soc, NULL) == 0);
        char replacement[32];
        snprintf(replacement, sizeof replacement, "\"schema\": %u", schema);
        CHECK(replace_snapshot_text(directory, "\"schema\": 39",
                                    replacement));
        CHECK(replace_snapshot_text(
            directory,
            "\"battery\": {\"level\": 37, \"charging\": true}",
            "\"battery\": null"));
        cemu_battery_restore_state(soc.battery_periph, 61, 0);
        CHECK(snapshot_read_dir(directory, &cpu, &soc) == 0);
        CHECK(cemu_battery_level(soc.battery_periph) == 61);
        CHECK(!cemu_battery_charging(soc.battery_periph));

        remove_snapshot_dir(directory);
        cemu_soc_free(&soc);
        free(flash);
    }
}

typedef struct { const char *name; void (*fn)(void); } entry_t;
static const entry_t TESTS[] = {
    {"pcf8813_persistence", test_pcf8813_persistence},
    {"hm17cm256_persistence", test_hm17cm256_persistence},
    {"hm17cm4096_persistence", test_hm17cm4096_persistence},
    {"pcf8833_persistence", test_pcf8833_persistence},
    {"s6b33bx_persistence", test_s6b33bx_persistence},
    {"legacy_null_battery_schemas", test_legacy_null_battery_schemas},
};

int main(void) {
    int n = (int)(sizeof TESTS / sizeof TESTS[0]);
    for (int i = 0; i < n; i++) {
        g_fail = 0;
        TESTS[i].fn();
        g_total_run++;
        g_total_fail += g_fail;
        if (g_fail) printf("[FAIL] %s (%d)\n", TESTS[i].name, g_fail);
    }
    printf("\n%d tests, %d failed\n", g_total_run, g_total_fail);
    return g_total_fail ? 1 : 0;
}
