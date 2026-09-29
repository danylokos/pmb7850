#define _POSIX_C_SOURCE 200809L

#include "emu_artifact.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static int path_join(char *out, size_t cap,
                     const char *left, const char *right) {
    int count = snprintf(out, cap, "%s/%s", left, right);
    return count >= 0 && (size_t)count < cap ? 0 : -1;
}

static int ensure_directory(const char *path) {
    if (mkdir(path, 0777) == 0) return 0;
    if (errno != EEXIST) return -1;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static void icount_short(char *out, size_t cap, uint64_t count) {
    if (count >= UINT64_C(1000000)) {
        uint64_t whole = count / UINT64_C(1000000);
        uint64_t frac = count % UINT64_C(1000000) / UINT64_C(100000);
        if (frac)
            snprintf(out, cap, "%llum%llu", (unsigned long long)whole,
                     (unsigned long long)frac);
        else
            snprintf(out, cap, "%llum", (unsigned long long)whole);
    } else if (count >= 1000) {
        snprintf(out, cap, "%lluk", (unsigned long long)(count / 1000));
    } else {
        snprintf(out, cap, "%llu", (unsigned long long)count);
    }
}

void emu_artifact_today_mmdd(char *out, size_t cap) {
    time_t now = time(NULL);
    struct tm local;
    if (!out || !cap || !localtime_r(&now, &local)) return;
    snprintf(out, cap, "%02d%02d", local.tm_mon + 1, local.tm_mday);
}

int emu_artifact_run_open(emu_artifact_run_t *run, const char *root,
                          const char *device, const char *label,
                          const char *mmdd) {
    if (!run || !root || !root[0] || !device || !device[0] ||
        !mmdd || strlen(device) >= sizeof run->device ||
        strlen(mmdd) >= sizeof run->mmdd)
        return -1;
    memset(run, 0, sizeof *run);
    snprintf(run->root, sizeof run->root, "%s", root);
    snprintf(run->device, sizeof run->device, "%s", device);
    snprintf(run->mmdd, sizeof run->mmdd, "%s", mmdd);
    run->explicit_label = label && label[0];
    if (run->explicit_label) {
        if (path_join(run->path, sizeof run->path, root, label)) return -1;
    } else {
        int count = snprintf(run->path, sizeof run->path,
                             "%s/.%s-%s.partial", root, device, mmdd);
        if (count < 0 || (size_t)count >= sizeof run->path) return -1;
    }
    if (ensure_directory(root) || ensure_directory(run->path)) return -1;
    return 0;
}

static int register_artifact(emu_artifact_run_t *run, const char *kind,
                             const char *path) {
    if (run->registration_count >= EMU_ARTIFACT_REGISTRY_MAX) return -1;
    emu_artifact_registration_t *registration =
        &run->registrations[run->registration_count++];
    snprintf(registration->kind, sizeof registration->kind, "%s", kind);
    snprintf(registration->path, sizeof registration->path, "%s", path);
    return 0;
}

int emu_artifact_run_subdir(emu_artifact_run_t *run, const char *name,
                            char *out, size_t cap) {
    if (!run || !name || !name[0] || !out || !cap ||
        path_join(out, cap, run->path, name) || ensure_directory(out))
        return -1;
    return 0;
}

int emu_artifact_run_path(emu_artifact_run_t *run, const char *kind,
                          const char *relative, char *out, size_t cap) {
    if (!run || !kind || !relative || !out || !cap ||
        path_join(out, cap, run->path, relative) ||
        register_artifact(run, kind, relative))
        return -1;
    return 0;
}

int emu_artifact_run_finalize(emu_artifact_run_t *run, uint64_t icount) {
    if (!run || run->finalized) return -1;
    if (!run->explicit_label) {
        char short_count[24];
        char final_path[EMU_ARTIFACT_PATH_MAX];
        icount_short(short_count, sizeof short_count, icount);
        int count = snprintf(final_path, sizeof final_path,
                             "%s/%s-icount%s-%s", run->root, run->device,
                             short_count, run->mmdd);
        struct stat st;
        if (count < 0 || (size_t)count >= sizeof final_path)
            return -1;
        if (lstat(final_path, &st) == 0) {
            errno = EEXIST;
            return -1;
        }
        if (errno != ENOENT || rename(run->path, final_path) != 0) return -1;
        snprintf(run->path, sizeof run->path, "%s", final_path);
    }
    run->finalized = 1;
    return 0;
}

static void put_be32(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static int write_chunk(FILE *file, const char type[4],
                       const uint8_t *data, uint32_t length) {
    uint8_t word[4];
    put_be32(word, length);
    if (fwrite(word, 1, 4, file) != 4 ||
        fwrite(type, 1, 4, file) != 4) return -1;
    if (length && fwrite(data, 1, length, file) != length) return -1;
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (length) crc = crc32(crc, data, length);
    put_be32(word, (uint32_t)crc);
    return fwrite(word, 1, 4, file) == 4 ? 0 : -1;
}

int emu_png_write_rgb(const char *path, const uint8_t *rgb,
                      unsigned width, unsigned height) {
    static const uint8_t signature[8] = {137,80,78,71,13,10,26,10};
    if (!path || !rgb || !width || !height) return -1;
    size_t stride = (size_t)width * 3u;
    if (stride / 3u != width || height > SIZE_MAX / (stride + 1u)) return -1;
    size_t raw_size = (stride + 1u) * height;
    uint8_t *raw = malloc(raw_size);
    if (!raw) return -1;
    for (unsigned y = 0; y < height; y++) {
        raw[(stride + 1u) * y] = 0;
        memcpy(raw + (stride + 1u) * y + 1u,
               rgb + stride * y, stride);
    }
    uLongf compressed_size = compressBound(raw_size);
    uint8_t *compressed = malloc(compressed_size);
    if (!compressed) { free(raw); return -1; }
    int zresult = compress2(compressed, &compressed_size, raw, raw_size,
                            Z_BEST_SPEED);
    free(raw);
    if (zresult != Z_OK || compressed_size > UINT32_MAX) {
        free(compressed);
        return -1;
    }
    FILE *file = fopen(path, "wb");
    if (!file) { free(compressed); return -1; }
    uint8_t ihdr[13];
    put_be32(ihdr, width);
    put_be32(ihdr + 4, height);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    int ok = fwrite(signature, 1, sizeof signature, file) == sizeof signature &&
             write_chunk(file, "IHDR", ihdr, sizeof ihdr) == 0 &&
             write_chunk(file, "IDAT", compressed,
                         (uint32_t)compressed_size) == 0 &&
             write_chunk(file, "IEND", NULL, 0) == 0;
    int close_ok = fclose(file) == 0;
    free(compressed);
    return ok && close_ok ? 0 : -1;
}

static int gif_put_bytes(emu_gif_writer_t *writer,
                         const void *data, size_t size) {
    if (writer->failed || fwrite(data, 1, size, writer->file) != size) {
        writer->failed = 1;
        return -1;
    }
    return 0;
}

static int gif_put_byte(emu_gif_writer_t *writer, uint8_t value) {
    return gif_put_bytes(writer, &value, 1);
}

static int gif_put_le16(emu_gif_writer_t *writer, unsigned value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return gif_put_bytes(writer, bytes, sizeof bytes);
}

int emu_gif_open(emu_gif_writer_t *writer, const char *path,
                 unsigned width, unsigned height) {
    static const uint8_t loop_extension[] = {
        0x21,0xff,0x0b,'N','E','T','S','C','A','P','E','2','.','0',
        0x03,0x01,0x00,0x00,0x00,
    };
    if (!writer || !path || !width || !height ||
        width > UINT16_MAX || height > UINT16_MAX) return -1;
    memset(writer, 0, sizeof *writer);
    writer->file = fopen(path, "wb");
    if (!writer->file) return -1;
    writer->width = width;
    writer->height = height;
    if (gif_put_bytes(writer, "GIF89a", 6) ||
        gif_put_le16(writer, width) || gif_put_le16(writer, height) ||
        gif_put_byte(writer, 0x70) || gif_put_byte(writer, 0) ||
        gif_put_byte(writer, 0) ||
        gif_put_bytes(writer, loop_extension, sizeof loop_extension)) {
        emu_gif_close(writer);
        return -1;
    }
    return 0;
}

typedef struct {
    emu_gif_writer_t *writer;
    uint8_t block[255];
    unsigned block_size;
    uint32_t bits;
    unsigned bit_count;
    unsigned code_bits;
} gif_image_data_t;

static int flush_block(gif_image_data_t *data) {
    if (!data->block_size) return 0;
    if (gif_put_byte(data->writer, (uint8_t)data->block_size) ||
        gif_put_bytes(data->writer, data->block, data->block_size)) return -1;
    data->block_size = 0;
    return 0;
}

static int emit_data_byte(gif_image_data_t *data, uint8_t value) {
    data->block[data->block_size++] = value;
    return data->block_size == sizeof data->block ? flush_block(data) : 0;
}

static int emit_code(gif_image_data_t *data, unsigned code) {
    data->bits |= (uint32_t)code << data->bit_count;
    data->bit_count += data->code_bits;
    while (data->bit_count >= 8) {
        if (emit_data_byte(data, (uint8_t)data->bits)) return -1;
        data->bits >>= 8;
        data->bit_count -= 8;
    }
    return 0;
}

static int finish_image_data(gif_image_data_t *data) {
    if (data->bit_count && emit_data_byte(data, (uint8_t)data->bits)) return -1;
    return flush_block(data) == 0 ? gif_put_byte(data->writer, 0) : -1;
}

/* A local palette lets successive frames use different colors. Hash slots hold
 * palette indexes plus one, so an all-zero table represents an empty palette. */
static unsigned gif_color_slot(uint32_t color) {
    return (color * UINT32_C(2654435761)) >> (32u - 10u);
}

static uint32_t gif_color(const uint8_t *rgb) {
    return ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
}

static unsigned gif_lookup(uint32_t color, const uint32_t palette[256],
                           const uint16_t slots[1024]) {
    unsigned slot = gif_color_slot(color);
    while (slots[slot]) {
        unsigned index = slots[slot] - 1u;
        if (palette[index] == color) return index;
        slot = (slot + 1u) & 1023u;
    }
    return 256u;
}

static unsigned gif_rgb332(const uint8_t *rgb) {
    return (rgb[0] & 0xe0u) | ((rgb[1] & 0xe0u) >> 3u) |
           (rgb[2] >> 6u);
}

static unsigned gif_pair_slot(unsigned prefix, unsigned suffix) {
    return ((prefix * 257u + suffix) * UINT32_C(2654435761)) >> 19u;
}

static int gif_emit_pixels(gif_image_data_t *data, const uint8_t *indexes,
                           size_t pixels, unsigned min_bits) {
    uint32_t keys[8192] = {0};
    uint16_t values[8192] = {0};
    unsigned clear = 1u << min_bits;
    unsigned next = clear + 2u;
    data->code_bits = min_bits + 1u;
    if (emit_code(data, clear)) return -1;
    unsigned prefix = indexes[0];
    for (size_t i = 1; i < pixels; i++) {
        unsigned suffix = indexes[i];
        uint32_t key = ((uint32_t)prefix << 8u) | suffix;
        unsigned slot = gif_pair_slot(prefix, suffix);
        while (values[slot] && keys[slot] != key)
            slot = (slot + 1u) & 8191u;
        if (values[slot]) {
            prefix = values[slot];
            continue;
        }
        if (emit_code(data, prefix)) return -1;
        if (next < 4096u) {
            keys[slot] = key;
            values[slot] = (uint16_t)next++;
            /* GIF decoders add this entry on the following code, so the
             * encoder changes width one emitted code later. */
            if (next > (1u << data->code_bits) && data->code_bits < 12u)
                data->code_bits++;
        } else {
            if (emit_code(data, clear)) return -1;
            memset(values, 0, sizeof values);
            next = clear + 2u;
            data->code_bits = min_bits + 1u;
        }
        prefix = suffix;
    }
    return emit_code(data, prefix) || emit_code(data, clear + 1u)
         ? -1 : finish_image_data(data);
}

int emu_gif_write_rgb(emu_gif_writer_t *writer, const uint8_t *rgb,
                      unsigned delay_cs) {
    if (!writer || !writer->file || !rgb || writer->failed ||
        delay_cs > UINT16_MAX) return -1;
    size_t pixels = (size_t)writer->width * writer->height;
    if (pixels / writer->height != writer->width ||
        pixels > SIZE_MAX / 3u) return -1;
    uint8_t *indexes = malloc(pixels);
    if (!indexes) return -1;
    uint32_t palette[256] = {0};
    uint16_t slots[1024] = {0};
    unsigned colors = 0;
    int quantized = 0;
    for (size_t i = 0; i < pixels; i++) {
        uint32_t color = gif_color(rgb + i * 3u);
        unsigned index = gif_lookup(color, palette, slots);
        if (index == 256u) {
            if (colors == 256u) { quantized = 1; break; }
            index = colors++;
            palette[index] = color;
            unsigned slot = gif_color_slot(color);
            while (slots[slot]) slot = (slot + 1u) & 1023u;
            slots[slot] = (uint16_t)(index + 1u);
        }
        indexes[i] = (uint8_t)index;
    }
    if (quantized) {
        colors = 256u;
        for (unsigned i = 0; i < 256u; i++) {
            unsigned r = (i >> 5u) & 7u;
            unsigned g = (i >> 2u) & 7u;
            unsigned b = i & 3u;
            palette[i] = ((uint32_t)((r * 255u + 3u) / 7u) << 16u) |
                         ((uint32_t)((g * 255u + 3u) / 7u) << 8u) |
                         ((b * 255u + 1u) / 3u);
        }
        for (size_t i = 0; i < pixels; i++)
            indexes[i] = (uint8_t)gif_rgb332(rgb + i * 3u);
    }
    unsigned table_bits = 1u;
    while ((1u << table_bits) < colors) table_bits++;
    unsigned min_bits = table_bits < 2u ? 2u : table_bits;
    static const uint8_t gce_prefix[] = {0x21,0xf9,0x04,0x04};
    int failed = gif_put_bytes(writer, gce_prefix, sizeof gce_prefix) ||
        gif_put_le16(writer, delay_cs) || gif_put_byte(writer, 0) ||
        gif_put_byte(writer, 0) || gif_put_byte(writer, 0x2c) ||
        gif_put_le16(writer, 0) || gif_put_le16(writer, 0) ||
        gif_put_le16(writer, writer->width) ||
        gif_put_le16(writer, writer->height) ||
        gif_put_byte(writer, (uint8_t)(0x80u | (table_bits - 1u)));
    for (unsigned i = 0; i < (1u << table_bits) && !failed; i++) {
        uint8_t entry[3] = {(uint8_t)(palette[i] >> 16u),
                            (uint8_t)(palette[i] >> 8u),
                            (uint8_t)palette[i]};
        failed = gif_put_bytes(writer, entry, sizeof entry);
    }
    if (!failed) failed = gif_put_byte(writer, (uint8_t)min_bits);
    gif_image_data_t data = {.writer = writer};
    if (!failed) failed = gif_emit_pixels(&data, indexes, pixels, min_bits);
    free(indexes);
    if (failed) return -1;
    writer->frame_count++;
    return 0;
}

int emu_gif_close(emu_gif_writer_t *writer) {
    if (!writer || !writer->file) return -1;
    int ok = !writer->failed && gif_put_byte(writer, 0x3b) == 0;
    if (fclose(writer->file) != 0) ok = 0;
    writer->file = NULL;
    writer->failed = !ok;
    return ok ? 0 : -1;
}

int emu_image_capture_open(emu_image_capture_t *capture,
                           const char *strict_dir, const char *raw_dir,
                           int want_strict, int want_raw,
                           unsigned strict_width, unsigned strict_height) {
    if (!capture) return -1;
    memset(capture, 0, sizeof *capture);
    capture->want_strict = want_strict;
    capture->want_raw = want_raw;
    if (strict_dir)
        snprintf(capture->strict_dir, sizeof capture->strict_dir,
                 "%s", strict_dir);
    if (raw_dir)
        snprintf(capture->raw_dir, sizeof capture->raw_dir, "%s", raw_dir);
    if (want_strict) {
        char path[EMU_ARTIFACT_PATH_MAX];
        if (!strict_dir || path_join(path, sizeof path, strict_dir,
                                     "frames.gif") ||
            emu_gif_open(&capture->gif, path,
                         strict_width, strict_height)) {
            capture->failed = 1;
            return -1;
        }
        capture->gif_open = 1;
    }
    return 0;
}

int emu_image_capture_due(const emu_image_capture_t *capture,
                          emu_capture_kind_t kind, uint64_t sequence) {
    if (!capture || capture->failed) return 0;
    return kind == EMU_CAPTURE_STRICT
         ? capture->want_strict && sequence > capture->strict_written
         : capture->want_raw && sequence > capture->raw_written;
}

int emu_image_capture_write(emu_image_capture_t *capture,
                            emu_capture_kind_t kind,
                            uint64_t sequence, uint64_t icount,
                            const uint8_t *rgb,
                            unsigned width, unsigned height) {
    if (!capture || !rgb || capture->failed) return -1;
    uint64_t *written = kind == EMU_CAPTURE_STRICT
                      ? &capture->strict_written : &capture->raw_written;
    int wanted = kind == EMU_CAPTURE_STRICT
               ? capture->want_strict : capture->want_raw;
    const char *directory = kind == EMU_CAPTURE_STRICT
                          ? capture->strict_dir : capture->raw_dir;
    if (!wanted || sequence <= *written) return 0;
    char path[EMU_ARTIFACT_PATH_MAX];
    int count = snprintf(path, sizeof path,
                         "%s/frame-%04llu-icount-%012llu.png", directory,
                         (unsigned long long)sequence,
                         (unsigned long long)icount);
    int result = count < 0 || (size_t)count >= sizeof path
               ? -1 : emu_png_write_rgb(path, rgb, width, height);
    if (!result && kind == EMU_CAPTURE_STRICT && capture->gif_open)
        result = emu_gif_write_rgb(&capture->gif, rgb, 40u);
    *written = sequence;
    if (result) capture->failed = 1;
    return result;
}

int emu_image_capture_close(emu_image_capture_t *capture) {
    if (!capture) return -1;
    if (capture->gif_open) {
        if (emu_gif_close(&capture->gif)) capture->failed = 1;
        capture->gif_open = 0;
    }
    return capture->failed ? -1 : 0;
}
