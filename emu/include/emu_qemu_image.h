#ifndef EMU_QEMU_IMAGE_H
#define EMU_QEMU_IMAGE_H

#include "emu_engine.h"

#define EMU_QEMU_PATH_MAX 4096u
#define EMU_QEMU_FACTORY_UID_SIZE 8u
#define EMU_QEMU_AM29_SECSI_SIZE 256u

typedef struct {
    char path[EMU_QEMU_PATH_MAX];
    char model[32];
    size_t source_offset;
    size_t size;
    uint8_t factory_uid[EMU_QEMU_FACTORY_UID_SIZE];
    int factory_uid_set;
    uint8_t am29_secsi[EMU_QEMU_AM29_SECSI_SIZE];
    int am29_secsi_set;
} emu_qemu_chip_image_t;

typedef struct {
    char directory[EMU_QEMU_PATH_MAX];
    char device[32];
    emu_qemu_chip_image_t chips[EMU_MAX_CHIPS];
    size_t chip_count;
    size_t combined_size;
    uint64_t already_applied_operations;
} emu_qemu_image_t;

emu_error_code_t emu_qemu_image_materialize(
    const emu_prepared_session_t *prepared, emu_qemu_image_t *image,
    emu_error_t *error);
emu_error_code_t emu_qemu_image_read(
    const emu_qemu_image_t *image, size_t offset, uint8_t *bytes,
    size_t size, emu_error_t *error);
emu_error_code_t emu_qemu_image_export(
    const emu_qemu_image_t *image, const char *path, emu_error_t *error);
void emu_qemu_image_destroy(emu_qemu_image_t *image);

/* Deterministic partial-creation failure injection for lifecycle tests. */
void emu_qemu_test_fail_chip_create_after(int successful_files);

#endif
