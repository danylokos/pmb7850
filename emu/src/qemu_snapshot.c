#define _POSIX_C_SOURCE 200809L
#include "emu_qemu_snapshot.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define SNAPSHOT_JSON_MAX 65536u
#define SNAPSHOT_PAYLOAD_MAX (UINT64_C(512) * 1024 * 1024)

/* Small bounded JSON reader for this manifest, with exact object membership.
 * Tokens retain subtree ends so nested or duplicate keys cannot shadow fields. */
typedef struct { const char *start, *end; size_t next; } token_t;
typedef struct { const char *cursor; token_t tokens[512]; size_t count; } json_t;

static emu_error_code_t fail(emu_error_t *e, emu_error_code_t code,
                             const char *format, ...) {
    if (e) {
        va_list args;
        va_start(args, format);
        e->code = code;
        vsnprintf(e->message, sizeof e->message, format, args);
        va_end(args);
    }
    return code;
}

int emu_qemu_snapshot_path(char *out, size_t size, const char *dir,
                            const char *file) {
    int n = snprintf(out, size, "%s/%s", dir, file);
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

static void space(json_t *j) {
    while (strchr(" \t\r\n", *j->cursor) && *j->cursor) j->cursor++;
}

static int value(json_t *j, unsigned depth) {
    space(j);
    if (depth > 16 || j->count == 512) return 0;
    size_t index = j->count++;
    token_t *t = &j->tokens[index];
    t->start = j->cursor;
    char c = *j->cursor++;
    if (c == '"') {
        while (*j->cursor && *j->cursor != '"') {
            if ((unsigned char)*j->cursor < 32) return 0;
            if (*j->cursor++ == '\\') {
                c = *j->cursor++;
                if (!c || !strchr("\"\\/bfnrtu", c)) return 0;
                if (c == 'u')
                    for (unsigned i = 0; i < 4; i++)
                        if (!isxdigit((unsigned char)*j->cursor++)) return 0;
            }
        }
        if (*j->cursor++ != '"') return 0;
    } else if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        space(j);
        if (*j->cursor != close) for (;;) {
            if (c == '{') {
                space(j);
                if (*j->cursor != '"' || !value(j, depth + 1)) return 0;
                space(j);
                if (*j->cursor++ != ':') return 0;
            }
            if (!value(j, depth + 1)) return 0;
            space(j);
            if (*j->cursor != ',') break;
            j->cursor++;
        }
        if (*j->cursor++ != close) return 0;
    } else if (c == 't' && !strncmp(j->cursor, "rue", 3)) j->cursor += 3;
    else if (c == 'f' && !strncmp(j->cursor, "alse", 4)) j->cursor += 4;
    else if (c >= '0' && c <= '9') {
        if (c == '0' && isdigit((unsigned char)*j->cursor)) return 0;
        while (isdigit((unsigned char)*j->cursor)) j->cursor++;
    } else return 0;
    t->end = j->cursor;
    t->next = j->count;
    return 1;
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static unsigned hex4(const char *p) {
    unsigned v = 0;
    for (unsigned i = 0; i < 4; i++) v = v * 16 + hex_digit(p[i]);
    return v;
}

static int string(const json_t *j, size_t index, char *out, size_t cap) {
    if (index >= j->count || *j->tokens[index].start != '"') return 0;
    const char *p = j->tokens[index].start + 1;
    const char *end = j->tokens[index].end - 1;
    size_t used = 0;
    while (p < end) {
        unsigned c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (c == 'u') {
                c = hex4(p); p += 4;
                if (c >= 0xd800 && c <= 0xdbff) {
                    if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return 0;
                    unsigned low = hex4(p + 2); p += 6;
                    if (low < 0xdc00 || low > 0xdfff) return 0;
                    c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
                } else if (c >= 0xdc00 && c <= 0xdfff) return 0;
                if (!c || used + 4 >= cap) return 0;
                if (c >= 0x10000) out[used++] = 0xf0 | (c >> 18);
                if (c >= 0x800) out[used++] = (c >= 0x10000 ? 0x80 : 0xe0) | ((c >> 12) & 63);
                if (c >= 0x80) out[used++] = (c >= 0x800 ? 0x80 : 0xc0) | ((c >> 6) & 63);
                out[used++] = c < 0x80 ? c : 0x80 | (c & 63);
                continue;
            }
            static const char escapes[] = "bfnrt";
            const char *esc = strchr(escapes, c);
            if (esc) c = "\b\f\n\r\t"[esc - escapes];
        }
        if (!c || used + 1 >= cap) return 0;
        out[used++] = c;
    }
    out[used] = 0;
    return 1;
}

static size_t member(const json_t *j, size_t object, const char *name) {
    if (object >= j->count || *j->tokens[object].start != '{') return SIZE_MAX;
    size_t found = SIZE_MAX;
    for (size_t i = object + 1; i < j->tokens[object].next;) {
        char key[128];
        if (!string(j, i, key, sizeof key)) return SIZE_MAX;
        size_t v = i + 1;
        if (!strcmp(key, name)) {
            if (found != SIZE_MAX) return SIZE_MAX;
            found = v;
        }
        i = j->tokens[v].next;
    }
    return found;
}

static int number(const json_t *j, size_t i, uint64_t *out) {
    if (i >= j->count) return 0;
    uint64_t n = 0;
    for (const char *p = j->tokens[i].start; p < j->tokens[i].end; p++) {
        if (*p < '0' || *p > '9' || n > (UINT64_MAX - (*p - '0')) / 10) return 0;
        n = n * 10 + *p - '0';
    }
    *out = n;
    return 1;
}

static int boolean(const json_t *j, size_t i, int *out) {
    if (i >= j->count) return 0;
    if (*j->tokens[i].start == 't') *out = 1;
    else if (*j->tokens[i].start == 'f') *out = 0;
    else return 0;
    return 1;
}

static int unhex(const char *s, uint8_t *out, size_t size) {
    if (strlen(s) != size * 2) return 0;
    for (size_t i = 0; i < size; i++) {
        int a = hex_digit(s[i * 2]), b = hex_digit(s[i * 2 + 1]);
        if (a < 0 || b < 0) return 0;
        out[i] = (a << 4) | b;
    }
    return 1;
}

static int hash_valid(const char *s) {
    uint8_t hash[32];
    return unhex(s, hash, sizeof hash);
}

emu_error_code_t emu_qemu_snapshot_read(const char *dir,
    emu_qemu_snapshot_t *s, emu_error_t *e) {
    char path[EMU_QEMU_PATH_MAX];
    if (!dir || !s || emu_qemu_snapshot_path(path, sizeof path, dir, "snapshot.json"))
        return fail(e, EMU_ERR_ARGUMENT, "invalid QEMU snapshot directory");
    FILE *f = fopen(path, "rb");
    if (!f) return fail(e, EMU_ERR_IO, "cannot read snapshot manifest: %s", path);
    char *text = calloc(SNAPSHOT_JSON_MAX + 1, 1);
    if (!text) { fclose(f); return fail(e, EMU_ERR_NOMEM, "snapshot metadata allocation failed"); }
    size_t count = fread(text, 1, SNAPSHOT_JSON_MAX, f);
    int bad = ferror(f) || count == SNAPSHOT_JSON_MAX || memchr(text, 0, count);
    if (fclose(f)) bad = 1;
    json_t j = {.cursor = text};
    memset(s, 0, sizeof *s);
    uint64_t n = 0;
    char field[1024];
#define GETSTR(obj, key, dest) string(&j, member(&j, obj, key), dest, sizeof(dest))
#define GETNUM(obj, key, dest) number(&j, member(&j, obj, key), dest)
    if (bad || !value(&j, 0)) goto invalid;
    space(&j);
    if (*j.cursor || !GETSTR(0, "engine", field) || strcmp(field, "qemu") ||
        !GETSTR(0, "schema", field) || strcmp(field, "emu-qemu-snapshot") ||
        !GETNUM(0, "version", &n) || n != EMU_QEMU_SNAPSHOT_VERSION ||
        !GETNUM(0, "native_version", &s->native_version) ||
        s->native_version != EMU_QEMU_SNAPSHOT_NATIVE_VERSION ||
        !GETSTR(0, "flash", s->flash) || !s->flash[0] ||
        !GETSTR(0, "flash_sha256", s->flash_sha256) || !hash_valid(s->flash_sha256) ||
        !GETNUM(0, "flash_size", &s->flash_size) || !s->flash_size ||
        !GETSTR(0, "device", s->device) || !s->device[0] ||
        !GETSTR(0, "qemu_version", s->qemu_version) ||
        !boolean(&j, member(&j, 0, "sim_stub"), &s->sim_stub) ||
        !GETNUM(0, "icount", &s->icount) || !GETNUM(0, "ticks", &s->ticks) ||
        !GETNUM(0, "pc", &n) || n > 0xffffff) goto invalid;
    s->pc = n;
    size_t patches = member(&j, 0, "firmware_patches");
    if (patches >= j.count || *j.tokens[patches].start != '[') goto invalid;
    for (size_t i = patches + 1; i < j.tokens[patches].next; i = j.tokens[i].next) {
        if (!string(&j, i, field, sizeof field)) goto invalid;
        int id = emu_patch_id_by_name(field);
        if (id < 0 || s->firmware_patches & (UINT64_C(1) << id)) goto invalid;
        s->firmware_patches |= UINT64_C(1) << id;
    }
    size_t vm = member(&j, 0, "vmstate");
    if (!GETNUM(vm, "size", &s->vmstate_size) || !s->vmstate_size ||
        s->vmstate_size > SNAPSHOT_PAYLOAD_MAX ||
        !GETSTR(vm, "sha256", s->vmstate_sha256) || !hash_valid(s->vmstate_sha256)) goto invalid;
    size_t chips = member(&j, 0, "chips");
    if (chips >= j.count || *j.tokens[chips].start != '[') goto invalid;
    for (size_t i = chips + 1; i < j.tokens[chips].next; i = j.tokens[i].next) {
        if (s->chip_count == EMU_MAX_CHIPS) goto invalid;
        emu_qemu_chip_image_t *c = &s->chips[s->chip_count];
        if (!GETSTR(i, "model", c->model) ||
            !GETNUM(i, "size", &n) || !n || n > SNAPSHOT_PAYLOAD_MAX) goto invalid;
        c->size = n;
        if (!GETNUM(i, "source_offset", &n) || n > SIZE_MAX) goto invalid;
        c->source_offset = n;
        if (n > s->flash_size || c->size > s->flash_size - n ||
            !GETSTR(i, "sha256", s->chip_sha256[s->chip_count]) ||
            !hash_valid(s->chip_sha256[s->chip_count]) ||
            !GETSTR(i, "factory_uid", field)) goto invalid;
        if (field[0]) {
            if (!unhex(field, c->factory_uid, sizeof c->factory_uid)) goto invalid;
            c->factory_uid_set = 1;
        }
        if (!GETSTR(i, "am29_secsi", field)) goto invalid;
        if (field[0]) {
            if (!unhex(field, c->am29_secsi, sizeof c->am29_secsi)) goto invalid;
            c->am29_secsi_set = 1;
        }
        s->chip_count++;
    }
    if (!s->chip_count) goto invalid;
    free(text);
    return EMU_OK;
invalid:
    free(text);
    return fail(e, EMU_ERR_INVALID_PREPARED_SESSION,
                "invalid or incompatible QEMU snapshot manifest: %s", path);
#undef GETSTR
#undef GETNUM
}

static int file_hash(const char *path, uint64_t *size, char hash[65]) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    int bad = fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
              (uint64_t)st.st_size > SNAPSHOT_PAYLOAD_MAX;
    if (!bad) {
        void *bytes = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (bytes == MAP_FAILED) bad = 1;
        else {
            uint8_t digest[32];
            emu_sha256(bytes, st.st_size, digest);
            emu_sha256_hex(digest, hash);
            *size = st.st_size;
            munmap(bytes, st.st_size);
        }
    }
    if (close(fd)) bad = 1;
    return bad ? -1 : 0;
}

emu_error_code_t emu_qemu_snapshot_validate(const char *dir,
    const emu_qemu_snapshot_t *s, const emu_prepared_session_t *p, emu_error_t *e) {
    if (strcmp(s->device, p->selected_device) || s->flash_size != p->source.size ||
        strcmp(s->flash_sha256, p->source.sha256_hex) ||
        s->firmware_patches != p->options.firmware_patches ||
        s->chip_count != p->chip_count)
        return fail(e, EMU_ERR_INVALID_PREPARED_SESSION, "QEMU snapshot source or configuration mismatch");
    for (size_t i = 0; i < s->chip_count; i++) {
        const emu_qemu_chip_image_t *c = &s->chips[i];
        int am29 = !strcmp(c->model, "am29lv640mh") ||
                   !strcmp(c->model, "am29lv128mh");
        if ((c->factory_uid_set && (am29 || c->am29_secsi_set)) ||
            (c->am29_secsi_set && !am29) ||
            strcmp(c->model, p->chips[i].model) || c->size != p->chips[i].size ||
            c->source_offset != p->chips[i].source_offset ||
            (i == p->identity_chip_index && !c->factory_uid_set && !c->am29_secsi_set))
            return fail(e, EMU_ERR_INVALID_PREPARED_SESSION, "QEMU snapshot flash topology/identity mismatch");
    }
    for (size_t i = 0; i <= s->chip_count; i++) {
        char name[32] = "vmstate.bin", path[EMU_QEMU_PATH_MAX], hash[65];
        uint64_t size;
        if (i < s->chip_count) snprintf(name, sizeof name, "flash-%zu.bin", i);
        if (emu_qemu_snapshot_path(path, sizeof path, dir, name) ||
            file_hash(path, &size, hash) ||
            size != (i < s->chip_count ? s->chips[i].size : s->vmstate_size) ||
            strcmp(hash, i < s->chip_count ? s->chip_sha256[i] : s->vmstate_sha256))
            return fail(e, EMU_ERR_INVALID_PREPARED_SESSION, "QEMU snapshot payload missing or corrupt: %s", name);
    }
    return EMU_OK;
}

static int copy_file(const char *source, const char *dest, int exclusive) {
    int in = open(source, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dest, O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC), 0600);
    if (out < 0) { close(in); return -1; }
    uint8_t buf[65536];
    int bad = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) bad = 1;
        if (n <= 0) break;
        for (ssize_t offset = 0; offset < n;) {
            ssize_t written = write(out, buf + offset, n - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { bad = 1; break; }
            offset += written;
        }
        if (bad) break;
    }
    if (fsync(out)) bad = 1;
    if (close(in)) bad = 1;
    if (close(out)) bad = 1;
    if (bad && exclusive) unlink(dest);
    return bad ? -1 : 0;
}

emu_error_code_t emu_qemu_snapshot_materialize(const char *dir,
    const emu_qemu_snapshot_t *s, emu_qemu_image_t *image, emu_error_t *e) {
    for (size_t i = 0; i < s->chip_count; i++) {
        char name[32], source[EMU_QEMU_PATH_MAX];
        snprintf(name, sizeof name, "flash-%zu.bin", i);
        if (emu_qemu_snapshot_path(source, sizeof source, dir, name) ||
            copy_file(source, image->chips[i].path, 0))
            return fail(e, EMU_ERR_IO, "cannot materialize snapshot chip %zu", i);
        char path[EMU_QEMU_PATH_MAX];
        memcpy(path, image->chips[i].path, sizeof path);
        image->chips[i] = s->chips[i];
        memcpy(image->chips[i].path, path, sizeof path);
    }
    return EMU_OK;
}

static void quoted(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        if (*p < 32) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

static void hex(FILE *f, const uint8_t *bytes, size_t size) {
    fputc('"', f);
    for (size_t i = 0; i < size; i++) fprintf(f, "%02x", bytes[i]);
    fputc('"', f);
}

emu_error_code_t emu_qemu_snapshot_commit(const char *dir,
    emu_qemu_snapshot_t *s, const emu_qemu_image_t *image, emu_error_t *e) {
    char path[EMU_QEMU_PATH_MAX], temporary[EMU_QEMU_PATH_MAX];
    if (emu_qemu_snapshot_path(path, sizeof path, dir, "vmstate.bin") ||
        file_hash(path, &s->vmstate_size, s->vmstate_sha256))
        return fail(e, EMU_ERR_IO, "cannot hash snapshot VMState");
    int fd = open(path, O_RDONLY);
    if (fd < 0) return fail(e, EMU_ERR_IO, "cannot open snapshot VMState for sync");
    int bad = fsync(fd);
    if (close(fd)) bad = 1;
    if (bad) return fail(e, EMU_ERR_IO, "cannot sync snapshot VMState");
    s->chip_count = image->chip_count;
    size_t copied = 0;
    for (; copied < image->chip_count; copied++) {
        char name[32];
        snprintf(name, sizeof name, "flash-%zu.bin", copied);
        s->chips[copied] = image->chips[copied];
        uint64_t size;
        if (emu_qemu_snapshot_path(path, sizeof path, dir, name) ||
            copy_file(image->chips[copied].path, path, 1)) goto failed;
        if (file_hash(path, &size, s->chip_sha256[copied]) ||
            size != image->chips[copied].size) { unlink(path); goto failed; }
    }
    if (emu_qemu_snapshot_path(temporary, sizeof temporary, dir, "snapshot.json.tmp") ||
        emu_qemu_snapshot_path(path, sizeof path, dir, "snapshot.json")) goto failed;
    FILE *f = fopen(temporary, "wx");
    if (!f) goto failed;
    fprintf(f, "{\n  \"schema\": \"emu-qemu-snapshot\",\n  \"version\": %u,\n"
               "  \"engine\": \"qemu\",\n  \"native_version\": %u,\n  \"flash\": ",
               EMU_QEMU_SNAPSHOT_VERSION, EMU_QEMU_SNAPSHOT_NATIVE_VERSION);
    quoted(f, s->flash);
    fprintf(f, ",\n  \"flash_sha256\": \"%s\",\n  \"flash_size\": %" PRIu64 ",\n  \"device\": ",
               s->flash_sha256, s->flash_size);
    quoted(f, s->device);
    fputs(",\n  \"qemu_version\": ", f); quoted(f, s->qemu_version);
    fputs(",\n  \"firmware_patches\": ", f); emu_patch_print_json(f, s->firmware_patches);
    fprintf(f, ",\n  \"sim_stub\": %s,\n  \"icount\": %" PRIu64 ",\n"
               "  \"ticks\": %" PRIu64 ",\n  \"pc\": %u,\n"
               "  \"vmstate\": {\"size\": %" PRIu64 ", \"sha256\": \"%s\"},\n  \"chips\": [\n",
            s->sim_stub ? "true" : "false", s->icount, s->ticks, s->pc,
            s->vmstate_size, s->vmstate_sha256);
    for (size_t i = 0; i < image->chip_count; i++) {
        const emu_qemu_chip_image_t *c = &image->chips[i];
        fputs("    {\"model\": ", f); quoted(f, c->model);
        fprintf(f, ", \"source_offset\": %zu, \"size\": %zu, \"sha256\": \"%s\", \"factory_uid\": ",
                c->source_offset, c->size, s->chip_sha256[i]);
        hex(f, c->factory_uid, c->factory_uid_set ? sizeof c->factory_uid : 0);
        fputs(", \"am29_secsi\": ", f);
        hex(f, c->am29_secsi, c->am29_secsi_set ? sizeof c->am29_secsi : 0);
        fprintf(f, "}%s\n", i + 1 < image->chip_count ? "," : "");
    }
    fputs("  ]\n}\n", f);
    bad = ferror(f) || fflush(f) || fsync(fileno(f));
    if (fclose(f)) bad = 1;
    if (bad || link(temporary, path)) { unlink(temporary); goto failed; }
    unlink(temporary);
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { unlink(path); goto failed; }
    bad = fsync(fd);
    if (close(fd)) bad = 1;
    if (bad) { unlink(path); goto failed; }
    return EMU_OK;
failed:
    for (size_t i = 0; i < copied; i++) {
        char name[32];
        snprintf(name, sizeof name, "flash-%zu.bin", i);
        if (!emu_qemu_snapshot_path(path, sizeof path, dir, name)) unlink(path);
    }
    return fail(e, EMU_ERR_IO, "cannot publish QEMU snapshot in %s: %s", dir, strerror(errno));
}
