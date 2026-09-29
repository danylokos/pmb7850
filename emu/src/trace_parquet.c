#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "emu_trace_parquet.h"
#include "emu_trace_schema.h"

#include <carquet/carquet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PARQUET_ROWS 65536u

enum {
    TYPE_I64 = EMU_TRACE_SCHEMA_I64,
    TYPE_BOOL = EMU_TRACE_SCHEMA_BOOL,
    TYPE_NULL = EMU_TRACE_SCHEMA_NULL,
    TYPE_STR = EMU_TRACE_SCHEMA_STR,
};

typedef struct {
    uint64_t seq, icount;
    uint32_t pc, addr, value, info_start;
    int32_t size;
    size_t detail_off;
    uint16_t kind_id;
    uint8_t flags, info_n;
} direct_event_t;
typedef struct {
    size_t str_off;
    int64_t i64;
    uint16_t key_id;
    uint8_t type;
} direct_kv_t;
typedef struct {
    direct_event_t *events;
    size_t event_n;
    direct_kv_t *kvs;
    size_t kv_n, kv_cap;
    char *arena;
    size_t arena_n, arena_cap;
} direct_batch_t;
typedef struct {
    char *name;
    char *stem;
    unsigned types;
    int col_present, col_type, col_i64, col_bool, col_str;
} key_schema_t;
typedef struct {
    char *kind, *encoded;
    uint64_t rows, first_seq, last_seq;
    int have_seq;
    key_schema_t *keys;
    size_t key_n, key_cap;
    carquet_writer_t *writer;
    uint32_t rg_rows;
    char *parquet_path;
} kind_schema_t;
typedef struct {
    kind_schema_t *kinds;
    size_t kind_n, kind_cap;
    char error[512];
} schema_set_t;
typedef struct {
    int16_t *defs;
    int64_t *i64;
    int32_t *i32;
    uint8_t *boolean;
    carquet_byte_array_t *strings;
    size_t cap;
    int32_t *kv_index;
    size_t kv_cap;
} scratch_t;

static void set_error(char *out, size_t cap, const char *fmt, ...)
{
    if (!out || !cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

 static int grow(void **p, size_t * cap, size_t need, size_t item)
{
    if (need <= *cap)
        return 0;
    size_t n = *cap ? *cap * 2u : 16u;
    while (n < need) {
        if (n > SIZE_MAX / 2u) {
            n = need;
            break;
        }
        n *= 2u;
    }
    if (n > SIZE_MAX / item)
        return -1;
    void *q = realloc(*p, n * item);
    if (!q)
        return -1;
    *p = q;
    *cap = n;
    return 0;
}

static int cmp_kind(const void *a, const void *b)
{
    return strcmp(((const kind_schema_t *)a)->kind, ((const kind_schema_t *)b)->kind);
}

static int cmp_key(const void *a, const void *b)
{
    return strcmp(((const key_schema_t *)a)->name, ((const key_schema_t *)b)->name);
}

static kind_schema_t *find_kind(schema_set_t * set, const char *kind, int create)
{
    for (size_t i = 0; i < set->kind_n; i++)
        if (!strcmp(set->kinds[i].kind, kind))
            return &set->kinds[i];
    if (!create || grow((void **)&set->kinds, &set->kind_cap, set->kind_n + 1, sizeof *set->kinds))
        return NULL;
    kind_schema_t *k = &set->kinds[set->kind_n++];
    memset(k, 0, sizeof *k);
    k->kind = strdup(kind);
    return k->kind ? k : NULL;
}

static key_schema_t *find_key(kind_schema_t * kind, const char *name, int create)
{
    for (size_t i = 0; i < kind->key_n; i++)
        if (!strcmp(kind->keys[i].name, name))
            return &kind->keys[i];
    if (!create || grow((void **)&kind->keys, &kind->key_cap, kind->key_n + 1, sizeof *kind->keys))
        return NULL;
    key_schema_t *k = &kind->keys[kind->key_n++];
    memset(k, 0, sizeof *k);
    k->name = strdup(name);
    k->col_present = k->col_type = k->col_i64 = k->col_bool = k->col_str = -1;
    return k->name ? k : NULL;
}

static int valid_column_stem(const char *key)
{
    if (!key || !((*key >= 97 && *key <= 122) || *key == 95)) return 0;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        if (!((*p >= 97 && *p <= 122) || (*p >= 48 && *p <= 57) || *p == 95))
            return 0;
    return 1;
}

static int finalize_schema(schema_set_t *set)
{
    qsort(set->kinds, set->kind_n, sizeof *set->kinds, cmp_kind);
    for (size_t k = 0; k < set->kind_n; k++) {
        kind_schema_t *kind = &set->kinds[k];
        if (k && !strcmp(kind[-1].kind, kind->kind)) return -1;
        qsort(kind->keys, kind->key_n, sizeof *kind->keys, cmp_key);
        for (size_t j = 0; j < kind->key_n; j++) {
            key_schema_t *key = &kind->keys[j];
            if (!valid_column_stem(key->name) || !key->types ||
                (j && !strcmp(kind->keys[j - 1].name, key->name))) return -1;
            key->stem = strdup(key->name);
            if (!key->stem) return -1;
        }
    }
    return 0;
}

static void schema_free(schema_set_t * set)
{
    for (size_t i = 0; i < set->kind_n; i++) {
        kind_schema_t *k = &set->kinds[i];
        free(k->kind);
        free(k->encoded);
        free(k->parquet_path);
        for (size_t j = 0; j < k->key_n; j++) {
            free(k->keys[j].name);
            free(k->keys[j].stem);
        }
        free(k->keys);
    }
    free(set->kinds);
    memset(set, 0, sizeof *set);
}

 static char *percent_kind(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(s), need = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || strchr("-_.~", c);
        need += safe ? 1 : 3;
    }
    char *out = malloc(need + 1);
    if (!out)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || strchr("-_.~", c);
        if (safe)
            out[j++] = (char)c;
        else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 15];
        }
    }
    out[j] = 0;
    return out;
}

static int mkdir_one(const char *p)
{
    return mkdir(p, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

static int remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st)) {
        return errno == ENOENT ? 0 : -1;
    }
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        size_t n = strlen(path) + strlen(e->d_name) + 2;
        char *child = malloc(n);
        if (!child) {
            rc = -1;
            break;
        }
        snprintf(child, n, "%s/%s", path, e->d_name);
        if (remove_tree(child))
            rc = -1;
        free(child);
        if (rc)
            break;
    }
    closedir(d);
    return rc ? rc : rmdir(path);
}

static unsigned nonnull_types(unsigned t)
{
    return ! !(t & TYPE_I64) + ! !(t & TYPE_BOOL) + ! !(t & TYPE_STR);
}

static int setup_writer(kind_schema_t * k, const char *root, char *error, size_t cap)
{
    size_t dn = strlen(root) + strlen(k->encoded) + 7;
    k->parquet_path = malloc(dn + 20);
    if (!k->parquet_path) {
        set_error(error, cap, "out of memory");
        return -1;
    }
    snprintf(k->parquet_path, dn + 20, "%s/kind=%s", root, k->encoded);
    if (mkdir_one(k->parquet_path)) {
        set_error(error, cap, "mkdir %s: %s", k->parquet_path, strerror(errno));
        return -1;
    }
    strcat(k->parquet_path, "/part-00000.parquet");
    carquet_error_t ce = CARQUET_ERROR_INIT;
    carquet_schema_t *s = carquet_schema_create(&ce);
    if (!s) {
        set_error(error, cap, "Carquet schema: %s", ce.message);
        return -1;
    }
    carquet_logical_type_t string = { 0 };
    string.id = CARQUET_LOGICAL_STRING;
    int col = 0;
#define ADD(name,type,logical,rep) do{if(carquet_schema_add_column(s,name,type,logical,rep,0,0)!=CARQUET_OK)goto schema_fail;col++;}while(0)
    ADD("seq", CARQUET_PHYSICAL_INT64, NULL, CARQUET_REPETITION_REQUIRED);
    ADD("icount", CARQUET_PHYSICAL_INT64, NULL, CARQUET_REPETITION_REQUIRED);
    ADD("pc", CARQUET_PHYSICAL_INT32, NULL, CARQUET_REPETITION_REQUIRED);
    ADD("addr", CARQUET_PHYSICAL_INT32, NULL, CARQUET_REPETITION_OPTIONAL);
    ADD("size", CARQUET_PHYSICAL_INT32, NULL, CARQUET_REPETITION_OPTIONAL);
    ADD("value", CARQUET_PHYSICAL_INT64, NULL, CARQUET_REPETITION_OPTIONAL);
    ADD("detail", CARQUET_PHYSICAL_BYTE_ARRAY, &string, CARQUET_REPETITION_REQUIRED);
    for (size_t i = 0; i < k->key_n; i++) {
        key_schema_t *key = &k->keys[i];
        char name[512];
        int meta = (key->types & TYPE_NULL) || nonnull_types(key->types) > 1;
        if (meta) {
            snprintf(name, sizeof name, "info_%s_present", key->stem);
            key->col_present = col;
            ADD(name, CARQUET_PHYSICAL_BOOLEAN, NULL, CARQUET_REPETITION_REQUIRED);
            snprintf(name, sizeof name, "info_%s_type", key->stem);
            key->col_type = col;
            ADD(name, CARQUET_PHYSICAL_BYTE_ARRAY, &string, CARQUET_REPETITION_OPTIONAL);
        }
        if (key->types & TYPE_I64) {
            snprintf(name, sizeof name, "info_%s_i64", key->stem);
            key->col_i64 = col;
            ADD(name, CARQUET_PHYSICAL_INT64, NULL, CARQUET_REPETITION_OPTIONAL);
        }
        if (key->types & TYPE_BOOL) {
            snprintf(name, sizeof name, "info_%s_bool", key->stem);
            key->col_bool = col;
            ADD(name, CARQUET_PHYSICAL_BOOLEAN, NULL, CARQUET_REPETITION_OPTIONAL);
        }
        if (key->types & TYPE_STR) {
            snprintf(name, sizeof name, "info_%s_str", key->stem);
            key->col_str = col;
            ADD(name, CARQUET_PHYSICAL_BYTE_ARRAY, &string, CARQUET_REPETITION_OPTIONAL);
        }
    }
#undef ADD
    carquet_writer_options_t opts;
    carquet_writer_options_init(&opts);
    opts.compression = CARQUET_COMPRESSION_ZSTD;
    opts.compression_level = 3;
    opts.row_group_size = INT64_MAX;
    /* Carquet cannot concatenate independent BOOLEAN RLE streams in one page. */
    opts.max_rows_per_page = 1;
    opts.write_statistics = true;
    opts.created_by = "CEMU Trace Carquet";
    k->writer = carquet_writer_create(k->parquet_path, s, &opts, &ce);
    carquet_schema_free(s);
    if (!k->writer) {
        set_error(error, cap, "Carquet writer %s: %s", k->kind, ce.message);
        return -1;
    }
    if (carquet_writer_set_column_encoding(k->writer, 6,
                                           CARQUET_ENCODING_RLE_DICTIONARY) != CARQUET_OK)
        goto encoding_fail;
    for (size_t i = 0; i < k->key_n; i++) {
        key_schema_t *key = &k->keys[i];
        if (key->col_present >= 0 &&
            carquet_writer_set_column_encoding(k->writer, key->col_present,
                                               CARQUET_ENCODING_RLE) != CARQUET_OK)
            goto encoding_fail;
        if (key->col_bool >= 0 &&
            carquet_writer_set_column_encoding(k->writer, key->col_bool,
                                               CARQUET_ENCODING_RLE) != CARQUET_OK)
            goto encoding_fail;
        if (key->col_type >= 0 &&
            carquet_writer_set_column_encoding(k->writer, key->col_type,
                                               CARQUET_ENCODING_RLE_DICTIONARY) != CARQUET_OK)
            goto encoding_fail;
        if (key->col_str >= 0 &&
            carquet_writer_set_column_encoding(k->writer, key->col_str,
                                               CARQUET_ENCODING_RLE_DICTIONARY) != CARQUET_OK)
            goto encoding_fail;
    }
    return 0;
  encoding_fail:
    carquet_writer_abort(k->writer);
    k->writer = NULL;
    set_error(error, cap, "Carquet encoding setup failed for %s", k->kind);
    return -1;
  schema_fail:
    carquet_schema_free(s);
    set_error(error, cap, "Carquet schema column failure for %s", k->kind);
    return -1;
}

static int scratch_reserve(scratch_t *s, size_t n, size_t keys)
{
    if (n > s->cap) {
        void *p = realloc(s->defs, n * sizeof *s->defs);
        if (!p) return -1;
        s->defs = p;
        p = realloc(s->i64, n * sizeof *s->i64);
        if (!p) return -1;
        s->i64 = p;
        p = realloc(s->i32, n * sizeof *s->i32);
        if (!p) return -1;
        s->i32 = p;
        p = realloc(s->boolean, n);
        if (!p) return -1;
        s->boolean = p;
        p = realloc(s->strings, n * sizeof *s->strings);
        if (!p) return -1;
        s->strings = p;
        s->cap = n;
    }
    if (keys && n > SIZE_MAX / keys) return -1;
    size_t cells = n * keys;
    if (cells > s->kv_cap) {
        void *p = realloc(s->kv_index, cells * sizeof *s->kv_index);
        if (!p) return -1;
        s->kv_index = p;
        s->kv_cap = cells;
    }
    return 0;
}

static void scratch_free(scratch_t *s)
{
    free(s->defs);
    free(s->i64);
    free(s->i32);
    free(s->boolean);
    free(s->strings);
    free(s->kv_index);
    memset(s, 0, sizeof *s);
}

static int cq_write(carquet_writer_t *w, int col, const void *v, size_t n,
                    const int16_t *defs, char *error, size_t cap)
{
    carquet_status_t st = carquet_writer_write_batch(w, col, v, (int64_t)n, defs, NULL);
    if (st != CARQUET_OK) {
        set_error(error, cap, "Carquet write column %d failed (%d)", col, (int)st);
        return -1;
    }
    return 0;
}

static const char *type_name(uint8_t type)
{
    return type == EMU_TRACE_VALUE_I64 ? "i64" : type == EMU_TRACE_VALUE_BOOL ? "bool" :
           type == EMU_TRACE_VALUE_NULL ? "null" : "str";
}

static const direct_event_t *batch_event(const direct_batch_t *batch,
                                         const uint32_t *rows, size_t row)
{
    return &batch->events[rows[row]];
}

static int write_range(kind_schema_t *kind, const direct_batch_t *batch,
                       const uint32_t *rows, size_t base, size_t n,
                       scratch_t *scratch, char *error, size_t cap)
{
    if (scratch_reserve(scratch, n, kind->key_n)) {
        set_error(error, cap, "out of memory");
        return -1;
    }
    if (kind->key_n) memset(scratch->kv_index, 0xff,
                            n * kind->key_n * sizeof *scratch->kv_index);
    for (size_t i = 0; i < n; i++) {
        const direct_event_t *event = batch_event(batch, rows, base + i);
        for (unsigned j = 0; j < event->info_n; j++) {
            size_t index = event->info_start + j;
            const direct_kv_t *kv = &batch->kvs[index];
            scratch->kv_index[i * kind->key_n + kv->key_id] = (int32_t)index;
        }
    }

    for (size_t i = 0; i < n; i++)
        scratch->i64[i] = (int64_t)batch_event(batch, rows, base + i)->seq;
    if (cq_write(kind->writer, 0, scratch->i64, n, NULL, error, cap)) return -1;
    for (size_t i = 0; i < n; i++)
        scratch->i64[i] = (int64_t)batch_event(batch, rows, base + i)->icount;
    if (cq_write(kind->writer, 1, scratch->i64, n, NULL, error, cap)) return -1;
    for (size_t i = 0; i < n; i++)
        scratch->i32[i] = (int32_t)batch_event(batch, rows, base + i)->pc;
    if (cq_write(kind->writer, 2, scratch->i32, n, NULL, error, cap)) return -1;

    size_t values = 0;
    for (size_t i = 0; i < n; i++) {
        const direct_event_t *event = batch_event(batch, rows, base + i);
        scratch->defs[i] = (event->flags & 1u) ? 1 : 0;
        if (scratch->defs[i]) scratch->i32[values++] = (int32_t)event->addr;
    }
    if (cq_write(kind->writer, 3, scratch->i32, n, scratch->defs, error, cap)) return -1;
    values = 0;
    for (size_t i = 0; i < n; i++) {
        const direct_event_t *event = batch_event(batch, rows, base + i);
        scratch->defs[i] = (event->flags & 2u) ? 1 : 0;
        if (scratch->defs[i]) scratch->i32[values++] = event->size;
    }
    if (cq_write(kind->writer, 4, scratch->i32, n, scratch->defs, error, cap)) return -1;
    values = 0;
    for (size_t i = 0; i < n; i++) {
        const direct_event_t *event = batch_event(batch, rows, base + i);
        scratch->defs[i] = (event->flags & 4u) ? 1 : 0;
        if (scratch->defs[i]) scratch->i64[values++] = event->value;
    }
    if (cq_write(kind->writer, 5, scratch->i64, n, scratch->defs, error, cap)) return -1;
    for (size_t i = 0; i < n; i++) {
        const char *detail = batch->arena + batch_event(batch, rows, base + i)->detail_off;
        scratch->strings[i].data = (uint8_t *)detail;
        scratch->strings[i].length = (int32_t)strlen(detail);
    }
    if (cq_write(kind->writer, 6, scratch->strings, n, NULL, error, cap)) return -1;

    int col = 7;
    for (size_t key_index = 0; key_index < kind->key_n; key_index++) {
        key_schema_t *key = &kind->keys[key_index];
        int metadata = (key->types & TYPE_NULL) || nonnull_types(key->types) > 1;
        if (metadata) {
            for (size_t i = 0; i < n; i++)
                scratch->boolean[i] = scratch->kv_index[i * kind->key_n + key_index] >= 0;
            if (cq_write(kind->writer, col++, scratch->boolean, n, NULL, error, cap)) return -1;
            values = 0;
            for (size_t i = 0; i < n; i++) {
                int32_t index = scratch->kv_index[i * kind->key_n + key_index];
                scratch->defs[i] = index >= 0;
                if (index >= 0) {
                    const char *name = type_name(batch->kvs[index].type);
                    scratch->strings[values].data = (uint8_t *)name;
                    scratch->strings[values++].length = (int32_t)strlen(name);
                }
            }
            if (cq_write(kind->writer, col++, scratch->strings, n, scratch->defs, error, cap)) return -1;
        }
        if (key->types & TYPE_I64) {
            values = 0;
            for (size_t i = 0; i < n; i++) {
                int32_t index = scratch->kv_index[i * kind->key_n + key_index];
                scratch->defs[i] = index >= 0 && batch->kvs[index].type == EMU_TRACE_VALUE_I64;
                if (scratch->defs[i]) scratch->i64[values++] = batch->kvs[index].i64;
            }
            if (cq_write(kind->writer, col++, scratch->i64, n, scratch->defs, error, cap)) return -1;
        }
        if (key->types & TYPE_BOOL) {
            values = 0;
            for (size_t i = 0; i < n; i++) {
                int32_t index = scratch->kv_index[i * kind->key_n + key_index];
                scratch->defs[i] = index >= 0 && batch->kvs[index].type == EMU_TRACE_VALUE_BOOL;
                if (scratch->defs[i]) scratch->boolean[values++] = !!batch->kvs[index].i64;
            }
            if (cq_write(kind->writer, col++, scratch->boolean, n, scratch->defs, error, cap)) return -1;
        }
        if (key->types & TYPE_STR) {
            values = 0;
            for (size_t i = 0; i < n; i++) {
                int32_t index = scratch->kv_index[i * kind->key_n + key_index];
                scratch->defs[i] = index >= 0 && batch->kvs[index].type == EMU_TRACE_VALUE_STRING;
                if (scratch->defs[i]) {
                    const char *text = batch->arena + batch->kvs[index].str_off;
                    scratch->strings[values].data = (uint8_t *)text;
                    scratch->strings[values++].length = (int32_t)strlen(text);
                }
            }
            if (cq_write(kind->writer, col++, scratch->strings, n, scratch->defs, error, cap)) return -1;
        }
    }
    return 0;
}

 static void json_string(FILE * f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", f);
            break;
        case '\\':
            fputs("\\\\", f);
            break;
        case '\b':
            fputs("\\b", f);
            break;
        case '\f':
            fputs("\\f", f);
            break;
        case '\n':
            fputs("\\n", f);
            break;
        case '\r':
            fputs("\\r", f);
            break;
        case '\t':
            fputs("\\t", f);
            break;
        default:
            if (*p < 0x20)
                fprintf(f, "\\u%04x", *p);
            else
                fputc(*p, f);
        }
    }
    fputc('"', f);
}

static void manifest_column(FILE * f, int *first, const char *name, const char *type)
{
    if (!*first)
        fputs(",\n", f);
    *first = 0;
    fputs("          {\"name\": ", f);
    json_string(f, name);
    fputs(", \"type\": ", f);
    json_string(f, type);
    fputc('}', f);
}

static int write_manifest(const char *root, uint64_t events,
                          const schema_set_t *set, char *error, size_t cap)
{
    size_t n = strlen(root) + 15;
    char *path = malloc(n);
    if (!path)
        return -1;
    snprintf(path, n, "%s/manifest.json", root);
    FILE *f = fopen(path, "wb");
    free(path);
    if (!f) {
        set_error(error, cap, "cannot write manifest: %s", strerror(errno));
        return -1;
    }
    fprintf(f,
            "{\n  \"compression\": \"zstd\",\n  \"events\": %llu,\n"
            "  \"finalized\": true,\n  \"format\": \"cemu-trace-parquet-v1\",\n  \"partitions\": [\n",
            (unsigned long long)events);
    int partition_first = 1;
    for (size_t x = 0; x < set->kind_n; x++) {
        kind_schema_t *k = &set->kinds[x];
        if (!k->rows) continue;
        if (!partition_first) fputs(",\n", f);
        partition_first = 0;
        fputs("    {\n      \"columns\": [\n", f);
        int first = 1;
        manifest_column(f, &first, "seq", "BIGINT");
        manifest_column(f, &first, "icount", "BIGINT");
        manifest_column(f, &first, "pc", "INTEGER");
        manifest_column(f, &first, "addr", "INTEGER");
        manifest_column(f, &first, "size", "INTEGER");
        manifest_column(f, &first, "value", "BIGINT");
        manifest_column(f, &first, "detail", "VARCHAR");
        for (size_t i = 0; i < k->key_n; i++) {
            key_schema_t *key = &k->keys[i];
            char name[512];
            int meta = (key->types & TYPE_NULL) || nonnull_types(key->types) > 1;
            if (meta) {
                snprintf(name, sizeof name, "info_%s_present", key->stem);
                manifest_column(f, &first, name, "BOOLEAN");
                snprintf(name, sizeof name, "info_%s_type", key->stem);
                manifest_column(f, &first, name, "VARCHAR");
            }
            if (key->types & TYPE_I64) {
                snprintf(name, sizeof name, "info_%s_i64", key->stem);
                manifest_column(f, &first, name, "BIGINT");
            }
            if (key->types & TYPE_BOOL) {
                snprintf(name, sizeof name, "info_%s_bool", key->stem);
                manifest_column(f, &first, name, "BOOLEAN");
            }
            if (key->types & TYPE_STR) {
                snprintf(name, sizeof name, "info_%s_str", key->stem);
                manifest_column(f, &first, name, "VARCHAR");
            }
        }
        fputs("\n      ],\n      \"first_seq\": ", f);
        fprintf(f, "%llu,\n      \"info\": {", (unsigned long long)k->first_seq);
        for (size_t i = 0; i < k->key_n; i++) {
            key_schema_t *key = &k->keys[i];
            if (i)
                fputc(',', f);
            fputs("\n        ", f);
            json_string(f, key->name);
            fputs(": {\"metadata_columns\": ", f);
            fputs((key->types & TYPE_NULL) || nonnull_types(key->types) > 1 ? "true" : "false", f);
            fputs(", \"stem\": ", f);
            json_string(f, key->stem);
            fputs(", \"types\": [", f);
            int tf = 1;
            const struct {
                unsigned bit;
                const char *name;
            } ts[] = { {
            TYPE_BOOL, "bool"}, {
            TYPE_I64, "i64"}, {
            TYPE_NULL, "null"}, {
            TYPE_STR, "str"}};
            for (unsigned t = 0; t < 4; t++)
                if (key->types & ts[t].bit) {
                    if (!tf)
                        fputs(", ", f);
                    tf = 0;
                    json_string(f, ts[t].name);
                }
            fputs("]}", f);
        }
        if (k->key_n)
            fputc('\n', f);
        fputs("      },\n      \"kind\": ", f);
        json_string(f, k->kind);
        fprintf(f, ",\n      \"last_seq\": %llu,\n      \"path\": ",
                (unsigned long long)k->last_seq);
        char rel[1024];
        snprintf(rel, sizeof rel, "kind=%s/part-00000.parquet", k->encoded);
        json_string(f, rel);
        fprintf(f, ",\n      \"rows\": %llu\n    }", (unsigned long long)k->rows);
    }
    fprintf(f, "\n  ],\n  \"row_group_rows\": %u,\n  \"schema\": 1\n}\n",
            PARQUET_ROWS);
    int rc = fclose(f);
    if (rc)
        set_error(error, cap, "cannot finalize manifest: %s", strerror(errno));
    return rc ? -1 : 0;
}

static char *absolute_path(const char *p)
{
    char *r = realpath(p, NULL);
    if (r)
        return r;
    if (p[0] == '/')
        return strdup(p);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd))
        return NULL;
    size_t n = strlen(cwd) + strlen(p) + 2;
    r = malloc(n);
    if (r)
        snprintf(r, n, "%s/%s", cwd, p);
    return r;
}

typedef struct {
    schema_set_t schema;
    direct_batch_t *active;
    scratch_t scratch;
    char *output;
    char *partial;
    pthread_t worker;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_cond_t space;
    direct_batch_t *queue[2];
    size_t queue_head, queue_count;
    int closing, worker_started;
    int mutex_initialized, ready_initialized, space_initialized;
    atomic_int failed;
    unsigned long serial;
    char error[512];
} direct_sink_t;

static atomic_ulong EMU_TRACE_SERIAL;
static atomic_int EMU_TRACE_FAIL_WORKER_AFTER = -1;

void emu_trace_test_fail_worker_after(int successful_batches) {
    atomic_store(&EMU_TRACE_FAIL_WORKER_AFTER, successful_batches);
}

static int catalog_init(schema_set_t *set) {
    size_t count = 0;
    const emu_trace_schema_kind_t *catalog = emu_trace_schema_catalog(&count);
    if (emu_trace_schema_validate_catalog(set->error, sizeof set->error))
        return -1;
    for (size_t i = 0; i < count; i++) {
        const emu_trace_schema_kind_t *src = &catalog[i];
        kind_schema_t *kind = find_kind(set, src->kind, 1);
        if (!kind) return -1;
        for (size_t j = 0; j < src->key_n; j++) {
            key_schema_t *key = find_key(kind, src->keys[j].name, 1);
            if (!key) return -1;
            key->types = src->keys[j].types;
        }
    }
    return finalize_schema(set);
}

static int direct_kind_index(const schema_set_t *set, const char *name) {
    size_t lo = 0, hi = set->kind_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, set->kinds[mid].kind);
        if (!cmp) return (int)mid;
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return -1;
}

static int direct_key_index(const kind_schema_t *kind, const char *name) {
    size_t lo = 0, hi = kind->key_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, kind->keys[mid].name);
        if (!cmp) return (int)mid;
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return -1;
}

static size_t direct_arena_add(direct_batch_t *batch, const char *text) {
    if (!text) text = "";
    size_t n = strlen(text) + 1;
    if (grow((void **)&batch->arena, &batch->arena_cap,
             batch->arena_n + n, 1)) return SIZE_MAX;
    size_t off = batch->arena_n;
    memcpy(batch->arena + off, text, n);
    batch->arena_n += n;
    return off;
}

static int direct_setup_writer(direct_sink_t *direct, kind_schema_t *kind,
                               char *error, size_t cap) {
    if (kind->writer) return 0;
    kind->encoded = percent_kind(kind->kind);
    if (!kind->encoded) {
        set_error(error, cap, "out of memory");
        return -1;
    }
    return setup_writer(kind, direct->partial, error, cap);
}

static int direct_write_batch(direct_sink_t *direct, direct_batch_t *batch,
                              char *error, size_t cap) {
    if (!batch->event_n) return 0;
    size_t kinds = direct->schema.kind_n;
    size_t *counts = calloc(kinds, sizeof *counts);
    size_t *starts = calloc(kinds + 1, sizeof *starts);
    size_t *cursor = calloc(kinds, sizeof *cursor);
    uint32_t *order = malloc(batch->event_n * sizeof *order);
    if (!counts || !starts || !cursor || !order) goto oom;
    for (size_t i = 0; i < batch->event_n; i++) counts[batch->events[i].kind_id]++;
    for (size_t k = 0; k < kinds; k++) starts[k + 1] = starts[k] + counts[k];
    memcpy(cursor, starts, kinds * sizeof *cursor);
    for (size_t i = 0; i < batch->event_n; i++) {
        unsigned k = batch->events[i].kind_id;
        order[cursor[k]++] = (uint32_t)i;
    }
    for (size_t kx = 0; kx < kinds; kx++) {
        if (!counts[kx]) continue;
        kind_schema_t *kind = &direct->schema.kinds[kx];
        if (direct_setup_writer(direct, kind, error, cap)) goto fail;
        size_t base = 0;
        while (base < counts[kx]) {
            if (kind->rg_rows == PARQUET_ROWS) {
                if (carquet_writer_new_row_group(kind->writer) != CARQUET_OK) {
                    set_error(error, cap, "cannot start row group for %s", kind->kind);
                    goto fail;
                }
                kind->rg_rows = 0;
            }
            size_t n = counts[kx] - base;
            size_t room = PARQUET_ROWS - kind->rg_rows;
            if (n > room) n = room;
            if (write_range(kind, batch, order + starts[kx], base, n,
                            &direct->scratch, error, cap)) goto fail;
            base += n;
            kind->rg_rows += (uint32_t)n;
        }
        if (!kind->have_seq) {
            kind->first_seq = batch->events[order[starts[kx]]].seq;
            kind->have_seq = 1;
        }
        kind->last_seq = batch->events[order[starts[kx] + counts[kx] - 1]].seq;
        kind->rows += counts[kx];
    }
    free(counts); free(starts); free(cursor); free(order);
    return 0;
oom:
    set_error(error, cap, "out of memory");
fail:
    free(counts); free(starts); free(cursor); free(order);
    return -1;
}

static direct_batch_t *direct_batch_new(void) {
    direct_batch_t *batch = calloc(1, sizeof *batch);
    if (!batch) return NULL;
    batch->events = calloc(EMU_TRACE_BATCH_EVENTS, sizeof *batch->events);
    if (!batch->events) { free(batch); return NULL; }
    return batch;
}

static void direct_batch_free(direct_batch_t *batch) {
    if (!batch) return;
    free(batch->events);
    free(batch->kvs);
    free(batch->arena);
    free(batch);
}

static void direct_fail(direct_sink_t *direct, const char *message) {
    pthread_mutex_lock(&direct->mutex);
    if (!atomic_load(&direct->failed)) {
        snprintf(direct->error, sizeof direct->error, "%s", message);
        atomic_store(&direct->failed, 1);
    }
    pthread_cond_broadcast(&direct->space);
    pthread_cond_broadcast(&direct->ready);
    pthread_mutex_unlock(&direct->mutex);
}

static void direct_emit_fail(emu_trace_sink_t *sink, const char *fmt, ...) {
    char message[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    direct_fail(sink->impl, message);
    sink->failed = 1;
}

static int direct_enqueue(direct_sink_t *direct, direct_batch_t *batch) {
    pthread_mutex_lock(&direct->mutex);
    while (direct->queue_count == 2 && !atomic_load(&direct->failed))
        pthread_cond_wait(&direct->space, &direct->mutex);
    if (atomic_load(&direct->failed)) {
        pthread_mutex_unlock(&direct->mutex);
        return -1;
    }
    size_t tail = (direct->queue_head + direct->queue_count) % 2;
    direct->queue[tail] = batch;
    direct->queue_count++;
    pthread_cond_signal(&direct->ready);
    pthread_mutex_unlock(&direct->mutex);
    return 0;
}

static void *direct_worker(void *opaque) {
    direct_sink_t *direct = opaque;
    for (;;) {
        pthread_mutex_lock(&direct->mutex);
        while (!direct->queue_count && !direct->closing)
            pthread_cond_wait(&direct->ready, &direct->mutex);
        if (!direct->queue_count && direct->closing) {
            pthread_mutex_unlock(&direct->mutex);
            break;
        }
        direct_batch_t *batch = direct->queue[direct->queue_head];
        direct->queue_head = (direct->queue_head + 1) % 2;
        direct->queue_count--;
        pthread_cond_signal(&direct->space);
        pthread_mutex_unlock(&direct->mutex);

        int fail_after = atomic_load(&EMU_TRACE_FAIL_WORKER_AFTER);
        if (fail_after == 0) {
            atomic_store(&EMU_TRACE_FAIL_WORKER_AFTER, -1);
            direct_fail(direct, "injected Parquet worker failure");
        } else if (fail_after > 0) {
            atomic_fetch_sub(&EMU_TRACE_FAIL_WORKER_AFTER, 1);
        }
        if (!atomic_load(&direct->failed)) {
            char error[512] = {0};
            if (direct_write_batch(direct, batch, error, sizeof error))
                direct_fail(direct, error[0] ? error : "Parquet batch write failed");
        }
        direct_batch_free(batch);
    }
    return NULL;
}

static void direct_emit(emu_trace_sink_t *sink, const emu_trace_event_t *event) {
    direct_sink_t *direct = sink->impl;
    if (atomic_load(&direct->failed)) { sink->failed = 1; return; }
    direct_batch_t *batch = direct->active;
    if (batch->event_n == EMU_TRACE_BATCH_EVENTS) {
        if (direct_enqueue(direct, batch)) { sink->failed = 1; return; }
        direct->active = NULL;
        batch = direct_batch_new();
        if (!batch) {
            direct_emit_fail(sink, "out of memory allocating Parquet trace batch");
            return;
        }
        direct->active = batch;
    }
    int kind_id = direct_kind_index(&direct->schema, event->kind ? event->kind : "");
    int info_n = event->info.n;
    if (kind_id < 0 || info_n < 0 || info_n > 32) {
        direct_emit_fail(sink, "undeclared trace kind %s", event->kind ? event->kind : "");
        return;
    }
    kind_schema_t *kind = &direct->schema.kinds[kind_id];
    size_t detail = direct_arena_add(batch, event->detail);
    if (detail == SIZE_MAX ||
        grow((void **)&batch->kvs, &batch->kv_cap,
             batch->kv_n + (size_t)info_n, sizeof *batch->kvs)) {
        direct_emit_fail(sink, "out of memory buffering Parquet trace event");
        return;
    }
    direct_event_t *dst = &batch->events[batch->event_n];
    *dst = (direct_event_t){
        .seq = sink->count, .icount = event->icount, .pc = event->pc,
        .addr = event->addr, .value = event->value, .size = event->size,
        .info_start = (uint32_t)batch->kv_n, .detail_off = detail,
        .kind_id = (uint16_t)kind_id, .info_n = (uint8_t)info_n,
        .flags = (uint8_t)((event->has_addr ? 1 : 0) |
                           (event->has_size ? 2 : 0) |
                           (event->has_value ? 4 : 0)),
    };
    for (int i = 0; i < info_n; i++) {
        const emu_trace_field_t *src = &event->info.kv[i];
        int key_id = direct_key_index(kind, src->key ? src->key : "");
        unsigned type = src->kind <= EMU_TRACE_VALUE_STRING ? 1u << src->kind : 0;
        if (key_id < 0 || !type || !(kind->keys[key_id].types & type)) {
            direct_emit_fail(sink, "undeclared trace field %s.%s type %u",
                             kind->kind, src->key ? src->key : "",
                             (unsigned)src->kind);
            return;
        }
        for (int j = 0; j < i; j++) {
            if (batch->kvs[dst->info_start + (uint32_t)j].key_id ==
                (uint16_t)key_id) {
                direct_emit_fail(sink, "duplicate trace field %s.%s",
                                 kind->kind, src->key ? src->key : "");
                return;
            }
        }
        direct_kv_t *kv = &batch->kvs[batch->kv_n++];
        *kv = (direct_kv_t){.i64 = src->ival, .key_id = (uint16_t)key_id,
                            .type = (uint8_t)src->kind};
        if (src->kind == EMU_TRACE_VALUE_STRING) {
            kv->str_off = direct_arena_add(batch, src->sval);
            if (kv->str_off == SIZE_MAX) {
                direct_emit_fail(sink, "out of memory buffering Parquet trace string");
                return;
            }
        }
    }
    batch->event_n++;
    sink->count++;
}

static int direct_close(emu_trace_sink_t *sink) {
    direct_sink_t *direct = sink->impl;
    if (sink->failed) atomic_store(&direct->failed, 1);
    if (!atomic_load(&direct->failed) && direct->active && direct->active->event_n) {
        if (direct_enqueue(direct, direct->active)) atomic_store(&direct->failed, 1);
        else direct->active = NULL;
    }
    direct_batch_free(direct->active);
    direct->active = NULL;
    pthread_mutex_lock(&direct->mutex);
    direct->closing = 1;
    pthread_cond_broadcast(&direct->ready);
    pthread_mutex_unlock(&direct->mutex);
    if (direct->worker_started) pthread_join(direct->worker, NULL);
    int failed = atomic_load(&direct->failed);
    for (size_t i = 0; i < direct->schema.kind_n; i++) {
        kind_schema_t *kind = &direct->schema.kinds[i];
        if (!kind->writer) continue;
        if (!failed && carquet_writer_close(kind->writer) != CARQUET_OK) {
            snprintf(direct->error, sizeof direct->error,
                     "cannot finalize Parquet for %s", kind->kind);
            failed = 1;
        } else if (failed) carquet_writer_abort(kind->writer);
        kind->writer = NULL;
    }
    if (!failed && write_manifest(direct->partial, sink->count,
                                  &direct->schema, direct->error,
                                  sizeof direct->error)) failed = 1;
    if (!failed) {
        size_t backup_n = strlen(direct->output) + 64;
        char *backup = malloc(backup_n);
        if (!backup) {
            set_error(direct->error, sizeof direct->error, "out of memory installing trace");
            failed = 1;
        } else {
            snprintf(backup, backup_n, "%s.old-%ld-%lu", direct->output,
                     (long)getpid(), direct->serial);
            if (remove_tree(backup)) {
                set_error(direct->error, sizeof direct->error,
                          "cannot remove stale trace backup %s: %s",
                          backup, strerror(errno));
                failed = 1;
            }
            int had_old = !access(direct->output, F_OK);
            if (!failed && had_old && rename(direct->output, backup)) {
                set_error(direct->error, sizeof direct->error,
                          "cannot stage existing trace %s: %s",
                          direct->output, strerror(errno));
                failed = 1;
            }
            if (!failed && rename(direct->partial, direct->output)) {
                int install_errno = errno;
                if (had_old && rename(backup, direct->output)) {
                    int restore_errno = errno;
                    set_error(direct->error, sizeof direct->error,
                              "cannot install trace: %s; cannot restore previous trace: %s",
                              strerror(install_errno), strerror(restore_errno));
                } else {
                    set_error(direct->error, sizeof direct->error,
                              "cannot install trace %s: %s",
                              direct->output, strerror(install_errno));
                }
                failed = 1;
            } else if (!failed && had_old && remove_tree(backup)) {
                set_error(direct->error, sizeof direct->error,
                          "trace installed but cannot remove backup %s: %s",
                          backup, strerror(errno));
                failed = 1;
            }
            free(backup);
        }
    }
    if (failed) {
        fprintf(stderr, "error: direct Parquet trace failed: %s\n",
                direct->error[0] ? direct->error : strerror(errno));
        remove_tree(direct->partial);
    }
    scratch_free(&direct->scratch);
    if (direct->space_initialized) pthread_cond_destroy(&direct->space);
    if (direct->ready_initialized) pthread_cond_destroy(&direct->ready);
    if (direct->mutex_initialized) pthread_mutex_destroy(&direct->mutex);
    free(direct->output); free(direct->partial);
    schema_free(&direct->schema);
    free(direct); free(sink);
    return failed ? -1 : 0;
}

emu_trace_sink_t *emu_trace_open(const char *path, emu_trace_mask_t mask) {
    emu_trace_sink_t *sink = calloc(1, sizeof *sink);
    direct_sink_t *direct = calloc(1, sizeof *direct);
    if (!sink || !direct || catalog_init(&direct->schema)) goto fail;
    direct->output = absolute_path(path);
    if (!direct->output) goto fail;
    direct->serial = atomic_fetch_add(&EMU_TRACE_SERIAL, 1) + 1;
    size_t n = strlen(direct->output) + 64;
    direct->partial = malloc(n);
    if (!direct->partial) goto fail;
    snprintf(direct->partial, n, "%s.partial-%ld-%lu", direct->output,
             (long)getpid(), direct->serial);
    if (remove_tree(direct->partial) || mkdir(direct->partial, 0777)) goto fail;
    if (pthread_mutex_init(&direct->mutex, NULL)) goto fail;
    direct->mutex_initialized = 1;
    if (pthread_cond_init(&direct->ready, NULL)) goto fail;
    direct->ready_initialized = 1;
    if (pthread_cond_init(&direct->space, NULL)) goto fail;
    direct->space_initialized = 1;
    direct->active = direct_batch_new();
    if (!direct->active) goto fail;
    if (pthread_create(&direct->worker, NULL, direct_worker, direct)) goto fail;
    direct->worker_started = 1;
    sink->mask = mask;
    sink->impl = direct;
    sink->backend_emit = direct_emit;
    sink->backend_close = direct_close;
    return sink;
fail:
    if (direct) {
        if (direct->worker_started) {
            pthread_mutex_lock(&direct->mutex); direct->closing = 1;
            pthread_cond_signal(&direct->ready); pthread_mutex_unlock(&direct->mutex);
            pthread_join(direct->worker, NULL);
        }
        if (direct->partial) remove_tree(direct->partial);
        direct_batch_free(direct->active);
        if (direct->space_initialized) pthread_cond_destroy(&direct->space);
        if (direct->ready_initialized) pthread_cond_destroy(&direct->ready);
        if (direct->mutex_initialized) pthread_mutex_destroy(&direct->mutex);
        free(direct->output); free(direct->partial);
        schema_free(&direct->schema);
    }
    free(direct); free(sink);
    return NULL;
}
