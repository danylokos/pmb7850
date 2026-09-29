#define _POSIX_C_SOURCE 200809L

#include <unistd.h>

#include "emu_eeprom.h"
#include "emu_qemu_eeprom_trace.h"
#include "emu_trace_schema.h"
#include "test_support.h"

typedef struct {
    char kind[24], change[24], access[24], attribution[32], chip_name[32];
    long chip, block, generation, payload;
    uint32_t addr, value;
} event_t;

typedef struct {
    event_t events[128];
    size_t count;
} log_t;

static void capture(void *opaque, const emu_trace_event_t *event, uint64_t seq) {
    log_t *log = opaque;
    char error[256];
    (void)seq;
    EMU_CHECK(!emu_trace_schema_validate_event(event, error, sizeof error));
    EMU_CHECK(log->count < 128);
    event_t *out = &log->events[log->count++];
    *out = (event_t){.addr = event->addr, .value = event->value};
    snprintf(out->kind, sizeof out->kind, "%s", event->kind);
    for (int i = 0; i < event->info.n; i++) {
        const emu_trace_field_t *f = &event->info.kv[i];
#define INT(keyname, member) if (!strcmp(f->key, keyname)) out->member = f->ival
#define STR(keyname, member) if (!strcmp(f->key, keyname)) \
        snprintf(out->member, sizeof out->member, "%s", f->sval)
        INT("chip_index", chip);
        INT("block_id", block);
        INT("mapping_generation", generation);
        INT("current_payload_offset", payload);
        STR("change", change);
        STR("access", access);
        STR("attribution", attribution);
        STR("chip_name", chip_name);
#undef INT
#undef STR
    }
}

static void fixture(emu_qemu_chip_image_t *chip, unsigned index) {
    char path[] = "/tmp/emu-qemu-eeprom-XXXXXX";
    int fd = mkstemp(path);
    EMU_CHECK(fd >= 0);
    FILE *file = fdopen(fd, "wb");
    EMU_CHECK(file != NULL);
    chip->size = 0x10000 + EMU_EEPROM_REGION_SIZE;
    uint8_t *bytes = malloc(chip->size);
    EMU_CHECK(bytes != NULL);
    memset(bytes, 0xff, chip->size);
    bytes[0x10010] = bytes[0x10011] = 0xfe;
    memcpy(bytes + 0x10012, "EELITE", 6);
    memcpy(bytes + 0x30012, "EEFULL", 6);
    memcpy(bytes + 0x50012, "EEFULL", 6);
    /* Active block 20, then its older/inactive fallback: physical +0x60000
     * and +0x60100 correspond to EEPROM-linear 0xff0000 and 0xff0100. */
    const uint8_t records[][12] = {
        {0xfc, 2, 16, 0, 0, 0, 0xff, 0, 20, 0, 0, 0xfc},
        {0xf8, 3, 16, 0, 0, 1, 0xff, 0, 20, 0, 0, 0xf8},
        {0xf0, 4, 16, 0, 0, 0x10, 0xfa, 0, 30, 0, 0, 0xf0},
    };
    memcpy(bytes + 0x10200, records[0], 12);
    memcpy(bytes + 0x10220, records[1], 12);
    memcpy(bytes + 0x10240, records[2], 12);
    bytes[0x60000] = (uint8_t)(0xa0 + index);
    EMU_CHECK(fwrite(bytes, 1, chip->size, file) == chip->size);
    EMU_CHECK(!fclose(file));
    free(bytes);
    snprintf(chip->path, sizeof chip->path, "%s", path);
    snprintf(chip->model, sizeof chip->model, "%s",
             index ? "am29lv640mh" : "m58lw064d");
}

static void parse(emu_qemu_eeprom_trace_t *trace, const char *line) {
    char error[256] = {0};
    int rc = emu_qemu_eeprom_trace_parse(trace, line, error, sizeof error);
    if (rc != 1) fprintf(stderr, "%s: %s\n", error, line);
    EMU_CHECK(rc == 1);
}

static void test_history(void) {
    emu_qemu_image_t image = {.device = "s55", .chip_count = 2};
    for (unsigned i = 0; i < 2; i++) fixture(&image.chips[i], i);
    emu_qemu_eeprom_trace_t *trace = NULL;
    char error[256];
    EMU_CHECK(!emu_qemu_eeprom_trace_create(&trace, &image, error, sizeof error));
    /* Final files are deliberately unusable. Both attachment and replay must
     * depend only on the captured pre-execution bytes. */
    for (unsigned i = 0; i < 2; i++) EMU_CHECK(!unlink(image.chips[i].path));
    for (unsigned replay = 0; replay < 2; replay++) {
        log_t log = {0};
        emu_trace_sink_t *sink = emu_trace_open_callback(
            EMU_TRACE_MASK_EEPROM, capture, &log);
        EMU_CHECK(!emu_qemu_eeprom_trace_attach(trace, sink, error, sizeof error));
        EMU_CHECK(log.count == 4);
        EMU_CHECK(log.events[0].chip == 0 && log.events[2].chip == 1);
        EMU_CHECK(!strcmp(log.events[0].chip_name, "flash-primary"));
        EMU_CHECK(!strcmp(log.events[2].chip_name, "flash-secondary"));
        EMU_CHECK(log.events[0].payload == 0x60000);
        EMU_CHECK(!strcmp(log.events[0].change, "initial"));
        parse(trace, "x55_nor_read tick=1 icount=2 pc=0x10 chip=0 offset=0x60000 addr=0x860000 value=0xa0 size=1 array=1 source=cpu\n");
        EMU_CHECK(log.count == 5 && log.events[4].block == 20);
        EMU_CHECK(log.events[4].addr == 0x860000 && log.events[4].value == 0xa0);
        parse(trace, "x55_nor_read tick=1 icount=2 pc=0x10 chip=0 offset=0x60000 addr=0x860000 value=0x80 size=1 array=0 source=cpu");
        EMU_CHECK(log.count == 5);
        parse(trace, "x55_nor_read tick=2 icount=3 pc=0x12 chip=1 offset=0x60000 addr=0x460000 value=0xa1 size=1 array=1 source=pec");
        EMU_CHECK(log.count == 6 && log.events[5].chip == 1);
        parse(trace, "x55_nor_mutation tick=3 icount=4 pc=0x14 chip=0 offset=0x60000 size=2 operation=program value=0x1234");
        parse(trace, "x55_nor_read tick=4 icount=5 pc=0x16 chip=0 offset=0x60000 addr=0xc60000 value=0x1234 size=2 array=1 source=cpu");
        EMU_CHECK(log.events[log.count - 1].value == 0x1234);
        EMU_CHECK(log.events[log.count - 1].generation == 0);
        size_t before = log.count;
        parse(trace, "x55_nor_mutation tick=5 icount=6 pc=0x18 chip=0 offset=0x10200 size=12 operation=erase value=0x0");
        EMU_CHECK(log.count == before + 2);
        event_t *map = &log.events[log.count - 1];
        EMU_CHECK(!strcmp(map->change, "remapped"));
        EMU_CHECK(map->payload == 0x60100 && map->generation == 1);
        parse(trace, "x55_nor_read tick=6 icount=7 pc=0x20 chip=0 offset=0x60100 addr=0x860100 value=0xffff size=2 array=1 source=cpu");
        EMU_CHECK(log.events[log.count - 1].block == 20);
        EMU_CHECK(log.events[log.count - 1].generation == 1);
        parse(trace, "x55_nor_read tick=7 icount=8 pc=0x22 chip=1 offset=0x60000 addr=0x460000 value=0xa1 size=1 array=1 source=pec");
        EMU_CHECK(log.events[log.count - 1].generation == 0);
        EMU_CHECK(!emu_qemu_eeprom_trace_parse(trace, "unrelated event", error, sizeof error));
        emu_qemu_eeprom_trace_detach(trace);
        EMU_CHECK(!emu_trace_close(sink));
    }
    const char *bad[] = {
        "x55_nor_read",
        "x55_nor_mutation",
        "x55_nor_unknown tick=1",
        "x55_nor_read tick=-1 icount=2 pc=0x0 chip=0 offset=0x0 addr=0x0 value=0xff size=1 array=1 source=cpu",
        "x55_nor_read tick=18446744073709551616 icount=2 pc=0x0 chip=0 offset=0x0 addr=0x0 value=0xff size=1 array=1 source=cpu",
        "x55_nor_read tick=1 icount=2 pc=0x0 chip=2 offset=0x0 addr=0x0 value=0xff size=1 array=1 source=cpu",
        "x55_nor_read tick=1 icount=2 pc=0x0 chip=0 offset=0x0 addr=0x0 value=0xff size=0 array=1 source=cpu",
        "x55_nor_read tick=1 icount=2 pc=0x0 chip=0 offset=0x0 addr=0x0 value=0xff size=1 array=2 source=cpu",
        "x55_nor_read tick=1 icount=2 pc=0x0 chip=0 offset=0x0 addr=0x0 value=0xff size=1 array=1 source=gdb",
        "x55_nor_read tick=1 icount=2 pc=0x0 chip=0 offset=0x0 addr=0x0 value=0xff size=1 array=1 source=cpu junk",
        "x55_nor_read tick=1 icount=2 pc=0x0 chip=0 offset=0x0 addr=0x0 value=0x1ff size=1 array=1 source=cpu",
        "x55_nor_read tick=1 icount=2 pc=0x0 chip=0 offset=0x0 addr=0x0 value=0x00 size=1 array=1 source=cpu",
        "x55_nor_mutation tick=1 icount=2 pc=0x0 chip=0 offset=0xffffffff size=2 operation=program value=0x0",
        "x55_nor_mutation tick=1 icount=2 pc=0x0 chip=0 offset=0x0 size=9 operation=program value=0x0",
        "x55_nor_mutation tick=1 icount=2 pc=0x0 chip=0 offset=0x0 size=1 operation=program value=0x100",
        "x55_nor_mutation tick=1 icount=2 pc=0x0 chip=0 offset=0x0 size=1 operation=erase value=0x1",
        "x55_nor_mutation tick=1 icount=2 pc=0x0 chip=0 offset=0x0 size=1 operation=rejected value=0x0",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        log_t log = {0};
        emu_trace_sink_t *sink = emu_trace_open_callback(
            EMU_TRACE_MASK_EEPROM, capture, &log);
        EMU_CHECK(!emu_qemu_eeprom_trace_attach(trace, sink, error, sizeof error));
        EMU_CHECK(emu_qemu_eeprom_trace_parse(trace, bad[i], error, sizeof error) < 0);
        EMU_CHECK(sink->failed);
        emu_qemu_eeprom_trace_detach(trace);
        EMU_CHECK(emu_trace_close(sink) == -1);
    }
    emu_qemu_eeprom_trace_destroy(&trace);
    EMU_CHECK(trace == NULL);
    EMU_CHECK(emu_qemu_eeprom_trace_create(&trace, &image, error, sizeof error) < 0);
    EMU_CHECK(trace == NULL);
}

int main(void) {
    test_history();
    puts("QEMU EEPROM trace tests passed");
    return 0;
}
