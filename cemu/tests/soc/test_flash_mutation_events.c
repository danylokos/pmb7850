#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc.h"
#include "flash.h"
#include "synth.h"
#include "cemu_core.h"
#include "cemu_core_diagnostics.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct {
    cemu_flash_mutation_event_t typed[64];
    size_t typed_count;
    struct {
        const char *kind;
        const char *chip_name;
        const char *model;
        long chip_index;
        long offset;
        long tick;
        int size;
        uint64_t icount;
        uint32_t pc;
    } normalized[64];
    size_t normalized_count;
    int sequence[192];
    size_t sequence_count;
} event_log_t;

enum { SEQ_START = 1, SEQ_TYPED, SEQ_NORMALIZED };

static long field_i64(const cemu_event_fields_t *info, const char *key) {
    for (int i = 0; i < info->n; i++)
        if (!strcmp(info->kv[i].key, key)) return info->kv[i].ival;
    return -1;
}

static const char *field_string(
        const cemu_event_fields_t *info, const char *key) {
    for (int i = 0; i < info->n; i++)
        if (!strcmp(info->kv[i].key, key)) return info->kv[i].sval;
    return NULL;
}

static int flash_filter(void *opaque, const char *kind) {
    (void)opaque;
    return !strncmp(kind, "flash_", 6);
}

static void event_consumer(void *opaque, const cemu_event_t *event) {
    event_log_t *log = opaque;
    if (event->type == CEMU_EVENT_FLASH_MUTATION) {
        CHECK(log->typed_count < 64);
        if (log->typed_count < 64)
            log->typed[log->typed_count++] = event->as.flash_mutation;
        log->sequence[log->sequence_count++] = SEQ_TYPED;
        return;
    }
    if (event->type != CEMU_EVENT_PERIPHERAL) return;
    const cemu_native_trace_event_t *trace = event->as.peripheral.trace;
    if (!strcmp(trace->kind, "flash_program_start") ||
        !strcmp(trace->kind, "flash_erase_start")) {
        log->sequence[log->sequence_count++] = SEQ_START;
        return;
    }
    if (strcmp(trace->kind, "flash_program_complete") &&
        strcmp(trace->kind, "flash_erase_complete"))
        return;
    CHECK(log->normalized_count < 64);
    if (log->normalized_count < 64) {
        size_t i = log->normalized_count++;
        log->normalized[i].kind = trace->kind;
        log->normalized[i].chip_name = field_string(&trace->info, "chip_name");
        log->normalized[i].model = field_string(&trace->info, "model");
        log->normalized[i].chip_index = field_i64(&trace->info, "chip_index");
        log->normalized[i].offset = field_i64(&trace->info, "operation_offset");
        log->normalized[i].tick = field_i64(&trace->info, "tick");
        log->normalized[i].size = trace->has_size ? trace->size : -1;
        log->normalized[i].icount = trace->icount;
        log->normalized[i].pc = trace->pc;
    }
    log->sequence[log->sequence_count++] = SEQ_NORMALIZED;
}

static void subscribe_events(soc_t *soc, event_log_t *log) {
    memset(log, 0, sizeof *log);
    CHECK(cemu_event_subscribe_filtered(
        &soc->instrumentation,
        (cemu_event_mask_t)(CEMU_EVENT_FLASH_MUTATION |
                            CEMU_EVENT_PERIPHERAL),
        event_consumer, flash_filter, log) != 0);
}

static void set_stamp(soc_t *soc, cpu_t *cpu,
                      uint64_t icount, uint32_t pc) {
    cpu->icount = icount;
    cpu->csp = (uint8_t)(pc >> 16);
    cpu->ip = (uint16_t)pc;
    cemu_soc_attach_cpu(soc, cpu);
}

static void check_pair(const event_log_t *log, size_t i,
                       cemu_flash_mutation_kind_t kind,
                       int chip_index, const char *chip_name,
                       const char *model, uint32_t offset, uint32_t size,
                       uint64_t tick, uint64_t icount, uint32_t pc) {
    CHECK(i < log->typed_count && i < log->normalized_count);
    if (i >= log->typed_count || i >= log->normalized_count) return;
    const cemu_flash_mutation_event_t *typed = &log->typed[i];
    CHECK(typed->kind == kind);
    CHECK(typed->chip_index == chip_index);
    CHECK(!strcmp(typed->chip_name, chip_name));
    CHECK(!strcmp(typed->model, model));
    CHECK(typed->offset == offset && typed->size == size);
    CHECK(typed->tick == tick && typed->icount == icount && typed->pc == pc);
    CHECK(!strcmp(log->normalized[i].kind,
                  kind == CEMU_FLASH_MUTATION_ERASE
                      ? "flash_erase_complete" : "flash_program_complete"));
    CHECK(log->normalized[i].chip_index == chip_index);
    CHECK(!strcmp(log->normalized[i].chip_name, chip_name));
    CHECK(!strcmp(log->normalized[i].model, model));
    CHECK(log->normalized[i].offset == (long)offset);
    CHECK(log->normalized[i].size == (int)size);
    CHECK(log->normalized[i].tick == (long)tick);
    CHECK(log->normalized[i].icount == icount &&
          log->normalized[i].pc == pc);
}

static void m58_program8(soc_t *soc, uint32_t off, uint8_t value) {
    bus_write8(&soc->bus, 0x800000u + off, 0x40);
    bus_write8(&soc->bus, 0x800000u + off, value);
}

static void m58_program16(soc_t *soc, uint32_t off, uint16_t value) {
    bus_write8(&soc->bus, 0x800000u + off, 0x40);
    bus_write16(&soc->bus, 0x800000u + off, value);
}

static void test_m58_synchronous_mutations(void) {
    const size_t size = 8u * 1024u * 1024u;
    uint8_t *image = malloc(size);
    CHECK(image != NULL);
    if (!image) return;
    memset(image, 0xFF, size);
    soc_t soc;
    CHECK(cemu_soc_init(&soc, image, size, cemu_device_by_name("c55"),
                        synth_defaults(), 0).code == CEMU_STATUS_OK);
    cpu_t cpu;
    cemu_cpu_init(&cpu, &soc.bus);
    set_stamp(&soc, &cpu, 41, 0x123456);
    soc.ticks = 17;
    event_log_t log;
    subscribe_events(&soc, &log);

    /* Accepted no-op and NOR-error targets are still committed programs. */
    m58_program8(&soc, 0x100, 0xFF);
    m58_program16(&soc, 0x102, 0xFFFF);
    m58_program8(&soc, 0x104, 0x00);
    m58_program8(&soc, 0x104, 0xFF);

    /* Commit order is retained, including adjacency, repetition/overlap, and
     * a noncontiguous entry within the same write-buffer page. */
    bus_write8(&soc.bus, 0x800120, 0xE8);
    bus_write16(&soc.bus, 0x800120, 3);
    bus_write16(&soc.bus, 0x800120, 0x1234);
    bus_write16(&soc.bus, 0x800122, 0x5678);
    bus_write16(&soc.bus, 0x800120, 0x9ABC);
    bus_write16(&soc.bus, 0x80012E, 0xDEF0);
    bus_write8(&soc.bus, 0x800120, 0xD0);

    bus_write8(&soc.bus, 0x820024, 0x20);
    bus_write8(&soc.bus, 0x820024, 0xD0);

    static const uint32_t offsets[] = {
        0x100, 0x102, 0x104, 0x104, 0x120, 0x122, 0x120, 0x12E,
        0x20000,
    };
    static const uint32_t lengths[] = {1, 2, 1, 1, 2, 2, 2, 2, 0x20000};
    CHECK(log.typed_count == 9 && log.normalized_count == 9);
    for (size_t i = 0; i < 9; i++)
        check_pair(&log, i,
                   i == 8 ? CEMU_FLASH_MUTATION_ERASE
                          : CEMU_FLASH_MUTATION_PROGRAM,
                   0, "flash", "m58lw064d", offsets[i], lengths[i],
                   17, 41, 0x123456);
    CHECK(log.sequence_count == 24);
    CHECK(log.sequence[0] == SEQ_START && log.sequence[1] == SEQ_TYPED &&
          log.sequence[2] == SEQ_NORMALIZED);
    CHECK(log.sequence[log.sequence_count - 3] == SEQ_START &&
          log.sequence[log.sequence_count - 2] == SEQ_TYPED &&
          log.sequence[log.sequence_count - 1] == SEQ_NORMALIZED);

    /* Unconfirmed/reset buffer traffic and direct debugger mutations emit no
     * committed guest event. */
    size_t count = log.typed_count;
    bus_write8(&soc.bus, 0x800140, 0xE8);
    bus_write16(&soc.bus, 0x800140, 0);
    bus_write16(&soc.bus, 0x800140, 0x1111);
    bus_write8(&soc.bus, 0x800000, 0xFF);
    cemu_memory_controller_poke8(&soc.memory, 0x800150, 0);
    CHECK(log.typed_count == count && log.normalized_count == count);

    cemu_soc_free(&soc);
    free(image);
}

static void am29_unlock(soc_t *soc, uint8_t command) {
    bus_write8(&soc->bus, 0x000AAA, 0xAA);
    bus_write8(&soc->bus, 0x000555, 0x55);
    bus_write8(&soc->bus, 0x000AAA, command);
}

static void am29_unlock_at(soc_t *soc, uint32_t off, uint8_t command) {
    bus_write8(&soc->bus, 0x000AAA, 0xAA);
    bus_write8(&soc->bus, 0x000555, 0x55);
    bus_write8(&soc->bus, off, command);
}

static void test_am29_program_order_and_timed_erase(void) {
    const size_t size = 8u * 1024u * 1024u;
    uint8_t *image = malloc(size);
    CHECK(image != NULL);
    if (!image) return;
    memset(image, 0xFF, size);
    soc_t soc;
    CHECK(cemu_soc_init(&soc, image, size, cemu_device_by_name("a60"),
                        synth_defaults(), 0).code == CEMU_STATUS_OK);
    cpu_t cpu;
    cemu_cpu_init(&cpu, &soc.bus);
    set_stamp(&soc, &cpu, 70, 0x234560);
    soc.ticks = 100;
    event_log_t log;
    subscribe_events(&soc, &log);

    am29_unlock(&soc, 0xA0);
    bus_write8(&soc.bus, 0x050080, 0xFF);
    am29_unlock(&soc, 0xA0);
    bus_write16(&soc.bus, 0x050082, 0xFFFF);

    am29_unlock_at(&soc, 0x050100, 0x25);
    bus_write16(&soc.bus, 0x050100, 3);
    bus_write16(&soc.bus, 0x050100, 0x1234);
    bus_write16(&soc.bus, 0x050102, 0x5678);
    bus_write16(&soc.bus, 0x050100, 0x9ABC);
    bus_write16(&soc.bus, 0x05010E, 0xDEF0);
    bus_write8(&soc.bus, 0x050000, 0x29);

    static const uint32_t offsets[] = {
        0x50080, 0x50082, 0x50100, 0x50102, 0x50100, 0x5010E,
    };
    static const uint32_t lengths[] = {1, 2, 2, 2, 2, 2};
    CHECK(log.typed_count == 6 && log.normalized_count == 6);
    for (size_t i = 0; i < 6; i++)
        check_pair(&log, i, CEMU_FLASH_MUTATION_PROGRAM, 0, "flash",
                   "am29lv640mh", offsets[i], lengths[i],
                   100, 70, 0x234560);

    am29_unlock(&soc, 0x80);
    bus_write8(&soc.bus, 0x000AAA, 0xAA);
    bus_write8(&soc.bus, 0x000555, 0x55);
    bus_write8(&soc.bus, 0x050000, 0x30);
    CHECK(log.typed_count == 6);
    cemu_soc_tick(&soc, AM29LV_SECTOR_ERASE_TICKS - 1u);
    CHECK(log.typed_count == 6);
    set_stamp(&soc, &cpu, 99, 0x345670);
    cemu_soc_tick(&soc, 1);
    CHECK(log.typed_count == 7 && log.normalized_count == 7);
    check_pair(&log, 6, CEMU_FLASH_MUTATION_ERASE, 0, "flash",
               "am29lv640mh", 0x50000, AM29LV_SECTOR_SIZE,
               100 + AM29LV_SECTOR_ERASE_TICKS, 99, 0x345670);

    /* SecSi/protection writes and an erase reset before completion are not
     * guest main-array completions. */
    size_t count = log.typed_count;
    am29_unlock(&soc, 0x88);
    am29_unlock(&soc, 0xA0);
    bus_write16(&soc.bus, 0x000020, 0);
    am29_unlock(&soc, 0x80);
    bus_write8(&soc.bus, 0x000AAA, 0xAA);
    bus_write8(&soc.bus, 0x000555, 0x55);
    bus_write8(&soc.bus, 0x060000, 0x30);
    cemu_soc_tick(&soc, 10);
    cemu_flash_reset(cemu_memory_controller_flash_state(&soc.memory, 0));
    cemu_soc_tick(&soc, AM29LV_SECTOR_ERASE_TICKS);
    CHECK(log.typed_count == count && log.normalized_count == count);

    cemu_soc_free(&soc);
    free(image);
}

static void w30_unlock(soc_t *soc, uint32_t cpu_off) {
    bus_write8(&soc->bus, 0x800000u + cpu_off, 0x60);
    bus_write8(&soc->bus, 0x800000u + cpu_off, 0xD0);
}

static void w30_start_program(soc_t *soc, uint32_t off, uint16_t value) {
    bus_write8(&soc->bus, 0x800000u + off, 0x40);
    bus_write16(&soc->bus, 0x800000u + off, value);
}

static void w30_start_erase(soc_t *soc, uint32_t off) {
    bus_write8(&soc->bus, 0x800000u + off, 0x20);
    bus_write8(&soc->bus, 0x800000u + off, 0xD0);
}

static void test_w30_timed_mutations_and_exclusions(void) {
    const size_t primary = 8u * 1024u * 1024u;
    const size_t total = 12u * 1024u * 1024u;
    uint8_t *image = malloc(total);
    CHECK(image != NULL);
    if (!image) return;
    memset(image, 0xFF, total);
    device_config_t cfg = *cemu_device_by_name("s55");
    cfg.flash_image = (device_flash_image_mapping_t){
        .file_offsets = {0, primary}, .count = 2,
    };
    soc_t soc;
    CHECK(cemu_soc_init(&soc, image, total, &cfg, synth_defaults(), 0).code ==
          CEMU_STATUS_OK);
    cpu_t cpu;
    cemu_cpu_init(&cpu, &soc.bus);
    set_stamp(&soc, &cpu, 120, 0x456780);
    soc.ticks = 500;
    event_log_t log;
    subscribe_events(&soc, &log);

    w30_unlock(&soc, 0x10000);
    w30_start_program(&soc, 0x10000, 0xFFFF);
    cemu_soc_tick(&soc, W30_PROGRAM_TICKS - 1u);
    CHECK(log.typed_count == 0);
    set_stamp(&soc, &cpu, 121, 0x456782);
    cemu_soc_tick(&soc, 1);
    check_pair(&log, 0, CEMU_FLASH_MUTATION_PROGRAM, 0, "flash-primary",
               W30_64MBIT_TOP_MODEL, 0x10000, 2,
               500 + W30_PROGRAM_TICKS, 121, 0x456782);

    uint64_t erase_start = soc.ticks;
    w30_start_erase(&soc, 0x10024);
    cemu_soc_tick(&soc, W30_ERASE_TICKS);
    check_pair(&log, 1, CEMU_FLASH_MUTATION_ERASE, 0, "flash-primary",
               W30_64MBIT_TOP_MODEL, 0x10000, W30_MAIN_BLOCK_SIZE,
               erase_start + W30_ERASE_TICKS, 121, 0x456782);

    erase_start = soc.ticks;
    w30_unlock(&soc, W30_64MBIT_TOP_PARAMETER_BASE + 0x2345);
    w30_start_erase(&soc, W30_64MBIT_TOP_PARAMETER_BASE + 0x2345);
    cemu_soc_tick(&soc, W30_ERASE_TICKS);
    check_pair(&log, 2, CEMU_FLASH_MUTATION_ERASE, 0, "flash-primary",
               W30_64MBIT_TOP_MODEL,
               W30_64MBIT_TOP_PARAMETER_BASE + W30_PARAMETER_BLOCK_SIZE,
               W30_PARAMETER_BLOCK_SIZE,
               erase_start + W30_ERASE_TICKS, 121, 0x456782);

    size_t count = log.typed_count;
    /* Protection storage completes on the same timer but is not main array. */
    bus_write8(&soc.bus, 0x80010A, 0xC0);
    bus_write16(&soc.bus, 0x80010A, 0x1234);
    cemu_soc_tick(&soc, W30_PROGRAM_TICKS);
    CHECK(log.typed_count == count && log.normalized_count == count);

    /* Locked requests, suspended operations reset before completion, startup
     * seeding, and direct debugger pokes produce no mutation event. */
    bus_write8(&soc.bus, 0x820000, 0x60);
    bus_write8(&soc.bus, 0x820000, 0x01);
    w30_start_program(&soc, 0x20000, 0);
    cemu_soc_tick(&soc, W30_PROGRAM_TICKS);
    w30_start_program(&soc, 0x30000, 0);
    cemu_soc_tick(&soc, 2);
    bus_write8(&soc.bus, 0x830000, 0xB0);
    cemu_flash_reset(cemu_memory_controller_flash_state(&soc.memory, 0));
    cemu_soc_tick(&soc, W30_PROGRAM_TICKS);
    CHECK(cemu_flash_seed_bytes(
        image, primary, cemu_memory_controller_flash_state(&soc.memory, 0),
        0x40000, (const uint8_t[]){0}, 1));
    cemu_memory_controller_poke8(&soc.memory, 0x840001, 0);
    CHECK(log.typed_count == count && log.normalized_count == count);

    CHECK(log.typed_count == 3 && log.normalized_count == 3);
    cemu_soc_free(&soc);
    free(image);
}

typedef struct {
    cemu_bus_event_t events[16];
    bus_transaction_t transactions[16];
    size_t count;
} bus_log_t;

static void bus_consumer(void *opaque, const cemu_event_t *event) {
    bus_log_t *log = opaque;
    if (event->type != CEMU_EVENT_BUS || log->count >= 16) return;
    size_t index = log->count++;
    log->events[index] = event->as.bus;
    log->transactions[index] = *event->as.bus.transaction;
    log->events[index].transaction = &log->transactions[index];
}

static void test_typed_bus_flash_targets_and_array_copy(void) {
    const size_t size = 8u * 1024u * 1024u;
    uint8_t *image = malloc(size);
    CHECK(image != NULL);
    if (!image) return;
    memset(image, 0xFF, size);
    image[0x123] = 0xA5;
    soc_t soc;
    CHECK(cemu_soc_init(&soc, image, size, cemu_device_by_name("c55"),
                        synth_defaults(), 0).code == CEMU_STATUS_OK);
    cpu_t cpu;
    cemu_cpu_init(&cpu, &soc.bus);
    set_stamp(&soc, &cpu, 222, 0x654321);
    soc.ticks = 333;
    bus_log_t log = {0};
    CHECK(cemu_event_subscribe(&soc.instrumentation, CEMU_EVENT_BUS,
                               bus_consumer, &log) != 0);

    CHECK(bus_read8(&soc.bus, 0x800123) == 0xA5);
    CHECK(bus_fetch8(&soc.bus, 0x800123) == 0xA5);
    bus_write8(&soc.bus, 0x800123, 0x90);
    (void)bus_read8(&soc.bus, 0x800123);
    CHECK(log.count == 4);
    for (size_t i = 0; i < log.count; i++) {
        CHECK(log.events[i].tick == 333 &&
              log.events[i].icount == 222 &&
              log.events[i].pc == 0x654321);
        CHECK(log.events[i].has_flash_target);
        CHECK(log.events[i].chip_index == 0);
        CHECK(!strcmp(log.events[i].chip_name, "flash"));
        CHECK(!strcmp(log.events[i].model, "m58lw064d"));
        CHECK(log.events[i].chip_offset == 0x123);
    }
    CHECK(log.events[0].flash_array_data);
    CHECK(log.transactions[0].kind == BUS_ACCESS_READ);
    CHECK(log.events[1].flash_array_data);
    CHECK(log.transactions[1].kind == BUS_ACCESS_FETCH);
    CHECK(!log.events[2].flash_array_data);
    CHECK(log.transactions[2].kind == BUS_ACCESS_WRITE);
    CHECK(!log.events[3].flash_array_data);

    cemu_core_t *core = NULL;
    CHECK(cemu_core_diagnostic_wrap_legacy(&core, &cpu, &soc).code ==
          CEMU_STATUS_OK);
    cemu_diagnostic_flash_chip_t info;
    CHECK(cemu_core_diagnostic_flash_chip_info(core, 0, &info).code ==
          CEMU_STATUS_OK);
    CHECK(info.chip_index == 0 && info.size == size &&
          !strcmp(info.chip_name, "flash") &&
          !strcmp(info.model, "m58lw064d"));
    uint8_t copied = 0;
    CHECK(cemu_core_diagnostic_flash_array_copy(
              core, 0, 0x123, &copied, 1).code == CEMU_STATUS_OK);
    CHECK(copied == 0xA5);
    /* The diagnostic copy does not escape ID mode. */
    CHECK(bus_read8(&soc.bus, 0x800000) != image[0]);
    cemu_core_destroy(core);

    cemu_soc_free(&soc);
    free(image);
}

int main(void) {
    test_m58_synchronous_mutations();
    test_am29_program_order_and_timed_erase();
    test_w30_timed_mutations_and_exclusions();
    test_typed_bus_flash_targets_and_array_copy();
    printf("flash mutation events: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
