#ifndef EMU_QEMU_SNAPSHOT_H
#define EMU_QEMU_SNAPSHOT_H

#include "emu_qemu_image.h"
#include "emu_patch.h"

#define EMU_QEMU_SNAPSHOT_VERSION 1u
#define EMU_QEMU_SNAPSHOT_NATIVE_VERSION 1u

typedef struct {
    char flash[512];
    char flash_sha256[EMU_SHA256_HEX_SIZE];
    uint64_t flash_size;
    char device[32];
    char qemu_version[128];
    uint64_t native_version;
    emu_patch_set_t firmware_patches;
    int sim_stub;
    uint64_t icount, ticks;
    uint32_t pc;
    uint64_t vmstate_size;
    char vmstate_sha256[EMU_SHA256_HEX_SIZE];
    size_t chip_count;
    emu_qemu_chip_image_t chips[EMU_MAX_CHIPS];
    char chip_sha256[EMU_MAX_CHIPS][EMU_SHA256_HEX_SIZE];
} emu_qemu_snapshot_t;

/* Metadata is bounded and parsed before image preparation or child creation. */
emu_error_code_t emu_qemu_snapshot_read(const char *directory,
    emu_qemu_snapshot_t *snapshot, emu_error_t *error);
emu_error_code_t emu_qemu_snapshot_validate(const char *directory,
    const emu_qemu_snapshot_t *snapshot, const emu_prepared_session_t *prepared,
    emu_error_t *error);
emu_error_code_t emu_qemu_snapshot_materialize(const char *directory,
    const emu_qemu_snapshot_t *snapshot, emu_qemu_image_t *image,
    emu_error_t *error);
/* Caller has completed migration to directory/vmstate.bin. Publish last. */
emu_error_code_t emu_qemu_snapshot_commit(const char *directory,
    emu_qemu_snapshot_t *snapshot, const emu_qemu_image_t *image,
    emu_error_t *error);
int emu_qemu_snapshot_path(char *out, size_t size, const char *directory,
                            const char *file);

#endif
