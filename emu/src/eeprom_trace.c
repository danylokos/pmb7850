#include "emu_eeprom_trace.h"

#include <stdlib.h>
#include <string.h>

#include "emu_eeprom.h"

typedef struct {
    emu_eeprom_trace_chip_t info;
    uint8_t *bytes;
    emu_eeprom_catalog_t catalog;
    uint64_t generation;
    int recognized;
} tracked_chip_t;

struct emu_eeprom_trace {
    emu_trace_sink_t *sink;
    tracked_chip_t *chips;
    size_t chip_count;
    emu_eeprom_trace_copy_fn copy;
    void *copy_opaque;
    int armed;
};

static void fail_trace(emu_eeprom_trace_t *trace) {
    if (trace && trace->sink) trace->sink->failed = 1;
}

static tracked_chip_t *find_chip(
        emu_eeprom_trace_t *trace, int chip_index) {
    if (!trace) return NULL;
    for (size_t i = 0; i < trace->chip_count; i++)
        if (trace->chips[i].info.chip_index == chip_index)
            return &trace->chips[i];
    return NULL;
}

static int record_compare(const void *left, const void *right) {
    const emu_eeprom_record_t *const *a = left;
    const emu_eeprom_record_t *const *b = right;
    if ((*a)->id != (*b)->id) return (*a)->id < (*b)->id ? -1 : 1;
    if ((*a)->directory_chip_offset != (*b)->directory_chip_offset)
        return (*a)->directory_chip_offset < (*b)->directory_chip_offset
             ? -1 : 1;
    return 0;
}

static const emu_eeprom_record_t **resolved_records(
        const emu_eeprom_catalog_t *catalog, size_t *count) {
    *count = catalog->resolved_count;
    if (!*count) return NULL;
    const emu_eeprom_record_t **records =
        malloc(*count * sizeof *records);
    if (!records) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < catalog->record_count; i++)
        if (catalog->records[i].resolved)
            records[n++] = &catalog->records[i];
    qsort(records, n, sizeof *records, record_compare);
    return records;
}

typedef struct {
    const char *active;
    const char *descriptor;
    const char *length;
    const char *linear;
    const char *payload;
    const char *version;
} record_field_names_t;

static const record_field_names_t CURRENT_FIELDS = {
    "current_active", "current_descriptor_offset", "current_length",
    "current_linear", "current_payload_offset", "current_version",
};
static const record_field_names_t PREVIOUS_FIELDS = {
    "previous_active", "previous_descriptor_offset", "previous_length",
    "previous_linear", "previous_payload_offset", "previous_version",
};

static void nullable_record_fields(
        emu_trace_info_t *info, const record_field_names_t *names,
        const emu_eeprom_record_t *record) {
    if (record) {
        emu_trace_info_bool(info, names->active, record->active);
        emu_trace_info_int(info, names->descriptor,
                           (long)record->directory_chip_offset);
        emu_trace_info_int(info, names->length, record->length);
        emu_trace_info_int(info, names->linear, record->linear);
        emu_trace_info_int(info, names->payload, (long)record->chip_offset);
        emu_trace_info_int(info, names->version, record->version);
    } else {
        emu_trace_info_null(info, names->active);
        emu_trace_info_null(info, names->descriptor);
        emu_trace_info_null(info, names->length);
        emu_trace_info_null(info, names->linear);
        emu_trace_info_null(info, names->payload);
        emu_trace_info_null(info, names->version);
    }
}

static void emit_map(
        emu_eeprom_trace_t *trace, const tracked_chip_t *chip,
        const char *change, const emu_eeprom_record_t *previous,
        const emu_eeprom_record_t *current,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    if (!emu_trace_sink_accepts(trace->sink, "eeprom_map")) return;
    const emu_eeprom_record_t *record = current ? current : previous;
    emu_trace_event_t event = {
        .kind = "eeprom_map", .icount = icount, .pc = pc,
        .detail = "EEPROM mapping",
    };
    emu_trace_info_int(&event.info, "block_id", record->id);
    emu_trace_info_str(&event.info, "change", change);
    emu_trace_info_int(&event.info, "chip_index", chip->info.chip_index);
    emu_trace_info_str(&event.info, "chip_name", chip->info.chip_name);
    nullable_record_fields(&event.info, &CURRENT_FIELDS, current);
    emu_trace_info_int(&event.info, "mapping_generation",
                       (long)chip->generation);
    emu_trace_info_str(&event.info, "model", chip->info.model);
    nullable_record_fields(&event.info, &PREVIOUS_FIELDS, previous);
    emu_trace_info_int(&event.info, "tick", (long)tick);
    emu_trace_emit(trace->sink, &event);
}

static int same_record(const emu_eeprom_record_t *a,
                       const emu_eeprom_record_t *b) {
    return a && b && a->id == b->id && a->active == b->active &&
           a->directory_chip_offset == b->directory_chip_offset &&
           a->chip_offset == b->chip_offset && a->linear == b->linear &&
           a->length == b->length && a->version == b->version;
}

static int initialize_chip(
        emu_eeprom_trace_t *trace, tracked_chip_t *chip,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    uint8_t *full = malloc(chip->info.size ? chip->info.size : 1u);
    if (!full) return -1;
    if (trace->copy(trace->copy_opaque, chip->info.chip_index, 0,
                    full, chip->info.size)) {
        free(full);
        return -1;
    }
    emu_error_t error = {0};
    emu_error_code_t code = emu_eeprom_catalog_parse(
        full, chip->info.size, &chip->catalog, &error);
    if (code == EMU_ERR_INVALID_PREPARED_SESSION) {
        free(full);
        return 0;
    }
    if (code != EMU_OK) {
        free(full);
        return -1;
    }
    chip->bytes = malloc(chip->catalog.region.size);
    if (!chip->bytes) {
        free(full);
        return -1;
    }
    memcpy(chip->bytes, full + chip->catalog.region.chip_offset,
           chip->catalog.region.size);
    free(full);
    chip->recognized = 1;
    size_t count = 0;
    const emu_eeprom_record_t **records =
        resolved_records(&chip->catalog, &count);
    if (count && !records) return -1;
    for (size_t i = 0; i < count; i++)
        emit_map(trace, chip, "initial", NULL, records[i],
                 tick, icount, pc);
    free(records);
    return 0;
}

int emu_eeprom_trace_create(
        emu_eeprom_trace_t **out, emu_trace_sink_t *sink,
        const emu_eeprom_trace_chip_t *chips, size_t chip_count,
        emu_eeprom_trace_copy_fn copy, void *copy_opaque) {
    if (!out || *out || !sink || (!chips && chip_count) || !copy)
        return -1;
    emu_eeprom_trace_t *trace = calloc(1, sizeof *trace);
    if (!trace) return -1;
    trace->chips = calloc(chip_count ? chip_count : 1u,
                          sizeof *trace->chips);
    if (!trace->chips) {
        free(trace);
        return -1;
    }
    trace->sink = sink;
    trace->chip_count = chip_count;
    trace->copy = copy;
    trace->copy_opaque = copy_opaque;
    for (size_t i = 0; i < chip_count; i++) trace->chips[i].info = chips[i];
    *out = trace;
    return 0;
}

int emu_eeprom_trace_arm(
        emu_eeprom_trace_t *trace, uint64_t tick,
        uint64_t icount, uint32_t pc) {
    if (!trace || trace->armed) return trace && trace->armed ? 0 : -1;
    for (size_t i = 0; i < trace->chip_count; i++) {
        if (initialize_chip(trace, &trace->chips[i], tick, icount, pc)) {
            fail_trace(trace);
            return -1;
        }
    }
    trace->armed = 1;
    return 0;
}

static int match_same(const emu_eeprom_match_t *a,
                      const emu_eeprom_match_t *b,
                      size_t distance) {
    if (a->area != b->area || b->offset != a->offset + distance)
        return 0;
    if (!a->record || !b->record) return a->record == b->record;
    return a->record->directory_chip_offset ==
           b->record->directory_chip_offset;
}

static void consider_boundary(size_t value, size_t offset, size_t *next) {
    if (value > offset && value < *next) *next = value;
}

static size_t next_classification_boundary(
        const emu_eeprom_catalog_t *catalog, size_t offset, size_t limit) {
    size_t next = limit;
    consider_boundary(catalog->region.chip_offset, offset, &next);
    consider_boundary(catalog->region.chip_offset + catalog->region.size,
                      offset, &next);
    for (size_t i = 0; i < catalog->region.header_count; i++) {
        size_t start = catalog->region.header_chip_offsets[i];
        consider_boundary(start, offset, &next);
        consider_boundary(start + 8u, offset, &next);
    }
    for (size_t i = 0; i < catalog->record_count; i++) {
        const emu_eeprom_record_t *record = &catalog->records[i];
        consider_boundary(record->directory_chip_offset, offset, &next);
        consider_boundary(record->directory_chip_offset +
                              EMU_EEPROM_DIRECTORY_RECORD_SIZE,
                          offset, &next);
        consider_boundary(record->chip_offset, offset, &next);
        consider_boundary(record->chip_offset + record->length,
                          offset, &next);
    }
    return next;
}

static const char *attribution_for(emu_eeprom_area_t area, int write) {
    switch (area) {
        case EMU_EEPROM_AREA_HEADER:
        case EMU_EEPROM_AREA_DESCRIPTOR: return "metadata";
        case EMU_EEPROM_AREA_RESOLVED_PAYLOAD: return "resolved";
        case EMU_EEPROM_AREA_SHADOWED_PAYLOAD: return "shadowed";
        case EMU_EEPROM_AREA_UNALLOCATED:
            return write ? "unattributed-journal" : "unattributed";
        case EMU_EEPROM_AREA_OUTSIDE: return "unattributed";
    }
    return "unattributed";
}

static uint32_t read_subvalue(uint32_t value, size_t byte_offset,
                              size_t size) {
    if (byte_offset >= 4u) return 0;
    uint32_t shifted = value >> (byte_offset * 8u);
    if (size >= 4u) return shifted;
    return shifted & ((UINT32_C(1) << (size * 8u)) - 1u);
}

static void emit_access(
        emu_eeprom_trace_t *trace, const tracked_chip_t *chip,
        const emu_eeprom_match_t *match, const char *access,
        size_t chip_offset, size_t size, int read,
        uint32_t guest_addr, uint32_t value, size_t read_delta,
        uint32_t mutation_offset, uint32_t mutation_size,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    if (!emu_trace_sink_accepts(trace->sink, "eeprom_access")) return;
    emu_trace_event_t event = {
        .kind = "eeprom_access", .icount = icount, .pc = pc,
        .has_addr = read, .addr = guest_addr + (uint32_t)read_delta,
        .has_size = 1, .size = (int)size,
        .has_value = read,
        .value = read ? read_subvalue(value, read_delta, size) : 0,
        .detail = "EEPROM logical access",
    };
    emu_trace_info_str(&event.info, "access", access);
    emu_trace_info_str(&event.info, "area", emu_eeprom_area_name(match->area));
    emu_trace_info_int(&event.info, "area_offset", (long)match->offset);
    emu_trace_info_str(&event.info, "attribution",
                       attribution_for(match->area, !read));
    if (match->record) {
        emu_trace_info_int(&event.info, "block_id", match->record->id);
        emu_trace_info_int(&event.info, "block_offset", (long)match->offset);
    } else {
        emu_trace_info_null(&event.info, "block_id");
        emu_trace_info_null(&event.info, "block_offset");
    }
    emu_trace_info_int(&event.info, "chip_index", chip->info.chip_index);
    emu_trace_info_str(&event.info, "chip_name", chip->info.chip_name);
    emu_trace_info_int(&event.info, "chip_offset", (long)chip_offset);
    emu_trace_info_int(&event.info, "mapping_generation",
                       (long)chip->generation);
    emu_trace_info_str(&event.info, "model", chip->info.model);
    if (read) {
        emu_trace_info_null(&event.info, "mutation_offset");
        emu_trace_info_null(&event.info, "mutation_size");
    } else {
        emu_trace_info_int(&event.info, "mutation_offset", mutation_offset);
        emu_trace_info_int(&event.info, "mutation_size", mutation_size);
    }
    emu_trace_info_int(&event.info, "tick", (long)tick);
    emu_trace_emit(trace->sink, &event);
}

static void emit_access_range(
        emu_eeprom_trace_t *trace, tracked_chip_t *chip,
        const char *access, size_t offset, size_t size, int read,
        uint32_t guest_addr, uint32_t value, size_t read_base,
        uint32_t mutation_offset, uint32_t mutation_size,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    size_t consumed = 0;
    while (consumed < size) {
        size_t segment_start = offset + consumed;
        emu_eeprom_match_t first;
        emu_eeprom_catalog_classify(
            &chip->catalog, segment_start, &first);
        if (first.area == EMU_EEPROM_AREA_OUTSIDE) {
            size_t next = next_classification_boundary(
                &chip->catalog, segment_start, offset + size);
            consumed += next - segment_start;
            continue;
        }
        size_t segment_end = next_classification_boundary(
            &chip->catalog, segment_start, offset + size);
        while (segment_end < offset + size) {
            emu_eeprom_match_t next;
            emu_eeprom_catalog_classify(
                &chip->catalog, segment_end, &next);
            if (!match_same(&first, &next,
                            segment_end - segment_start))
                break;
            segment_end = next_classification_boundary(
                &chip->catalog, segment_end, offset + size);
        }
        size_t span = segment_end - segment_start;
        emit_access(trace, chip, &first, access, segment_start, span,
                    read, guest_addr, value, read_base + consumed,
                    mutation_offset, mutation_size, tick, icount, pc);
        consumed += span;
    }
}

void emu_eeprom_trace_read(
        emu_eeprom_trace_t *trace, int chip_index, uint32_t chip_offset,
        uint32_t guest_addr, uint32_t value, size_t size,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    tracked_chip_t *chip = find_chip(trace, chip_index);
    if (!trace || !trace->armed || trace->sink->failed || !chip ||
        !chip->recognized || !size || chip_offset >= chip->info.size)
        return;
    size_t available = chip->info.size - chip_offset;
    if (size > available) size = available;
    emit_access_range(trace, chip, "read", chip_offset, size, 1,
                      guest_addr, value, 0, 0, 0, tick, icount, pc);
}

static int process_deltas(
        emu_eeprom_trace_t *trace, tracked_chip_t *chip,
        const emu_eeprom_catalog_t *old_catalog,
        const emu_eeprom_catalog_t *new_catalog,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    size_t old_count = 0, new_count = 0;
    const emu_eeprom_record_t **old_records =
        resolved_records(old_catalog, &old_count);
    const emu_eeprom_record_t **new_records =
        resolved_records(new_catalog, &new_count);
    if ((old_count && !old_records) || (new_count && !new_records)) {
        free(old_records);
        free(new_records);
        return -1;
    }
    size_t old_index = 0, new_index = 0;
    int changed = 0;
    while (old_index < old_count || new_index < new_count) {
        const emu_eeprom_record_t *old_record =
            old_index < old_count ? old_records[old_index] : NULL;
        const emu_eeprom_record_t *new_record =
            new_index < new_count ? new_records[new_index] : NULL;
        if (old_record && new_record && old_record->id == new_record->id) {
            if (!same_record(old_record, new_record)) changed = 1;
            old_index++;
            new_index++;
        } else if (!new_record ||
                   (old_record && old_record->id < new_record->id)) {
            changed = 1;
            old_index++;
        } else {
            changed = 1;
            new_index++;
        }
    }
    if (!changed) {
        free(old_records);
        free(new_records);
        return 0;
    }
    chip->generation++;
    old_index = new_index = 0;
    while (old_index < old_count || new_index < new_count) {
        const emu_eeprom_record_t *old_record =
            old_index < old_count ? old_records[old_index] : NULL;
        const emu_eeprom_record_t *new_record =
            new_index < new_count ? new_records[new_index] : NULL;
        if (old_record && new_record && old_record->id == new_record->id) {
            old_index++;
            new_index++;
            if (same_record(old_record, new_record)) continue;
        } else if (!new_record ||
                   (old_record && old_record->id < new_record->id)) {
            new_record = NULL;
            old_index++;
        } else {
            old_record = NULL;
            new_index++;
        }
        emit_map(trace, chip,
                 !old_record ? "added" : !new_record ? "removed" : "remapped",
                 old_record, new_record, tick, icount, pc);
    }
    free(old_records);
    free(new_records);
    return 0;
}

void emu_eeprom_trace_mutation(
        emu_eeprom_trace_t *trace, int chip_index,
        emu_eeprom_trace_mutation_kind_t kind,
        uint32_t offset, uint32_t size,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    tracked_chip_t *chip = find_chip(trace, chip_index);
    if (!trace || !trace->armed || trace->sink->failed || !chip ||
        !chip->recognized || !size || offset >= chip->info.size)
        return;
    size_t actual = size;
    if (actual > chip->info.size - offset)
        actual = chip->info.size - offset;
    size_t region_start = chip->catalog.region.chip_offset;
    size_t region_end = region_start + chip->catalog.region.size;
    size_t start = offset > region_start ? offset : region_start;
    size_t end = (size_t)offset + actual;
    if (end > region_end) end = region_end;
    if (start >= end) return;

    emit_access_range(trace, chip,
                      kind == EMU_EEPROM_MUTATION_ERASE ? "erase" : "program",
                      start, end - start, 0, 0, 0, 0,
                      offset, size, tick, icount, pc);

    if (trace->copy(trace->copy_opaque, chip_index, start,
                    chip->bytes + start - region_start, end - start)) {
        fail_trace(trace);
        return;
    }
    emu_eeprom_catalog_t refreshed = {0};
    emu_error_t error = {0};
    if (emu_eeprom_catalog_parse_cached_region(
            chip->bytes, chip->catalog.region.size, &chip->catalog.region,
            &refreshed, &error) != EMU_OK) {
        fail_trace(trace);
        return;
    }
    if (process_deltas(trace, chip, &chip->catalog, &refreshed,
                       tick, icount, pc)) {
        emu_eeprom_catalog_free(&refreshed);
        fail_trace(trace);
        return;
    }
    emu_eeprom_catalog_free(&chip->catalog);
    chip->catalog = refreshed;
}

int emu_eeprom_trace_resync(emu_eeprom_trace_t *trace) {
    if (!trace || !trace->armed || trace->sink->failed) return -1;
    for (size_t i = 0; i < trace->chip_count; i++) {
        tracked_chip_t *chip = &trace->chips[i];
        uint8_t *full = malloc(chip->info.size ? chip->info.size : 1u);
        if (!full || trace->copy(
                trace->copy_opaque, chip->info.chip_index, 0,
                full, chip->info.size)) {
            free(full);
            fail_trace(trace);
            return -1;
        }
        emu_eeprom_catalog_t refreshed = {0};
        emu_error_t error = {0};
        emu_error_code_t code = chip->recognized
            ? emu_eeprom_catalog_parse_region(
                  full, chip->info.size, &chip->catalog.region,
                  &refreshed, &error)
            : emu_eeprom_catalog_parse(
                  full, chip->info.size, &refreshed, &error);
        if (code == EMU_ERR_INVALID_PREPARED_SESSION && !chip->recognized) {
            free(full);
            continue;
        }
        if (code != EMU_OK) {
            free(full);
            fail_trace(trace);
            return -1;
        }
        uint8_t *cached = malloc(
            refreshed.region.size ? refreshed.region.size : 1u);
        if (!cached) {
            emu_eeprom_catalog_free(&refreshed);
            free(full);
            fail_trace(trace);
            return -1;
        }
        memcpy(cached, full + refreshed.region.chip_offset,
               refreshed.region.size);
        free(full);
        emu_eeprom_catalog_free(&chip->catalog);
        free(chip->bytes);
        chip->catalog = refreshed;
        chip->bytes = cached;
        chip->recognized = 1;
    }
    return 0;
}

const emu_eeprom_catalog_t *emu_eeprom_trace_catalog(
        const emu_eeprom_trace_t *trace, int chip_index) {
    if (!trace || !trace->armed) return NULL;
    for (size_t i = 0; i < trace->chip_count; i++)
        if (trace->chips[i].info.chip_index == chip_index)
            return trace->chips[i].recognized
                 ? &trace->chips[i].catalog : NULL;
    return NULL;
}

void emu_eeprom_trace_destroy(emu_eeprom_trace_t **pointer) {
    if (!pointer || !*pointer) return;
    emu_eeprom_trace_t *trace = *pointer;
    for (size_t i = 0; i < trace->chip_count; i++) {
        emu_eeprom_catalog_free(&trace->chips[i].catalog);
        free(trace->chips[i].bytes);
    }
    free(trace->chips);
    free(trace);
    *pointer = NULL;
}
