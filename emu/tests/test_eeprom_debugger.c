#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "emu_eeprom.h"
#include "emu_eeprom_debugger.h"

typedef struct {
    uint8_t *bytes[2];
    size_t sizes[2];
} images_t;

typedef struct {
    int count;
    char kind[32];
    char reason[640];
    uint32_t pc;
    uint64_t icount;
} stop_log_t;

static void put16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void put_record(uint8_t *flash, size_t offset, uint16_t id,
                       uint16_t length, uint32_t linear, uint16_t version,
                       uint16_t marker) {
    uint8_t low = marker == 0xF000 ? 0xF0
                : marker == 0xF800 ? 0xF8 : 0xFC;
    put16(flash + offset, (uint16_t)((version << 8) | low));
    put16(flash + offset + 2, length);
    put16(flash + offset + 4, (uint16_t)linear);
    put16(flash + offset + 6, (uint16_t)(linear >> 16));
    put16(flash + offset + 8, id);
    put16(flash + offset + 10, marker);
}

static uint8_t *make_image(size_t *size_out, size_t base, uint32_t linear) {
    size_t size = base + EMU_EEPROM_REGION_SIZE;
    uint8_t *flash = malloc(size);
    EMU_CHECK(flash != NULL);
    memset(flash, 0xFF, size);
    size_t magic = base + 0x12;
    flash[magic - 2] = flash[magic - 1] = 0xFE;
    memcpy(flash + magic, "EELITE", 6);
    memcpy(flash + magic + 0x20000, "EEFULL", 6);
    memcpy(flash + magic + 0x40000, "EEFULL", 6);
    put_record(flash, base + 0x200, 20, 16,
               linear + 0x50000, 2, 0xFC00);
    put_record(flash, base + 0x220, 20, 16,
               linear + 0x50100, 3, 0xF800);
    put_record(flash, base + 0x240, 30, 16,
               linear + 0x01000, 4, 0xF000);
    for (size_t i = 0; i < 16; i++) {
        flash[base + 0x50000 + i] = (uint8_t)(0x40 + i);
        flash[base + 0x50100 + i] = (uint8_t)(0x80 + i);
    }
    *size_out = size;
    return flash;
}

static int copy_image(void *opaque, int chip_index, size_t offset,
                      uint8_t *bytes, size_t size) {
    images_t *images = opaque;
    if (chip_index < 0 || chip_index >= 2 ||
        offset > images->sizes[chip_index] ||
        size > images->sizes[chip_index] - offset)
        return -1;
    memcpy(bytes, images->bytes[chip_index] + offset, size);
    return 0;
}

static void capture_stop(void *opaque, const char *kind,
                         uint32_t pc, uint64_t icount,
                         const char *reason) {
    stop_log_t *log = opaque;
    log->count++;
    snprintf(log->kind, sizeof log->kind, "%s", kind);
    snprintf(log->reason, sizeof log->reason, "%s", reason);
    log->pc = pc;
    log->icount = icount;
}

static void command(emu_eeprom_debugger_t *debugger, FILE *output,
                    int argc, const char **arguments) {
    char *argv[8];
    EMU_CHECK(argc <= (int)(sizeof argv / sizeof argv[0]));
    for (int i = 0; i < argc; i++) argv[i] = (char *)arguments[i];
    EMU_CHECK(emu_eeprom_debugger_command(
                  debugger, argc, argv, output) == 1);
    fflush(output);
}

static char *output_text(FILE *output) {
    fflush(output);
    long size = ftell(output);
    EMU_CHECK(size >= 0);
    char *text = malloc((size_t)size + 1u);
    EMU_CHECK(text != NULL);
    rewind(output);
    EMU_CHECK(fread(text, 1, (size_t)size, output) == (size_t)size);
    text[size] = 0;
    fseek(output, 0, SEEK_END);
    return text;
}

int main(void) {
    const size_t base0 = 0x10000, base1 = 0x20000;
    images_t images = {0};
    images.bytes[0] = make_image(&images.sizes[0], base0, 0xFA0000);
    images.bytes[1] = make_image(&images.sizes[1], base1, 0xF90000);
    emu_eeprom_trace_chip_t chips[] = {
        {0, "flash", "m58lw064d", images.sizes[0]},
        {1, "flash1", "am29lv640mh", images.sizes[1]},
    };
    stop_log_t stops = {0};
    emu_eeprom_debugger_t *debugger = NULL;
    EMU_CHECK(emu_eeprom_debugger_create(
                  &debugger, chips, 2, copy_image, &images,
                  capture_stop, &stops, 1, 2, 3) == 0);
    FILE *output = tmpfile();
    EMU_CHECK(output != NULL);

    const char *list[] = {"eeprom", "list"};
    command(debugger, output, 2, list);
    const char *ambiguous[] = {"eeprom", "show", "20"};
    command(debugger, output, 3, ambiguous);
    const char *show[] = {"eeprom", "show", "flash:20", "2", "8"};
    command(debugger, output, 5, show);
    const char *too_much[] = {
        "eeprom", "show", "flash:20", "0", "4097",
    };
    command(debugger, output, 5, too_much);
    const char *watch[] = {"eeprom", "watch", "flash:20", "r"};
    command(debugger, output, 4, watch);
    const char *log[] = {"eeprom", "log", "0:20", "w"};
    command(debugger, output, 4, log);

    emu_eeprom_debugger_read(
        debugger, 0, (uint32_t)(base0 + 0x50004),
        0xFF0004, 0xBBAA, 2, 10, 11, 0x123456);
    emu_eeprom_debugger_read(
        debugger, 0, (uint32_t)(base0 + 0x50103),
        0xFF0103, 0xCC, 1, 12, 13, 0x123458);
    emu_eeprom_debugger_read(
        debugger, 0, (uint32_t)(base0 + 0x202),
        0xFA0202, 0xDD, 1, 14, 15, 0x12345A);
    EMU_CHECK(stops.count == 2);
    EMU_CHECK(!strcmp(stops.kind, "eeprom-watch"));
    EMU_CHECK(strstr(stops.reason, "attribution=shadowed") != NULL);

    images.bytes[0][base0 + 0x50008] = 0x20;
    images.bytes[0][base0 + 0x50009] = 0x21;
    emu_eeprom_debugger_mutation(
        debugger, 0, EMU_EEPROM_MUTATION_PROGRAM,
        (uint32_t)(base0 + 0x50008), 2, 20, 21, 0x200000);

    memset(images.bytes[0] + base0 + 0x200, 0xFF, 12);
    emu_eeprom_debugger_mutation(
        debugger, 0, EMU_EEPROM_MUTATION_ERASE,
        (uint32_t)(base0 + 0x200), 12, 30, 31, 0x300000);
    emu_eeprom_debugger_read(
        debugger, 0, (uint32_t)(base0 + 0x50105),
        0xFF0105, 0xEE, 1, 32, 33, 0x300002);
    EMU_CHECK(stops.count == 3);

    const char *stats[] = {"eeprom", "stats", "flash:20"};
    command(debugger, output, 3, stats);
    const char *watches[] = {"eeprom", "watches"};
    command(debugger, output, 2, watches);
    memset(images.bytes[0] + base0 + 0x10, 0xFF, 8);
    emu_eeprom_debugger_mutation(
        debugger, 0, EMU_EEPROM_MUTATION_ERASE,
        (uint32_t)(base0 + 0x10), 8, 36, 37, 0x380000);
    emu_eeprom_debugger_resync(debugger, 40, 41, 0x400000);
    command(debugger, output, 2, watches);
    const char *list_flash[] = {"eeprom", "list", "flash"};
    command(debugger, output, 3, list_flash);
    const char *remove[] = {"eeprom", "unwatch", "1"};
    command(debugger, output, 3, remove);
    const char *watch_again[] = {"eeprom", "watch", "flash:20"};
    command(debugger, output, 3, watch_again);
    const char *remove_all[] = {"eeprom", "unwatch", "all"};
    command(debugger, output, 3, remove_all);

    char *text = output_text(output);
    EMU_CHECK(strstr(text, "flash[0] block=0x0014 (20)") != NULL);
    EMU_CHECK(strstr(text, "flash1[1] block=0x0014 (20)") != NULL);
    EMU_CHECK(strstr(text, "ambiguous EEPROM block 0x0014") != NULL);
    EMU_CHECK(strstr(text, "payload [0x2,0xa)") != NULL);
    EMU_CHECK(strstr(text, "0x0002: 42 43 44 45 46 47 48 49") != NULL);
    EMU_CHECK(strstr(text, "exceeds 4096 bytes") != NULL);
    EMU_CHECK(strstr(text, "[eeprom-log]") != NULL);
    EMU_CHECK(strstr(text, "[eeprom-map]") != NULL);
    EMU_CHECK(strstr(text, "resolved read=2/3B program=1/2B") != NULL);
    EMU_CHECK(strstr(text, "shadowed read=1/1B") != NULL);
    EMU_CHECK(strstr(text, "metadata read=1/1B program=0/0B erase=1/12B") != NULL);
    EMU_CHECK(strstr(text, "1 mode=watch") != NULL);
    EMU_CHECK(strstr(text, "2 mode=log") != NULL);
    EMU_CHECK(strstr(text, "state=mapped hits=3") != NULL);
    EMU_CHECK(strstr(text, "removed logical EEPROM watch 1") != NULL);
    EMU_CHECK(strstr(text, "EEPROM watch 3 set") != NULL);
    free(text);

    fclose(output);
    emu_eeprom_debugger_destroy(&debugger);
    EMU_CHECK(debugger == NULL);
    free(images.bytes[0]);
    free(images.bytes[1]);
    puts("logical EEPROM debugger: PASS");
    return 0;
}
