#include "emu_qemu_eeprom_trace.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emu_eeprom_trace.h"
#include "emu_product.h"

struct emu_qemu_eeprom_trace {
    size_t count;
    emu_eeprom_trace_chip_t chips[EMU_MAX_CHIPS];
    char names[EMU_MAX_CHIPS][32];
    char models[EMU_MAX_CHIPS][32];
    uint8_t *initial[EMU_MAX_CHIPS];
    uint8_t *working[EMU_MAX_CHIPS];
    emu_eeprom_trace_t *decoder;
    emu_trace_sink_t *sink;
};

static int fail(emu_qemu_eeprom_trace_t *trace, char *error,
                size_t capacity, const char *message) {
    if (trace && trace->sink) trace->sink->failed = 1;
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return -1;
}

void emu_qemu_eeprom_trace_detach(emu_qemu_eeprom_trace_t *trace) {
    if (!trace) return;
    emu_eeprom_trace_destroy(&trace->decoder);
    trace->sink = NULL;
    for (size_t i = 0; i < trace->count; i++) {
        free(trace->working[i]);
        trace->working[i] = NULL;
    }
}

void emu_qemu_eeprom_trace_destroy(emu_qemu_eeprom_trace_t **out) {
    if (!out || !*out) return;
    emu_qemu_eeprom_trace_t *trace = *out;
    emu_qemu_eeprom_trace_detach(trace);
    for (size_t i = 0; i < trace->count; i++) free(trace->initial[i]);
    free(trace);
    *out = NULL;
}

int emu_qemu_eeprom_trace_create(emu_qemu_eeprom_trace_t **out,
        const emu_qemu_image_t *image, char *error, size_t capacity) {
    if (!out || *out || !image || !image->chip_count ||
        image->chip_count > EMU_MAX_CHIPS)
        return fail(NULL, error, capacity, "invalid QEMU EEPROM image");
    const emu_product_t *product = emu_product_by_name(image->device);
    if (!product || product->chip_count != image->chip_count)
        return fail(NULL, error, capacity, "invalid QEMU EEPROM chip topology");
    emu_qemu_eeprom_trace_t *trace = calloc(1, sizeof *trace);
    if (!trace) return fail(NULL, error, capacity, "cannot allocate EEPROM shadow");
    trace->count = image->chip_count;
    for (size_t i = 0; i < trace->count; i++) {
        const emu_qemu_chip_image_t *chip = &image->chips[i];
        if (!chip->size || chip->size > UINT32_MAX) goto failed;
        trace->initial[i] = malloc(chip->size);
        if (!trace->initial[i]) goto failed;
        FILE *file = fopen(chip->path, "rb");
        if (!file) goto failed;
        int bad = fread(trace->initial[i], 1, chip->size, file) != chip->size;
        if (!bad) bad = fgetc(file) != EOF || ferror(file);
        if (fclose(file)) bad = 1;
        if (bad) goto failed;
        snprintf(trace->names[i], sizeof trace->names[i], "%s", product->chips[i].name);
        snprintf(trace->models[i], sizeof trace->models[i], "%s", chip->model);
        trace->chips[i] = (emu_eeprom_trace_chip_t) {
            .chip_index = (int)i, .chip_name = trace->names[i],
            .model = trace->models[i], .size = chip->size,
        };
    }
    *out = trace;
    return 0;
failed:
    emu_qemu_eeprom_trace_destroy(&trace);
    return fail(NULL, error, capacity, "cannot capture prepared EEPROM chip bytes");
}

static int copy_shadow(void *opaque, int chip, size_t offset,
                       uint8_t *bytes, size_t size) {
    emu_qemu_eeprom_trace_t *trace = opaque;
    if (chip < 0 || (size_t)chip >= trace->count || !trace->working[chip] ||
        offset > trace->chips[chip].size || size > trace->chips[chip].size - offset)
        return -1;
    memcpy(bytes, trace->working[chip] + offset, size);
    return 0;
}

int emu_qemu_eeprom_trace_attach(emu_qemu_eeprom_trace_t *trace,
        emu_trace_sink_t *sink, char *error, size_t capacity) {
    if (!trace || !sink || trace->sink)
        return fail(NULL, error, capacity, "invalid QEMU EEPROM trace attachment");
    trace->sink = sink;
    for (size_t i = 0; i < trace->count; i++) {
        trace->working[i] = malloc(trace->chips[i].size);
        if (!trace->working[i]) goto failed;
        memcpy(trace->working[i], trace->initial[i], trace->chips[i].size);
    }
    if (emu_eeprom_trace_create(&trace->decoder, sink, trace->chips,
            trace->count, copy_shadow, trace) ||
        emu_eeprom_trace_arm(trace->decoder, 0, 0, 0)) goto failed;
    return 0;
failed:
    fail(trace, error, capacity, "cannot initialize QEMU EEPROM catalog");
    emu_qemu_eeprom_trace_detach(trace);
    return -1;
}

/* Strict unsigned fields: reject signs, overflow, missing separators and tails. */
static int number(const char **cursor, const char *label, int base,
                  uint64_t maximum, uint64_t *value) {
    size_t length = strlen(label);
    if (strncmp(*cursor, label, length)) return -1;
    const char *start = *cursor + length;
    if (*start < '0' || *start > '9') return -1;
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(start, &end, base);
    if (errno || end == start || parsed > maximum ||
        (*end && *end != ' ' && *end != '\r' && *end != '\n')) return -1;
    *cursor = end;
    *value = parsed;
    return 0;
}

static int end_line(const char *cursor) {
    return cursor[strspn(cursor, " \r\n")] == 0;
}

int emu_qemu_eeprom_trace_parse(emu_qemu_eeprom_trace_t *trace,
        const char *line, char *error, size_t capacity) {
    if (!line) return fail(trace, error, capacity, "missing QEMU EEPROM record");
    const int read = !strncmp(line, "x55_nor_read ", 13);
    const int mutation = !strncmp(line, "x55_nor_mutation ", 17);
    if (!read && !mutation) {
        if (!strncmp(line, "x55_nor_", 8))
            return fail(trace, error, capacity, "unknown QEMU physical NOR record");
        return 0;
    }
    if (!trace || !trace->decoder || !trace->sink || trace->sink->failed)
        return fail(trace, error, capacity, "QEMU EEPROM replay is unavailable");
    const char *cursor = line + (read ? 12 : 16);
    uint64_t tick, icount, pc, chip, offset, size, value, addr, array;
#define NUM(label, base, max, dest) \
    do { if (number(&cursor, label, base, max, &dest)) goto malformed; } while (0)
    NUM(" tick=", 10, UINT64_MAX, tick);
    NUM(" icount=", 10, UINT64_MAX, icount);
    NUM(" pc=", 16, UINT32_MAX, pc);
    NUM(" chip=", 10, trace->count - 1, chip);
    NUM(" offset=", 16, UINT32_MAX, offset);
    if (read) {
        NUM(" addr=", 16, UINT32_MAX, addr);
        NUM(" value=", 16, UINT32_MAX, value);
        NUM(" size=", 10, 4, size);
        NUM(" array=", 10, 1, array);
        if (strncmp(cursor, " source=cpu", 11) &&
            strncmp(cursor, " source=pec", 11)) goto malformed;
        cursor += 11;
    } else {
        NUM(" size=", 10, UINT32_MAX, size);
        if (!strncmp(cursor, " operation=program", 18)) {
            array = EMU_EEPROM_MUTATION_PROGRAM;
            cursor += 18;
        } else if (!strncmp(cursor, " operation=erase", 16)) {
            array = EMU_EEPROM_MUTATION_ERASE;
            cursor += 16;
        } else goto malformed;
        NUM(" value=", 16, UINT64_MAX, value);
    }
#undef NUM
    if (!size || offset > trace->chips[chip].size ||
        size > trace->chips[chip].size - offset || !end_line(cursor))
        goto malformed;
    if (read) {
        if (addr > UINT32_MAX - (size - 1) ||
            (size < 4 && value >> (size * 8))) goto malformed;
        if (array) {
            for (size_t i = 0; i < size; i++) {
                if (trace->working[chip][offset + i] !=
                    (uint8_t)(value >> (8 * i)))
                    return fail(trace, error, capacity,
                                "QEMU NOR array read disagrees with mutation replay");
            }
        }
        if (array) emu_eeprom_trace_read(trace->decoder, (int)chip,
            (uint32_t)offset, (uint32_t)addr, (uint32_t)value, (size_t)size,
            tick, icount, (uint32_t)pc);
    } else {
        uint8_t *bytes = trace->working[chip] + offset;
        if (array == EMU_EEPROM_MUTATION_ERASE) {
            if (value) goto malformed;
            memset(bytes, 0xff, (size_t)size);
        } else {
            if (size > 8 || (size < 8 && value >> (size * 8))) goto malformed;
            for (size_t i = 0; i < size; i++) bytes[i] = (uint8_t)(value >> (8 * i));
        }
        emu_eeprom_trace_mutation(trace->decoder, (int)chip,
            (emu_eeprom_trace_mutation_kind_t)array, (uint32_t)offset,
            (uint32_t)size, tick, icount, (uint32_t)pc);
    }
    if (trace->sink->failed)
        return fail(trace, error, capacity, "QEMU EEPROM catalog replay failed");
    return 1;
malformed:
    return fail(trace, error, capacity, "malformed QEMU physical NOR trace record");
}
