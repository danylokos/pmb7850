#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <unistd.h>
#include "test_support.h"
#include "emu_qemu_snapshot.h"

static void write_bytes(const char *path, const void *bytes, size_t size) {
    FILE *f = fopen(path, "wb");
    EMU_CHECK(f && fwrite(bytes, 1, size, f) == size);
    EMU_CHECK(fclose(f) == 0);
}

int main(void) {
    char dir[] = "/tmp/emu-snapshot-test-XXXXXX", path[4096];
    EMU_CHECK(mkdtemp(dir));
    emu_prepared_session_t p;
    emu_test_prepared(&p);
    p.options.firmware_patches = 3;
    emu_qemu_image_t image = {.chip_count = 1};
    emu_qemu_chip_image_t *chip = &image.chips[0];
    strcpy(chip->model, p.chips[0].model);
    chip->size = p.source.size;
    chip->factory_uid_set = 1;
    memset(chip->factory_uid, 0x5a, sizeof chip->factory_uid);
    snprintf(chip->path, sizeof chip->path, "%s/input.bin", dir);
    write_bytes(chip->path, p.source.bytes, p.source.size);
    snprintf(path, sizeof path, "%s/vmstate.bin", dir);
    write_bytes(path, "native-stream", 13);
    emu_qemu_snapshot_t s = {.native_version = 1, .sim_stub = 1,
        .firmware_patches = 3, .icount = UINT64_C(9007199254740993),
        .ticks = 456, .pc = 0x123456, .flash_size = p.source.size};
    strcpy(s.flash, "/test/quoted\"backslash\\\nUnicode-📱.bin");
    strcpy(s.flash_sha256, p.source.sha256_hex);
    strcpy(s.device, p.selected_device);
    strcpy(s.qemu_version, "test build");
    emu_error_t error = {0};
    EMU_CHECK(emu_qemu_snapshot_commit(dir, &s, &image, &error) == EMU_OK);
    emu_qemu_snapshot_t loaded;
    EMU_CHECK(emu_qemu_snapshot_read(dir, &loaded, &error) == EMU_OK);
    EMU_CHECK(!strcmp(loaded.flash, s.flash) && loaded.icount == s.icount);
    EMU_CHECK(loaded.sim_stub && loaded.firmware_patches == 3);
    EMU_CHECK(emu_qemu_snapshot_validate(dir, &loaded, &p, &error) == EMU_OK);
    EMU_CHECK(emu_qemu_snapshot_commit(dir, &s, &image, &error) != EMU_OK);
    EMU_CHECK(emu_qemu_snapshot_validate(dir, &loaded, &p, &error) == EMU_OK);
    loaded.chips[0].size++;
    EMU_CHECK(emu_qemu_snapshot_validate(dir, &loaded, &p, &error) != EMU_OK);
    loaded.chips[0].size--;
    loaded.flash_sha256[0] ^= 1;
    EMU_CHECK(emu_qemu_snapshot_validate(dir, &loaded, &p, &error) != EMU_OK);
    loaded.flash_sha256[0] ^= 1;
    write_bytes(path, "bad", 3);
    EMU_CHECK(emu_qemu_snapshot_validate(dir, &loaded, &p, &error) != EMU_OK);
    write_bytes(path, "native-stream", 13);
    snprintf(path, sizeof path, "%s/snapshot.json", dir);
    FILE *f = fopen(path, "rb");
    char json[8192];
    size_t length = fread(json, 1, sizeof json, f);
    EMU_CHECK(fclose(f) == 0 && length < sizeof json);
    /* Every truncation must fail, including inside escaped strings. */
    for (size_t i = 0; i < length - 2; i++) {
        write_bytes(path, json, i);
        EMU_CHECK(emu_qemu_snapshot_read(dir, &loaded, &error) != EMU_OK);
    }
    const char *bad[] = {"{\"schema\":", "[", "\"\\u", "true", "{}",
        "{\"x\":18446744073709551616}", "{\"x\":\"\\ud800\"}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        write_bytes(path, bad[i], strlen(bad[i]));
        EMU_CHECK(emu_qemu_snapshot_read(dir, &loaded, &error) != EMU_OK);
    }
    unlink(path);
    const char *files[] = {"vmstate.bin", "flash-0.bin", "input.bin"};
    for (size_t i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        EMU_CHECK(unlink(path) == 0);
    }
    EMU_CHECK(rmdir(dir) == 0);
    emu_prepared_free(&p);
    puts("QEMU snapshot manifest tests passed");
    return 0;
}
