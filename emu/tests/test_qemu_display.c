#include <pixman.h>
#include <stdio.h>
#include <string.h>

#include "test_support.h"
#include "emu_qemu_display.h"

typedef struct {
    unsigned calls;
    unsigned width;
    unsigned height;
    uint8_t first[3];
    uint8_t last[3];
} capture_t;

static void capture_frame(void *opaque, const emu_frame_t *frame) {
    capture_t *capture = opaque;
    EMU_CHECK(frame->kind == EMU_FRAME_DISPLAY);
    EMU_CHECK(frame->width == capture->width &&
              frame->height == capture->height);
    EMU_CHECK(frame->rgb_size ==
              (size_t)capture->width * capture->height * 3u);
    memcpy(capture->first, frame->rgb, 3);
    memcpy(capture->last, frame->rgb + frame->rgb_size - 3u, 3);
    capture->calls++;
}

static void put_pixel(uint8_t *bytes, uint32_t value) {
    memcpy(bytes, &value, sizeof value);
}

static void test_scanout_and_update(void) {
    emu_qemu_display_t display;
    capture_t capture = {.width = 101, .height = 64};
    emu_error_t error = {0};
    uint8_t pixels[101u * 64u * 4u];
    memset(pixels, 0, sizeof pixels);
    put_pixel(pixels, 0x00112233u);
    put_pixel(pixels + sizeof pixels - 4u, 0x00aabbccu);
    emu_qemu_display_init(&display, capture_frame, &capture, 101, 64);
    EMU_CHECK(emu_qemu_display_scanout(
        &display, 101, 64, 101 * 4, PIXMAN_x8r8g8b8,
        pixels, sizeof pixels, &error) == EMU_OK);
    EMU_CHECK(capture.calls == 1);
    EMU_CHECK(emu_qemu_display_scanout(
        &display, 101, 64, 101 * 4, PIXMAN_x8r8g8b8,
        pixels, sizeof pixels, &error) == EMU_OK);
    EMU_CHECK(capture.calls == 2);
    EMU_CHECK(capture.first[0] == 0x11 && capture.first[1] == 0x22 &&
              capture.first[2] == 0x33);
    EMU_CHECK(capture.last[0] == 0xaa && capture.last[1] == 0xbb &&
              capture.last[2] == 0xcc);
    uint8_t update[8];
    put_pixel(update, 0x00fedcbau);
    put_pixel(update + 4, 0x00010203u);
    EMU_CHECK(emu_qemu_display_update(
        &display, 0, 0, 2, 1, 8, PIXMAN_a8r8g8b8,
        update, sizeof update, &error) == EMU_OK);
    EMU_CHECK(capture.calls == 3);
    EMU_CHECK(capture.first[0] == 0xfe && capture.first[1] == 0xdc &&
              capture.first[2] == 0xba);
    emu_qemu_display_destroy(&display);

    capture = (capture_t){.width = 130, .height = 130};
    uint8_t color[130u * 130u * 4u] = {0};
    put_pixel(color, 0x0055aaeeu);
    emu_qemu_display_init(&display, capture_frame, &capture, 130, 130);
    EMU_CHECK(emu_qemu_display_scanout(
        &display, 130, 130, 130 * 4, PIXMAN_x8r8g8b8,
        color, sizeof color, &error) == EMU_OK);
    EMU_CHECK(capture.calls == 1);
    emu_qemu_display_destroy(&display);
}

static void test_rejections(void) {
    emu_qemu_display_t display;
    emu_error_t error = {0};
    uint8_t pixels[16] = {0};
    emu_qemu_display_init(&display, NULL, NULL, 101, 64);
    EMU_CHECK(emu_qemu_display_scanout(
        &display, 4, 1, 16, PIXMAN_x8r8g8b8,
        pixels, sizeof pixels, &error) == EMU_ERR_UNSUPPORTED);
    EMU_CHECK(emu_qemu_display_update(
        &display, 0, 0, 1, 1, 4, PIXMAN_x8r8g8b8,
        pixels, 4, &error) == EMU_ERR_LIFECYCLE);
    emu_qemu_display_destroy(&display);
}

int main(void) {
    test_scanout_and_update();
    test_rejections();
    puts("QEMU D-Bus display conversion: PASS");
    return 0;
}
