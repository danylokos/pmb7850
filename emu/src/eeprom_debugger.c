#include "emu_eeprom_debugger.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "emu_eeprom.h"

#define EEPROM_DEBUGGER_MAX_WATCHES 32u
#define EEPROM_DEBUGGER_SHOW_DEFAULT 256u
#define EEPROM_DEBUGGER_SHOW_MAX 4096u

enum { CLASS_RESOLVED, CLASS_SHADOWED, CLASS_METADATA, CLASS_COUNT };
enum { OP_READ, OP_PROGRAM, OP_ERASE, OP_COUNT };
enum { WATCH_READ = 1u, WATCH_WRITE = 2u };

typedef struct {
    int chip_index;
    uint16_t block_id;
    uint64_t counts[CLASS_COUNT][OP_COUNT];
    uint64_t bytes[CLASS_COUNT][OP_COUNT];
} block_stats_t;

typedef struct {
    unsigned id;
    int chip_index;
    uint16_t block_id;
    unsigned kinds;
    int log;
    uint64_t hits;
} logical_watch_t;

typedef struct {
    uint8_t *bytes;
    emu_eeprom_catalog_t catalog;
    int recognized;
} chip_snapshot_t;

struct emu_eeprom_debugger {
    emu_eeprom_trace_chip_t *chips;
    size_t chip_count;
    emu_eeprom_trace_copy_fn copy;
    void *copy_opaque;
    emu_eeprom_debugger_stop_fn stop;
    void *stop_opaque;
    emu_trace_sink_t *sink;
    emu_eeprom_trace_t *trace;
    block_stats_t *stats;
    size_t stats_count;
    size_t stats_capacity;
    logical_watch_t watches[EEPROM_DEBUGGER_MAX_WATCHES];
    size_t watch_count;
    unsigned next_watch_id;
    FILE *log_out;
    int suppress_maps;
    int failed;
};

static const emu_trace_field_t *event_field(
        const emu_trace_event_t *event, const char *key) {
    for (int i = 0; i < event->info.n; i++)
        if (!strcmp(event->info.kv[i].key, key))
            return &event->info.kv[i];
    return NULL;
}

static int event_int(
        const emu_trace_event_t *event, const char *key, long *value) {
    const emu_trace_field_t *field = event_field(event, key);
    if (!field || field->kind != EMU_TRACE_VALUE_I64) return 0;
    *value = field->ival;
    return 1;
}

static const char *event_string(
        const emu_trace_event_t *event, const char *key) {
    const emu_trace_field_t *field = event_field(event, key);
    return field && field->kind == EMU_TRACE_VALUE_STRING
         ? field->sval : NULL;
}

static const emu_eeprom_trace_chip_t *find_chip(
        const emu_eeprom_debugger_t *debugger, int chip_index) {
    for (size_t i = 0; i < debugger->chip_count; i++)
        if (debugger->chips[i].chip_index == chip_index)
            return &debugger->chips[i];
    return NULL;
}

static block_stats_t *find_stats(
        emu_eeprom_debugger_t *debugger, int chip_index,
        uint16_t block_id, int create) {
    for (size_t i = 0; i < debugger->stats_count; i++)
        if (debugger->stats[i].chip_index == chip_index &&
            debugger->stats[i].block_id == block_id)
            return &debugger->stats[i];
    if (!create) return NULL;
    if (debugger->stats_count == debugger->stats_capacity) {
        size_t capacity = debugger->stats_capacity
                        ? debugger->stats_capacity * 2u : 16u;
        block_stats_t *stats = realloc(
            debugger->stats, capacity * sizeof *stats);
        if (!stats) {
            debugger->failed = 1;
            return NULL;
        }
        debugger->stats = stats;
        debugger->stats_capacity = capacity;
    }
    block_stats_t *stats = &debugger->stats[debugger->stats_count++];
    *stats = (block_stats_t){
        .chip_index = chip_index,
        .block_id = block_id,
    };
    return stats;
}

static int attribution_index(const char *attribution) {
    if (attribution && !strcmp(attribution, "resolved"))
        return CLASS_RESOLVED;
    if (attribution && !strcmp(attribution, "shadowed"))
        return CLASS_SHADOWED;
    if (attribution && !strcmp(attribution, "metadata"))
        return CLASS_METADATA;
    return -1;
}

static int operation_index(const char *access) {
    if (access && !strcmp(access, "read")) return OP_READ;
    if (access && !strcmp(access, "program")) return OP_PROGRAM;
    if (access && !strcmp(access, "erase")) return OP_ERASE;
    return -1;
}

static const char *watch_kinds(unsigned kinds) {
    return kinds == WATCH_READ ? "r" : kinds == WATCH_WRITE ? "w" : "rw";
}

static void emit_watch_access(
        emu_eeprom_debugger_t *debugger, logical_watch_t *watch,
        const emu_trace_event_t *event, const char *attribution,
        const char *access, long offset, long tick) {
    const emu_eeprom_trace_chip_t *chip =
        find_chip(debugger, watch->chip_index);
    watch->hits++;
    char detail[640];
    snprintf(detail, sizeof detail,
             "id=%u chip=%s[%d] block=0x%04x attribution=%s "
             "access=%s offset=0x%lx size=%d tick=%ld",
             watch->id, chip ? chip->chip_name : "?", watch->chip_index,
             watch->block_id, attribution, access, offset,
             event->has_size ? event->size : 0, tick);
    if (watch->log) {
        FILE *out = debugger->log_out ? debugger->log_out : stderr;
        fprintf(out, "[eeprom-log] pc=0x%08x icount=%llu %s\n",
                event->pc, (unsigned long long)event->icount, detail);
        fflush(out);
    } else if (debugger->stop) {
        debugger->stop(debugger->stop_opaque, "eeprom-watch",
                       event->pc, event->icount, detail);
    }
}

static void handle_access(
        emu_eeprom_debugger_t *debugger,
        const emu_trace_event_t *event) {
    long chip_index, block_id, offset = 0, tick = 0;
    const char *attribution = event_string(event, "attribution");
    const char *access = event_string(event, "access");
    int class_index = attribution_index(attribution);
    int op_index = operation_index(access);
    if (class_index < 0 || op_index < 0 || !event->has_size ||
        !event_int(event, "chip_index", &chip_index) ||
        !event_int(event, "block_id", &block_id) ||
        block_id < 0 || block_id > UINT16_MAX)
        return;
    (void)event_int(event, "block_offset", &offset);
    (void)event_int(event, "tick", &tick);
    block_stats_t *stats = find_stats(
        debugger, (int)chip_index, (uint16_t)block_id, 1);
    if (!stats) return;
    stats->counts[class_index][op_index]++;
    stats->bytes[class_index][op_index] += (uint64_t)event->size;

    if (class_index == CLASS_METADATA) return;
    unsigned kind = op_index == OP_READ ? WATCH_READ : WATCH_WRITE;
    for (size_t i = 0; i < debugger->watch_count; i++) {
        logical_watch_t *watch = &debugger->watches[i];
        if (watch->chip_index == chip_index &&
            watch->block_id == block_id && (watch->kinds & kind))
            emit_watch_access(debugger, watch, event, attribution,
                              access, offset, tick);
    }
}

static void handle_map(
        emu_eeprom_debugger_t *debugger,
        const emu_trace_event_t *event) {
    if (debugger->suppress_maps) return;
    const char *change = event_string(event, "change");
    long chip_index, block_id, generation = 0, tick = 0;
    if (!change || !strcmp(change, "initial") ||
        !event_int(event, "chip_index", &chip_index) ||
        !event_int(event, "block_id", &block_id))
        return;
    (void)event_int(event, "mapping_generation", &generation);
    (void)event_int(event, "tick", &tick);
    for (size_t i = 0; i < debugger->watch_count; i++) {
        logical_watch_t *watch = &debugger->watches[i];
        if (!watch->log || watch->chip_index != chip_index ||
            watch->block_id != block_id)
            continue;
        const emu_eeprom_trace_chip_t *chip =
            find_chip(debugger, (int)chip_index);
        FILE *out = debugger->log_out ? debugger->log_out : stderr;
        fprintf(out,
                "[eeprom-map] pc=0x%08x icount=%llu id=%u chip=%s[%ld] "
                "block=0x%04lx change=%s generation=%ld tick=%ld\n",
                event->pc, (unsigned long long)event->icount, watch->id,
                chip ? chip->chip_name : "?", chip_index, block_id,
                change, generation, tick);
        fflush(out);
    }
}

static void trace_callback(
        void *opaque, const emu_trace_event_t *event, uint64_t sequence) {
    emu_eeprom_debugger_t *debugger = opaque;
    (void)sequence;
    if (!strcmp(event->kind, "eeprom_access"))
        handle_access(debugger, event);
    else if (!strcmp(event->kind, "eeprom_map"))
        handle_map(debugger, event);
}

static int setup_tracker(
        emu_eeprom_debugger_t *debugger,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    debugger->sink = emu_trace_open_callback(
        EMU_TRACE_MASK_EEPROM, trace_callback, debugger);
    if (!debugger->sink || emu_eeprom_trace_create(
            &debugger->trace, debugger->sink, debugger->chips,
            debugger->chip_count, debugger->copy,
            debugger->copy_opaque) ||
        emu_eeprom_trace_arm(debugger->trace, tick, icount, pc))
        return -1;
    return 0;
}

int emu_eeprom_debugger_create(
        emu_eeprom_debugger_t **out,
        const emu_eeprom_trace_chip_t *chips, size_t chip_count,
        emu_eeprom_trace_copy_fn copy, void *copy_opaque,
        emu_eeprom_debugger_stop_fn stop, void *stop_opaque,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    if (!out || *out || (!chips && chip_count) || !copy) return -1;
    emu_eeprom_debugger_t *debugger = calloc(1, sizeof *debugger);
    if (!debugger) return -1;
    debugger->chips = calloc(chip_count ? chip_count : 1u,
                             sizeof *debugger->chips);
    if (!debugger->chips) {
        free(debugger);
        return -1;
    }
    memcpy(debugger->chips, chips, chip_count * sizeof *chips);
    debugger->chip_count = chip_count;
    debugger->copy = copy;
    debugger->copy_opaque = copy_opaque;
    debugger->stop = stop;
    debugger->stop_opaque = stop_opaque;
    debugger->next_watch_id = 1;
    debugger->suppress_maps = 1;
    if (setup_tracker(debugger, tick, icount, pc)) {
        emu_eeprom_debugger_destroy(&debugger);
        return -1;
    }
    debugger->suppress_maps = 0;
    *out = debugger;
    return 0;
}

void emu_eeprom_debugger_destroy(emu_eeprom_debugger_t **pointer) {
    if (!pointer || !*pointer) return;
    emu_eeprom_debugger_t *debugger = *pointer;
    emu_eeprom_trace_destroy(&debugger->trace);
    (void)emu_trace_close(debugger->sink);
    free(debugger->stats);
    free(debugger->chips);
    free(debugger);
    *pointer = NULL;
}

void emu_eeprom_debugger_resync(
        emu_eeprom_debugger_t *debugger,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    if (!debugger) return;
    (void)tick;
    (void)icount;
    (void)pc;
    if (emu_eeprom_trace_resync(debugger->trace)) debugger->failed = 1;
}

void emu_eeprom_debugger_read(
        emu_eeprom_debugger_t *debugger, int chip_index,
        uint32_t chip_offset, uint32_t guest_addr, uint32_t value, size_t size,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    if (debugger && !debugger->failed)
        emu_eeprom_trace_read(debugger->trace, chip_index, chip_offset,
                              guest_addr, value, size, tick, icount, pc);
}

void emu_eeprom_debugger_mutation(
        emu_eeprom_debugger_t *debugger, int chip_index,
        emu_eeprom_trace_mutation_kind_t kind, uint32_t offset, uint32_t size,
        uint64_t tick, uint64_t icount, uint32_t pc) {
    if (debugger && !debugger->failed) {
        emu_eeprom_trace_mutation(debugger->trace, chip_index, kind,
                                  offset, size, tick, icount, pc);
        if (debugger->sink->failed) debugger->failed = 1;
    }
}

static void free_snapshots(
        chip_snapshot_t *snapshots, size_t count) {
    if (!snapshots) return;
    for (size_t i = 0; i < count; i++) {
        emu_eeprom_catalog_free(&snapshots[i].catalog);
        free(snapshots[i].bytes);
    }
    free(snapshots);
}

static chip_snapshot_t *take_snapshots(
        emu_eeprom_debugger_t *debugger) {
    chip_snapshot_t *snapshots = calloc(
        debugger->chip_count ? debugger->chip_count : 1u,
        sizeof *snapshots);
    if (!snapshots) return NULL;
    for (size_t i = 0; i < debugger->chip_count; i++) {
        size_t size = debugger->chips[i].size;
        snapshots[i].bytes = malloc(size ? size : 1u);
        if (!snapshots[i].bytes || debugger->copy(
                debugger->copy_opaque, debugger->chips[i].chip_index, 0,
                snapshots[i].bytes, size)) {
            free_snapshots(snapshots, debugger->chip_count);
            return NULL;
        }
        const emu_eeprom_catalog_t *tracked = emu_eeprom_trace_catalog(
            debugger->trace, debugger->chips[i].chip_index);
        if (!tracked) continue;
        emu_error_t error = {0};
        emu_error_code_t code = emu_eeprom_catalog_parse_region(
            snapshots[i].bytes, size, &tracked->region,
            &snapshots[i].catalog, &error);
        if (code == EMU_OK) snapshots[i].recognized = 1;
        else {
            free_snapshots(snapshots, debugger->chip_count);
            return NULL;
        }
    }
    return snapshots;
}

static int parse_number(const char *text, unsigned long *value) {
    if (!text || !*text || isspace((unsigned char)*text) || *text == '-')
        return 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (!end || *end) return 0;
    *value = parsed;
    return 1;
}

static int chip_position(
        const emu_eeprom_debugger_t *debugger, const char *token) {
    for (size_t i = 0; i < debugger->chip_count; i++)
        if (!strcmp(debugger->chips[i].chip_name, token)) return (int)i;
    unsigned long index;
    if (!parse_number(token, &index) || index > INT32_MAX) return -1;
    for (size_t i = 0; i < debugger->chip_count; i++)
        if (debugger->chips[i].chip_index == (int)index) return (int)i;
    return -1;
}

static int resolve_selector(
        emu_eeprom_debugger_t *debugger, chip_snapshot_t *snapshots,
        const char *selector, int require_mapped,
        int *chip_position_out, uint16_t *block_id_out, FILE *out) {
    char copy[128];
    if (!selector || strlen(selector) >= sizeof copy) goto invalid;
    snprintf(copy, sizeof copy, "%s", selector);
    char *colon = strchr(copy, ':');
    unsigned long block;
    if (colon) {
        *colon = 0;
        int position = chip_position(debugger, copy);
        if (position < 0 || !parse_number(colon + 1, &block) ||
            block > UINT16_MAX)
            goto invalid;
        if (require_mapped && (!snapshots[position].recognized ||
            !emu_eeprom_catalog_resolve(
                &snapshots[position].catalog, (uint16_t)block))) {
            fprintf(out, "EEPROM block not mapped: %s\n", selector);
            return 0;
        }
        *chip_position_out = position;
        *block_id_out = (uint16_t)block;
        return 1;
    }
    if (!parse_number(copy, &block) || block > UINT16_MAX) goto invalid;
    int found = -1;
    for (size_t i = 0; i < debugger->chip_count; i++) {
        int present = snapshots[i].recognized &&
            emu_eeprom_catalog_resolve(&snapshots[i].catalog,
                                       (uint16_t)block);
        if (!present && !require_mapped)
            present = find_stats(debugger, debugger->chips[i].chip_index,
                                 (uint16_t)block, 0) != NULL;
        if (!present) continue;
        if (found >= 0) {
            fprintf(out, "ambiguous EEPROM block 0x%04lx; qualify it as CHIP:BLOCK\n",
                    block);
            return 0;
        }
        found = (int)i;
    }
    if (found < 0) {
        fprintf(out, "EEPROM block not %s: 0x%04lx\n",
                require_mapped ? "mapped" : "observed", block);
        return 0;
    }
    *chip_position_out = found;
    *block_id_out = (uint16_t)block;
    return 1;
invalid:
    fprintf(out, "invalid EEPROM selector: %s\n", selector ? selector : "");
    return 0;
}

static int record_pointer_compare(const void *left, const void *right) {
    const emu_eeprom_record_t *const *a = left;
    const emu_eeprom_record_t *const *b = right;
    if ((*a)->id != (*b)->id) return (*a)->id < (*b)->id ? -1 : 1;
    if ((*a)->directory_chip_offset != (*b)->directory_chip_offset)
        return (*a)->directory_chip_offset < (*b)->directory_chip_offset
             ? -1 : 1;
    return 0;
}

static size_t shadow_count(
        const emu_eeprom_catalog_t *catalog, uint16_t block_id) {
    size_t count = 0;
    for (size_t i = 0; i < catalog->record_count; i++)
        if (catalog->records[i].id == block_id &&
            !catalog->records[i].resolved)
            count++;
    return count;
}

static void print_record_summary(
        FILE *out, const emu_eeprom_trace_chip_t *chip,
        const emu_eeprom_catalog_t *catalog,
        const emu_eeprom_record_t *record) {
    fprintf(out,
            "%s[%d] block=0x%04x (%u) state=%s version=%u length=%u "
            "descriptor=0x%zx payload=0x%zx linear=0x%08x shadows=%zu\n",
            chip->chip_name, chip->chip_index, record->id, record->id,
            record->active ? "active" : "fallback", record->version,
            record->length, record->directory_chip_offset,
            record->chip_offset, record->linear,
            shadow_count(catalog, record->id));
}

static void command_list(
        emu_eeprom_debugger_t *debugger, chip_snapshot_t *snapshots,
        int argc, char *const *argv, FILE *out) {
    if (argc > 3) {
        fprintf(out, "usage: eeprom list [CHIP]\n");
        return;
    }
    int only = -1;
    if (argc == 3 && (only = chip_position(debugger, argv[2])) < 0) {
        fprintf(out, "unknown flash chip: %s\n", argv[2]);
        return;
    }
    size_t shown = 0;
    for (size_t i = 0; i < debugger->chip_count; i++) {
        if (only >= 0 && (int)i != only) continue;
        const emu_eeprom_catalog_t *catalog = &snapshots[i].catalog;
        if (!snapshots[i].recognized) continue;
        const emu_eeprom_record_t **records = calloc(
            catalog->resolved_count ? catalog->resolved_count : 1u,
            sizeof *records);
        if (!records) return;
        size_t count = 0;
        for (size_t j = 0; j < catalog->record_count; j++)
            if (catalog->records[j].resolved)
                records[count++] = &catalog->records[j];
        qsort(records, count, sizeof *records, record_pointer_compare);
        for (size_t j = 0; j < count; j++) {
            print_record_summary(out, &debugger->chips[i], catalog,
                                 records[j]);
            shown++;
        }
        free(records);
    }
    if (!shown) fprintf(out, "no logical EEPROM blocks mapped\n");
}

static void print_hexdump(
        FILE *out, const uint8_t *bytes, size_t block_offset,
        size_t count) {
    for (size_t row = 0; row < count; row += 16u) {
        size_t width = count - row < 16u ? count - row : 16u;
        fprintf(out, "  0x%04zx: ", block_offset + row);
        char ascii[17];
        for (size_t i = 0; i < 16u; i++) {
            if (i < width) {
                uint8_t byte = bytes[row + i];
                fprintf(out, "%02x ", byte);
                ascii[i] = byte >= 32 && byte < 127 ? (char)byte : '.';
            } else {
                fprintf(out, "   ");
                ascii[i] = ' ';
            }
        }
        ascii[16] = 0;
        fprintf(out, " %s\n", ascii);
    }
}

static void command_show(
        emu_eeprom_debugger_t *debugger, chip_snapshot_t *snapshots,
        int argc, char *const *argv, FILE *out) {
    if (argc < 3 || argc > 5) {
        fprintf(out, "usage: eeprom show SELECTOR [OFFSET [COUNT]]\n");
        return;
    }
    int position;
    uint16_t block_id;
    if (!resolve_selector(debugger, snapshots, argv[2], 1,
                          &position, &block_id, out)) return;
    const emu_eeprom_catalog_t *catalog = &snapshots[position].catalog;
    const emu_eeprom_record_t *record =
        emu_eeprom_catalog_resolve(catalog, block_id);
    unsigned long offset = 0, count = EEPROM_DEBUGGER_SHOW_DEFAULT;
    if ((argc >= 4 && !parse_number(argv[3], &offset)) ||
        (argc >= 5 && !parse_number(argv[4], &count))) {
        fprintf(out, "invalid EEPROM show range\n");
        return;
    }
    if (count > EEPROM_DEBUGGER_SHOW_MAX) {
        fprintf(out, "EEPROM show count exceeds %u bytes\n",
                EEPROM_DEBUGGER_SHOW_MAX);
        return;
    }
    if (offset >= record->length) {
        fprintf(out, "EEPROM show offset 0x%lx is outside block length %u\n",
                offset, record->length);
        return;
    }
    if (count > record->length - offset)
        count = record->length - offset;
    print_record_summary(out, &debugger->chips[position], catalog, record);
    fprintf(out, "physical records:\n");
    for (size_t i = 0; i < catalog->record_count; i++) {
        const emu_eeprom_record_t *physical = &catalog->records[i];
        if (physical->id != block_id) continue;
        fprintf(out,
                "  %s version=%u length=%u descriptor=0x%zx "
                "payload=0x%zx linear=0x%08x\n",
                physical->resolved ? "resolved" : "shadowed",
                physical->version, physical->length,
                physical->directory_chip_offset, physical->chip_offset,
                physical->linear);
    }
    fprintf(out, "payload [0x%lx,0x%lx):\n", offset, offset + count);
    print_hexdump(out,
                  snapshots[position].bytes + record->chip_offset + offset,
                  offset, count);
}

static int stats_pointer_compare(const void *left, const void *right) {
    const block_stats_t *const *a = left;
    const block_stats_t *const *b = right;
    if ((*a)->chip_index != (*b)->chip_index)
        return (*a)->chip_index < (*b)->chip_index ? -1 : 1;
    if ((*a)->block_id != (*b)->block_id)
        return (*a)->block_id < (*b)->block_id ? -1 : 1;
    return 0;
}

static void print_stats(
        emu_eeprom_debugger_t *debugger, const block_stats_t *stats,
        FILE *out) {
    static const char *classes[] = {"resolved", "shadowed", "metadata"};
    static const char *operations[] = {"read", "program", "erase"};
    const emu_eeprom_trace_chip_t *chip =
        find_chip(debugger, stats->chip_index);
    fprintf(out, "%s[%d] block=0x%04x (%u)\n",
            chip ? chip->chip_name : "?", stats->chip_index,
            stats->block_id, stats->block_id);
    for (int c = 0; c < CLASS_COUNT; c++) {
        fprintf(out, "  %s", classes[c]);
        for (int op = 0; op < OP_COUNT; op++)
            fprintf(out, " %s=%llu/%lluB", operations[op],
                    (unsigned long long)stats->counts[c][op],
                    (unsigned long long)stats->bytes[c][op]);
        fputc('\n', out);
    }
}

static void command_stats(
        emu_eeprom_debugger_t *debugger, chip_snapshot_t *snapshots,
        int argc, char *const *argv, FILE *out) {
    if (argc > 3) {
        fprintf(out, "usage: eeprom stats [SELECTOR]\n");
        return;
    }
    if (argc == 3) {
        int position;
        uint16_t block_id;
        if (!resolve_selector(debugger, snapshots, argv[2], 0,
                              &position, &block_id, out)) return;
        block_stats_t *stats = find_stats(
            debugger, debugger->chips[position].chip_index, block_id, 0);
        if (stats) print_stats(debugger, stats, out);
        else fprintf(out, "%s: no observed EEPROM activity\n", argv[2]);
        return;
    }
    if (!debugger->stats_count) {
        fprintf(out, "no observed EEPROM activity\n");
        return;
    }
    block_stats_t **ordered = calloc(
        debugger->stats_count, sizeof *ordered);
    if (!ordered) return;
    for (size_t i = 0; i < debugger->stats_count; i++)
        ordered[i] = &debugger->stats[i];
    qsort(ordered, debugger->stats_count, sizeof *ordered,
          stats_pointer_compare);
    for (size_t i = 0; i < debugger->stats_count; i++)
        print_stats(debugger, ordered[i], out);
    free(ordered);
}

static unsigned parse_watch_kinds(const char *token) {
    if (!token || !strcmp(token, "rw")) return WATCH_READ | WATCH_WRITE;
    if (!strcmp(token, "r")) return WATCH_READ;
    if (!strcmp(token, "w")) return WATCH_WRITE;
    return 0;
}

static void command_add_watch(
        emu_eeprom_debugger_t *debugger, chip_snapshot_t *snapshots,
        int argc, char *const *argv, FILE *out, int log) {
    if (argc < 3 || argc > 4) {
        fprintf(out, "usage: eeprom %s SELECTOR [r|w|rw]\n",
                log ? "log" : "watch");
        return;
    }
    unsigned kinds = parse_watch_kinds(argc == 4 ? argv[3] : NULL);
    if (!kinds) {
        fprintf(out, "invalid EEPROM watch kind: %s\n", argv[3]);
        return;
    }
    int position;
    uint16_t block_id;
    if (!resolve_selector(debugger, snapshots, argv[2], 1,
                          &position, &block_id, out)) return;
    if (debugger->watch_count == EEPROM_DEBUGGER_MAX_WATCHES) {
        fprintf(out, "EEPROM watch limit reached (%u)\n",
                EEPROM_DEBUGGER_MAX_WATCHES);
        return;
    }
    logical_watch_t *watch = &debugger->watches[debugger->watch_count++];
    *watch = (logical_watch_t){
        .id = debugger->next_watch_id++,
        .chip_index = debugger->chips[position].chip_index,
        .block_id = block_id,
        .kinds = kinds,
        .log = log,
    };
    fprintf(out, "EEPROM %s %u set: %s[%d]:0x%04x %s\n",
            log ? "log" : "watch", watch->id,
            debugger->chips[position].chip_name, watch->chip_index,
            watch->block_id, watch_kinds(watch->kinds));
}

static void command_watches(
        emu_eeprom_debugger_t *debugger, chip_snapshot_t *snapshots,
        int argc, FILE *out) {
    if (argc != 2) {
        fprintf(out, "usage: eeprom watches\n");
        return;
    }
    if (!debugger->watch_count) {
        fprintf(out, "no logical EEPROM watches\n");
        return;
    }
    for (size_t i = 0; i < debugger->watch_count; i++) {
        const logical_watch_t *watch = &debugger->watches[i];
        const emu_eeprom_trace_chip_t *chip =
            find_chip(debugger, watch->chip_index);
        int position = chip ? (int)(chip - debugger->chips) : -1;
        int mapped = position >= 0 && snapshots[position].recognized &&
            emu_eeprom_catalog_resolve(&snapshots[position].catalog,
                                       watch->block_id);
        fprintf(out,
                "%u mode=%s selector=%s[%d]:0x%04x kinds=%s "
                "state=%s hits=%llu\n",
                watch->id, watch->log ? "log" : "watch",
                chip ? chip->chip_name : "?", watch->chip_index,
                watch->block_id, watch_kinds(watch->kinds),
                mapped ? "mapped" : "unmapped",
                (unsigned long long)watch->hits);
    }
}

static void command_unwatch(
        emu_eeprom_debugger_t *debugger, int argc,
        char *const *argv, FILE *out) {
    if (argc != 3) {
        fprintf(out, "usage: eeprom unwatch ID|all\n");
        return;
    }
    if (!strcmp(argv[2], "all")) {
        size_t count = debugger->watch_count;
        debugger->watch_count = 0;
        fprintf(out, "removed %zu logical EEPROM watch(es)\n", count);
        return;
    }
    unsigned long id;
    if (!parse_number(argv[2], &id) || id > UINT32_MAX) {
        fprintf(out, "invalid EEPROM watch ID: %s\n", argv[2]);
        return;
    }
    for (size_t i = 0; i < debugger->watch_count; i++) {
        if (debugger->watches[i].id != id) continue;
        memmove(&debugger->watches[i], &debugger->watches[i + 1u],
                (debugger->watch_count - i - 1u) * sizeof debugger->watches[0]);
        debugger->watch_count--;
        fprintf(out, "removed logical EEPROM watch %lu\n", id);
        return;
    }
    fprintf(out, "unknown logical EEPROM watch: %lu\n", id);
}

void emu_eeprom_debugger_help(
        emu_eeprom_debugger_t *debugger, FILE *out) {
    (void)debugger;
    fprintf(out,
            "  eeprom list [CHIP]       list current logical EEPROM blocks\n"
            "  eeprom show SEL [O [N]] show metadata and up to 4096 payload bytes\n"
            "  eeprom stats [SEL]       live direct activity since debugger attach\n"
            "  eeprom watch SEL [K]     stop on logical payload access (K=r,w,rw)\n"
            "  eeprom log SEL [K]       log logical payload accesses and remaps\n"
            "  eeprom watches           list logical EEPROM watches/logs\n"
            "  eeprom unwatch ID|all    remove logical EEPROM watches/logs\n");
}

int emu_eeprom_debugger_command(
        emu_eeprom_debugger_t *debugger, int argc,
        char *const *argv, FILE *out) {
    if (!debugger || argc < 1 || strcmp(argv[0], "eeprom")) return 0;
    debugger->log_out = out;
    if (debugger->failed) {
        fprintf(out, "EEPROM debugger observer failed\n");
        return 1;
    }
    if (argc < 2) {
        fprintf(out, "usage: eeprom list|show|stats|watch|log|watches|unwatch ...\n");
        return 1;
    }
    if (!strcmp(argv[1], "unwatch")) {
        command_unwatch(debugger, argc, argv, out);
        return 1;
    }
    chip_snapshot_t *snapshots = take_snapshots(debugger);
    if (!snapshots) {
        fprintf(out, "cannot inspect live flash EEPROM state\n");
        return 1;
    }
    if (!strcmp(argv[1], "list"))
        command_list(debugger, snapshots, argc, argv, out);
    else if (!strcmp(argv[1], "show"))
        command_show(debugger, snapshots, argc, argv, out);
    else if (!strcmp(argv[1], "stats"))
        command_stats(debugger, snapshots, argc, argv, out);
    else if (!strcmp(argv[1], "watch"))
        command_add_watch(debugger, snapshots, argc, argv, out, 0);
    else if (!strcmp(argv[1], "log"))
        command_add_watch(debugger, snapshots, argc, argv, out, 1);
    else if (!strcmp(argv[1], "watches"))
        command_watches(debugger, snapshots, argc, out);
    else
        fprintf(out, "unknown EEPROM command: %s\n", argv[1]);
    free_snapshots(snapshots, debugger->chip_count);
    return 1;
}
