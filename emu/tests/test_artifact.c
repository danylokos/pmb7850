#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_support.h"
#include "emu_artifact.h"

static int is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void join_path(char *out, size_t cap,
                      const char *left, const char *right) {
    size_t left_size = strlen(left);
    size_t right_size = strlen(right);
    EMU_CHECK(left_size + 1u + right_size + 1u <= cap);
    memcpy(out, left, left_size);
    out[left_size] = '/';
    memcpy(out + left_size + 1u, right, right_size + 1u);
}

typedef struct {
    uint8_t bytes[131072];
    size_t size;
    size_t pos;
    unsigned loop_count;
    unsigned frames;
} gif_reader_t;

static unsigned gif_byte(gif_reader_t *reader) {
    EMU_CHECK(reader->pos < reader->size);
    return reader->bytes[reader->pos++];
}

static unsigned gif_word(gif_reader_t *reader) {
    unsigned lo = gif_byte(reader);
    return lo | (gif_byte(reader) << 8u);
}

static void gif_blocks(gif_reader_t *reader, uint8_t *out, size_t *size) {
    *size = 0;
    for (;;) {
        unsigned length = gif_byte(reader);
        if (!length) return;
        EMU_CHECK(reader->pos + length <= reader->size);
        if (out) {
            EMU_CHECK(*size + length <= 131072u);
            memcpy(out + *size, reader->bytes + reader->pos, length);
        }
        *size += length;
        reader->pos += length;
    }
}

static unsigned gif_code(const uint8_t *data, size_t size,
                         size_t *bit, unsigned width) {
    EMU_CHECK(*bit + width <= size * 8u);
    unsigned code = 0;
    for (unsigned i = 0; i < width; i++, (*bit)++)
        code |= ((data[*bit / 8u] >> (*bit % 8u)) & 1u) << i;
    return code;
}

static void gif_decode(const uint8_t *data, size_t size, unsigned min_bits,
                       uint8_t *pixels, size_t count) {
    uint16_t prefixes[4096] = {0};
    uint8_t suffixes[4096] = {0};
    uint8_t stack[4096];
    unsigned clear = 1u << min_bits;
    unsigned next = clear + 2u;
    unsigned width = min_bits + 1u;
    unsigned previous = 4096u;
    size_t bit = 0, written = 0;
    for (;;) {
        unsigned code = gif_code(data, size, &bit, width);
        if (code == clear) {
            next = clear + 2u;
            width = min_bits + 1u;
            previous = 4096u;
            continue;
        }
        if (code == clear + 1u) break;
        EMU_CHECK(code <= next && code < 4096u);
        unsigned cursor = code < next ? code : previous;
        size_t depth = 0;
        while (cursor >= clear) {
            EMU_CHECK(cursor < next && depth < sizeof stack);
            stack[depth++] = suffixes[cursor];
            cursor = prefixes[cursor];
        }
        EMU_CHECK(cursor < clear && depth < sizeof stack);
        stack[depth++] = (uint8_t)cursor;
        if (code == next) {
            EMU_CHECK(previous < 4096u && depth < sizeof stack);
            memmove(stack + 1u, stack, depth);
            stack[0] = stack[depth];
            depth++;
        }
        unsigned first = stack[depth - 1u];
        EMU_CHECK(written + depth <= count);
        while (depth) pixels[written++] = stack[--depth];
        if (previous < 4096u && next < 4096u) {
            prefixes[next] = (uint16_t)previous;
            suffixes[next++] = (uint8_t)first;
            if (next == (1u << width) && width < 12u) width++;
        }
        previous = code;
    }
    EMU_CHECK(written == count);
}

static void check_gif(const char *path, unsigned expected_width,
                      unsigned expected_height, const uint8_t *expected,
                      unsigned expected_frames) {
    FILE *file = fopen(path, "rb");
    EMU_CHECK(file != NULL);
    gif_reader_t *reader = calloc(1, sizeof *reader);
    EMU_CHECK(reader != NULL);
    reader->size = fread(reader->bytes, 1, sizeof reader->bytes, file);
    EMU_CHECK(!ferror(file) && feof(file));
    EMU_CHECK(fclose(file) == 0);
    EMU_CHECK(reader->size >= 13u &&
              memcmp(reader->bytes, "GIF89a", 6u) == 0);
    reader->pos = 6u;
    EMU_CHECK(gif_word(reader) == expected_width);
    EMU_CHECK(gif_word(reader) == expected_height);
    unsigned packed = gif_byte(reader);
    gif_byte(reader);
    gif_byte(reader);
    EMU_CHECK(!(packed & 0x80u));
    unsigned delay = 0;
    size_t pixels = (size_t)expected_width * expected_height;
    uint8_t *decoded = malloc(pixels);
    uint8_t *compressed = malloc(131072u);
    EMU_CHECK(decoded && compressed);
    while (reader->pos < reader->size) {
        unsigned marker = gif_byte(reader);
        if (marker == 0x3bu) break;
        if (marker == 0x21u) {
            unsigned label = gif_byte(reader);
            if (label == 0xf9u) {
                EMU_CHECK(gif_byte(reader) == 4u);
                gif_byte(reader);
                delay = gif_word(reader);
                gif_byte(reader);
                EMU_CHECK(gif_byte(reader) == 0u);
            } else if (label == 0xffu) {
                EMU_CHECK(gif_byte(reader) == 11u);
                EMU_CHECK(memcmp(reader->bytes + reader->pos,
                                 "NETSCAPE2.0", 11u) == 0);
                reader->pos += 11u;
                EMU_CHECK(gif_byte(reader) == 3u);
                EMU_CHECK(gif_byte(reader) == 1u);
                reader->loop_count = gif_word(reader);
                EMU_CHECK(gif_byte(reader) == 0u);
            } else {
                size_t ignored;
                gif_blocks(reader, NULL, &ignored);
            }
            continue;
        }
        EMU_CHECK(marker == 0x2cu && reader->frames < expected_frames);
        EMU_CHECK(gif_word(reader) == 0u && gif_word(reader) == 0u);
        EMU_CHECK(gif_word(reader) == expected_width);
        EMU_CHECK(gif_word(reader) == expected_height);
        packed = gif_byte(reader);
        EMU_CHECK(packed & 0x80u);
        unsigned colors = 1u << ((packed & 7u) + 1u);
        uint8_t palette[256 * 3];
        for (unsigned i = 0; i < colors * 3u; i++)
            palette[i] = (uint8_t)gif_byte(reader);
        unsigned min_bits = gif_byte(reader);
        size_t compressed_size;
        gif_blocks(reader, compressed, &compressed_size);
        gif_decode(compressed, compressed_size, min_bits, decoded, pixels);
        EMU_CHECK(delay == 40u);
        for (size_t i = 0; i < pixels; i++) {
            EMU_CHECK(decoded[i] < colors);
            EMU_CHECK(memcmp(palette + decoded[i] * 3u,
                             expected + (reader->frames * pixels + i) * 3u,
                             3u) == 0);
        }
        reader->frames++;
    }
    EMU_CHECK(reader->loop_count == 0u &&
              reader->frames == expected_frames &&
              reader->pos == reader->size);
    free(compressed);
    free(decoded);
    free(reader);
}

static void test_lifecycle(const char *root) {
    emu_artifact_run_t run;
    EMU_CHECK(emu_artifact_run_open(
                  &run, root, "c55", "named", "0102") == 0);
    EMU_CHECK(run.explicit_label && !run.finalized);
    char directory[EMU_ARTIFACT_PATH_MAX];
    char path[EMU_ARTIFACT_PATH_MAX];
    EMU_CHECK(emu_artifact_run_subdir(
                  &run, "trace", directory, sizeof directory) == 0);
    EMU_CHECK(emu_artifact_run_path(
                  &run, "trace", "trace/trace.parquet",
                  path, sizeof path) == 0);
    EMU_CHECK(run.registration_count == 1);
    EMU_CHECK(!strcmp(run.registrations[0].kind, "trace"));
    EMU_CHECK(!strcmp(run.registrations[0].path, "trace/trace.parquet"));
    EMU_CHECK(emu_artifact_run_finalize(&run, 42) == 0);
    EMU_CHECK(run.finalized && strstr(run.path, "/named") != NULL);
    EMU_CHECK(emu_artifact_run_finalize(&run, 43) == -1);

    emu_artifact_run_t automatic;
    EMU_CHECK(emu_artifact_run_open(
                  &automatic, root, "c55", NULL, "0102") == 0);
    EMU_CHECK(strstr(automatic.path, "/.c55-0102.partial") != NULL);
    EMU_CHECK(emu_artifact_run_finalize(&automatic, 5900000) == 0);
    EMU_CHECK(strstr(automatic.path, "/c55-icount5m9-0102") != NULL);

    emu_artifact_run_t collision;
    EMU_CHECK(emu_artifact_run_open(
                  &collision, root, "c55", NULL, "0103") == 0);
    join_path(path, sizeof path, root, "c55-icount1k-0103");
    EMU_CHECK(mkdir(path, 0777) == 0);
    EMU_CHECK(emu_artifact_run_finalize(&collision, 1000) == -1);
    EMU_CHECK(strstr(collision.path, ".partial") != NULL);
    EMU_CHECK(rmdir(path) == 0);
    EMU_CHECK(rmdir(collision.path) == 0);
}

static void test_images(const char *root) {
    char strict[EMU_ARTIFACT_PATH_MAX];
    char raw[EMU_ARTIFACT_PATH_MAX];
    join_path(strict, sizeof strict, root, "images");
    join_path(raw, sizeof raw, root, "raw");
    EMU_CHECK(mkdir(strict, 0777) == 0);
    EMU_CHECK(mkdir(raw, 0777) == 0);
    static const uint8_t first[] = {
        202,214,184, 30,39,32,
        30,39,32, 202,214,184,
    };
    static const uint8_t second[] = {
        30,39,32, 30,39,32,
        30,39,32, 30,39,32,
    };
    static const uint8_t color[] = {
        255,0,0, 0,255,0,
        0,0,255, 255,255,0,
    };
    emu_image_capture_t capture;
    EMU_CHECK(emu_image_capture_open(
                  &capture, strict, raw, 1, 1, 2, 2) == 0);
    EMU_CHECK(emu_image_capture_due(&capture, EMU_CAPTURE_STRICT, 1));
    EMU_CHECK(emu_image_capture_write(
                  &capture, EMU_CAPTURE_STRICT, 1, 7,
                  first, 2, 2) == 0);
    EMU_CHECK(!emu_image_capture_due(&capture, EMU_CAPTURE_STRICT, 1));
    EMU_CHECK(emu_image_capture_write(
                  &capture, EMU_CAPTURE_STRICT, 1, 8,
                  second, 2, 2) == 0);
    EMU_CHECK(emu_image_capture_write(
                  &capture, EMU_CAPTURE_STRICT, 2, 9,
                  second, 2, 2) == 0);
    EMU_CHECK(emu_image_capture_write(
                  &capture, EMU_CAPTURE_STRICT, 3, 11,
                  second, 2, 2) == 0);
    EMU_CHECK(emu_image_capture_write(
                  &capture, EMU_CAPTURE_STRICT, 4, 12,
                  color, 2, 2) == 0);
    EMU_CHECK(emu_image_capture_write(
                  &capture, EMU_CAPTURE_RAW_DDRAM, 1, 10,
                  first, 2, 2) == 0);
    EMU_CHECK(capture.strict_written == 4 && capture.raw_written == 1);
    EMU_CHECK(capture.gif.frame_count == 4);
    EMU_CHECK(emu_image_capture_close(&capture) == 0);

    char strict_first[EMU_ARTIFACT_PATH_MAX];
    char strict_second[EMU_ARTIFACT_PATH_MAX];
    char strict_third[EMU_ARTIFACT_PATH_MAX];
    char strict_fourth[EMU_ARTIFACT_PATH_MAX];
    char raw_first[EMU_ARTIFACT_PATH_MAX];
    char gif[EMU_ARTIFACT_PATH_MAX];
    join_path(strict_first, sizeof strict_first, strict,
              "frame-0001-icount-000000000007.png");
    join_path(strict_second, sizeof strict_second, strict,
              "frame-0002-icount-000000000009.png");
    join_path(strict_third, sizeof strict_third, strict,
              "frame-0003-icount-000000000011.png");
    join_path(strict_fourth, sizeof strict_fourth, strict,
              "frame-0004-icount-000000000012.png");
    join_path(raw_first, sizeof raw_first, raw,
              "frame-0001-icount-000000000010.png");
    join_path(gif, sizeof gif, strict, "frames.gif");
    EMU_CHECK(is_file(strict_first) && is_file(strict_second) &&
              is_file(strict_third) && is_file(strict_fourth) &&
              is_file(raw_first) && is_file(gif));
    uint8_t expected[sizeof first + sizeof second * 2u + sizeof color];
    memcpy(expected, first, sizeof first);
    memcpy(expected + sizeof first, second, sizeof second);
    memcpy(expected + sizeof first + sizeof second, second, sizeof second);
    memcpy(expected + sizeof first + sizeof second * 2u, color, sizeof color);
    check_gif(gif, 2, 2, expected, 4);
    EMU_CHECK(unlink(strict_first) == 0);
    EMU_CHECK(unlink(strict_second) == 0);
    EMU_CHECK(unlink(strict_third) == 0);
    EMU_CHECK(unlink(strict_fourth) == 0);
    EMU_CHECK(unlink(raw_first) == 0);
    EMU_CHECK(unlink(gif) == 0);
    EMU_CHECK(rmdir(strict) == 0 && rmdir(raw) == 0);

    emu_image_capture_t failed;
    EMU_CHECK(emu_image_capture_open(
                  &failed, "/no/such/x55-artifact-directory", NULL,
                  1, 0, 2, 2) == -1);
    EMU_CHECK(failed.failed);
}

static void test_quantized_gif(const char *root) {
    char path[EMU_ARTIFACT_PATH_MAX];
    join_path(path, sizeof path, root, "quantized.gif");
    uint8_t rgb[300 * 3], expected[300 * 3];
    for (unsigned i = 0; i < 300u; i++) {
        rgb[i * 3u] = (uint8_t)i;
        rgb[i * 3u + 1u] = (uint8_t)(i >> 8u);
        rgb[i * 3u + 2u] = (uint8_t)(i * 37u);
        unsigned r = rgb[i * 3u] >> 5u;
        unsigned g = rgb[i * 3u + 1u] >> 5u;
        unsigned b = rgb[i * 3u + 2u] >> 6u;
        expected[i * 3u] = (uint8_t)((r * 255u + 3u) / 7u);
        expected[i * 3u + 1u] = (uint8_t)((g * 255u + 3u) / 7u);
        expected[i * 3u + 2u] = (uint8_t)((b * 255u + 1u) / 3u);
    }
    emu_gif_writer_t writer;
    EMU_CHECK(emu_gif_open(&writer, path, 300, 1) == 0);
    EMU_CHECK(emu_gif_write_rgb(&writer, rgb, 40u) == 0);
    EMU_CHECK(emu_gif_close(&writer) == 0);
    check_gif(path, 300, 1, expected, 1);
    EMU_CHECK(unlink(path) == 0);
}

static void test_dictionary_reset(const char *root) {
    char path[EMU_ARTIFACT_PATH_MAX];
    join_path(path, sizeof path, root, "large.gif");
    uint8_t rgb[100 * 100 * 3];
    uint32_t state = 1u;
    for (size_t i = 0; i < 100u * 100u; i++) {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        uint8_t index = (uint8_t)state;
        rgb[i * 3u] = index;
        rgb[i * 3u + 1u] = (uint8_t)(index * 37u);
        rgb[i * 3u + 2u] = (uint8_t)(index * 97u);
    }
    emu_gif_writer_t writer;
    EMU_CHECK(emu_gif_open(&writer, path, 100, 100) == 0);
    EMU_CHECK(emu_gif_write_rgb(&writer, rgb, 40u) == 0);
    EMU_CHECK(emu_gif_close(&writer) == 0);
    check_gif(path, 100, 100, rgb, 1);
    EMU_CHECK(unlink(path) == 0);
}

int main(void) {
    char root[] = "/tmp/x55-artifact-XXXXXX";
    EMU_CHECK(mkdtemp(root) != NULL);
    test_lifecycle(root);
    test_images(root);
    test_quantized_gif(root);
    test_dictionary_reset(root);

    char path[EMU_ARTIFACT_PATH_MAX];
    join_path(path, sizeof path, root, "named/trace");
    EMU_CHECK(rmdir(path) == 0);
    join_path(path, sizeof path, root, "named");
    EMU_CHECK(rmdir(path) == 0);
    join_path(path, sizeof path, root, "c55-icount5m9-0102");
    EMU_CHECK(rmdir(path) == 0);
    EMU_CHECK(rmdir(root) == 0);
    puts("host artifact lifecycle and image writers: PASS");
    return 0;
}
