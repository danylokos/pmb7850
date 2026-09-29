#define _POSIX_C_SOURCE 200809L

#include "emu_qemu_image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emu_product.h"

typedef struct {
    uint8_t *main_array;
    uint8_t factory_uid[EMU_QEMU_FACTORY_UID_SIZE];
    int factory_uid_set;
    uint8_t am29_secsi[EMU_QEMU_AM29_SECSI_SIZE];
    int am29_secsi_set;
} staged_chip_t;

static int fail_chip_create_after = -1;

void emu_qemu_test_fail_chip_create_after(int successful_files) {
    fail_chip_create_after = successful_files;
}

static emu_error_code_t image_fail(emu_error_t *error,
                                   emu_error_code_t code,
                                   const char *format, ...) {
    if (error) {
        va_list arguments;
        error->code = code;
        va_start(arguments, format);
        vsnprintf(error->message, sizeof error->message, format, arguments);
        va_end(arguments);
    }
    return code;
}

static void image_ok(emu_error_t *error) {
    if (error) {
        error->code = EMU_OK;
        error->message[0] = 0;
    }
}

static int write_all(int fd, const uint8_t *bytes, size_t size) {
    while (size) {
        ssize_t written = write(fd, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        bytes += (size_t)written;
        size -= (size_t)written;
    }
    return 0;
}

static int read_all_at(int fd, size_t offset, uint8_t *bytes, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t count = pread(fd, bytes + done, size - done,
                              (off_t)(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        done += (size_t)count;
    }
    return 0;
}

static int model_has_factory_uid(const char *model) {
    return model && (!strcmp(model, "m58lw064d") ||
                     !strcmp(model, "w30-64mbit-top") ||
                     !strcmp(model, "w30-128mbit-top"));
}

static int model_has_am29_secsi(const char *model) {
    return model && (!strcmp(model, "am29lv640mh") ||
                     !strcmp(model, "am29lv128mh"));
}

static emu_error_code_t validate_topology(
        const emu_prepared_session_t *prepared, emu_error_t *error) {
    if (!prepared || !prepared->source.bytes || !prepared->source.size)
        return image_fail(error, EMU_ERR_ARGUMENT,
                          "missing prepared QEMU image");
    emu_error_code_t code = emu_prepared_validate(
        prepared, EMU_CAP_STORAGE_INIT, error);
    if (code != EMU_OK) return code;
    code = emu_product_validate_prepared_storage(prepared, error);
    if (code != EMU_OK) return code;
    for (size_t i = 0; i < prepared->operation_count; i++) {
        const emu_storage_operation_t *operation = &prepared->operations[i];
        if (operation->stage == EMU_STORAGE_STAGE_POST_RESTORE) {
            if (prepared->options.snapshot_path ||
                prepared->options.defer_post_restore_storage)
                return image_fail(error, EMU_ERR_UNSUPPORTED,
                                  "QEMU snapshot resume cannot apply post-restore operations");
            if (operation->space == EMU_STORAGE_MAIN_ARRAY) continue;
            return image_fail(error, EMU_ERR_UNSUPPORTED,
                              "QEMU post-restore operations require the main array");
        }
        if (operation->space == EMU_STORAGE_MAIN_ARRAY) continue;
        if (operation->chip_index != prepared->identity_chip_index)
            return image_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                              "QEMU identity operation targets the wrong owner chip");
        const char *model = prepared->chips[operation->chip_index].model;
        if (operation->space == EMU_STORAGE_FACTORY_UID) {
            if (!model_has_factory_uid(model) || operation->offset ||
                (operation->expected_size &&
                 operation->expected_size != EMU_QEMU_FACTORY_UID_SIZE) ||
                operation->replacement_size != EMU_QEMU_FACTORY_UID_SIZE)
                return image_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                  "invalid factory UID operation for model %s",
                                  model);
        } else if (operation->space == EMU_STORAGE_AM29_FACTORY_SECSI ||
                   operation->space == EMU_STORAGE_AM29_CUSTOMER_SECSI) {
            size_t operation_size =
                operation->expected_size > operation->replacement_size
                ? operation->expected_size : operation->replacement_size;
            if (!model_has_am29_secsi(model) ||
                operation->offset > EMU_QEMU_AM29_SECSI_SIZE ||
                operation_size >
                    EMU_QEMU_AM29_SECSI_SIZE - operation->offset)
                return image_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                  "invalid AM29 SecSi operation for model %s",
                                  model);
        } else {
            return image_fail(error, EMU_ERR_UNSUPPORTED,
                              "QEMU does not support storage space %s",
                              emu_storage_space_name(operation->space));
        }
    }
    return EMU_OK;
}

static const uint8_t *operation_current(
        const emu_storage_operation_t *operation,
        staged_chip_t staged[EMU_MAX_CHIPS], size_t *size, int *present) {
    staged_chip_t *chip = &staged[operation->chip_index];
    *present = 1;
    if (operation->space == EMU_STORAGE_MAIN_ARRAY) {
        *size = SIZE_MAX;
        return chip->main_array + operation->offset;
    }
    if (operation->space == EMU_STORAGE_FACTORY_UID) {
        *size = EMU_QEMU_FACTORY_UID_SIZE;
        *present = chip->factory_uid_set;
        return chip->factory_uid;
    }
    *size = EMU_QEMU_AM29_SECSI_SIZE - operation->offset;
    return chip->am29_secsi + operation->offset;
}

static int operation_matches(
        const emu_storage_operation_t *operation,
        staged_chip_t staged[EMU_MAX_CHIPS], const uint8_t *bytes,
        size_t bytes_size) {
    size_t available = 0;
    int present = 0;
    const uint8_t *current = operation_current(
        operation, staged, &available, &present);
    return present && bytes && bytes_size && bytes_size <= available &&
           !memcmp(current, bytes, bytes_size);
}

static int same_storage_object(const emu_storage_operation_t *a,
                               const emu_storage_operation_t *b) {
    return a->chip_index == b->chip_index && a->space == b->space;
}

static int replacements_conflict(const emu_storage_operation_t *a,
                                 const emu_storage_operation_t *b) {
    if (!same_storage_object(a, b)) return 0;
    size_t a_end = a->offset + a->replacement_size;
    size_t b_end = b->offset + b->replacement_size;
    size_t start = a->offset > b->offset ? a->offset : b->offset;
    size_t end = a_end < b_end ? a_end : b_end;
    if (start >= end) return 0;
    return memcmp(a->replacement + start - a->offset,
                  b->replacement + start - b->offset, end - start) != 0;
}

static void apply_operation(const emu_storage_operation_t *operation,
                            staged_chip_t staged[EMU_MAX_CHIPS]) {
    staged_chip_t *chip = &staged[operation->chip_index];
    if (operation->space == EMU_STORAGE_MAIN_ARRAY) {
        memcpy(chip->main_array + operation->offset,
               operation->replacement, operation->replacement_size);
    } else if (operation->space == EMU_STORAGE_FACTORY_UID) {
        memcpy(chip->factory_uid, operation->replacement,
               EMU_QEMU_FACTORY_UID_SIZE);
        chip->factory_uid_set = 1;
    } else {
        memcpy(chip->am29_secsi + operation->offset,
               operation->replacement, operation->replacement_size);
        chip->am29_secsi_set = 1;
    }
}

static emu_error_code_t apply_operations(
        const emu_prepared_session_t *prepared,
        staged_chip_t staged[EMU_MAX_CHIPS], uint64_t *already_applied,
        emu_error_t *error) {
    for (size_t first = 0; first < prepared->operation_count;) {
        size_t last = first + 1u;
        while (last < prepared->operation_count &&
               prepared->operations[last].group ==
                   prepared->operations[first].group)
            last++;
        int pending = 0;
        int applied = 0;
        for (size_t i = first; i < last; i++) {
            const emu_storage_operation_t *operation =
                &prepared->operations[i];
            for (size_t j = first; j < i; j++)
                if (replacements_conflict(operation,
                                          &prepared->operations[j]))
                    return image_fail(
                        error, EMU_ERR_INVALID_PREPARED_SESSION,
                        "QEMU storage group %zu has conflicting replacements",
                        operation->group);
            int replaced = operation_matches(
                operation, staged, operation->replacement,
                operation->replacement_size);
            int expected = operation->expected_size
                ? operation_matches(operation, staged, operation->expected,
                                    operation->expected_size)
                : !replaced;
            if (!expected && !replaced)
                return image_fail(
                    error, EMU_ERR_INVALID_PREPARED_SESSION,
                    "QEMU storage group %zu conflicts on chip %zu at 0x%zx",
                    operation->group, operation->chip_index,
                    operation->offset);
            if (expected && replaced) continue;
            pending |= expected;
            applied |= replaced;
        }
        if (pending && applied)
            return image_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                              "QEMU storage group %zu is partially applied",
                              prepared->operations[first].group);
        if (pending)
            for (size_t i = first; i < last; i++)
                apply_operation(&prepared->operations[i], staged);
        else
            for (size_t i = first; i < last; i++)
                *already_applied |= UINT64_C(1) << i;
        first = last;
    }
    return EMU_OK;
}

static void free_staged(staged_chip_t staged[EMU_MAX_CHIPS],
                        size_t chip_count) {
    for (size_t i = 0; i < chip_count; i++) free(staged[i].main_array);
}

void emu_qemu_image_destroy(emu_qemu_image_t *image) {
    if (!image) return;
    for (size_t i = 0; i < EMU_MAX_CHIPS; i++)
        if (image->chips[i].path[0]) unlink(image->chips[i].path);
    if (image->directory[0]) rmdir(image->directory);
    memset(image, 0, sizeof *image);
}

emu_error_code_t emu_qemu_image_materialize(
        const emu_prepared_session_t *prepared, emu_qemu_image_t *image,
        emu_error_t *error) {
    if (!image || image->directory[0] || image->chip_count)
        return image_fail(error, EMU_ERR_ARGUMENT,
                          "invalid QEMU image output");
    emu_error_code_t code = validate_topology(prepared, error);
    if (code != EMU_OK) return code;

    staged_chip_t staged[EMU_MAX_CHIPS] = {{0}};
    for (size_t i = 0; i < prepared->chip_count; i++) {
        staged[i].main_array = malloc(prepared->chips[i].size);
        if (!staged[i].main_array) {
            free_staged(staged, prepared->chip_count);
            return image_fail(error, EMU_ERR_NOMEM,
                              "cannot allocate QEMU chip image");
        }
        memcpy(staged[i].main_array,
               prepared->source.bytes + prepared->chips[i].source_offset,
               prepared->chips[i].size);
        memset(staged[i].am29_secsi, 0xff,
               sizeof staged[i].am29_secsi);
    }
    code = apply_operations(prepared, staged,
                            &image->already_applied_operations, error);
    if (code != EMU_OK) {
        free_staged(staged, prepared->chip_count);
        image->already_applied_operations = 0;
        return code;
    }

    char template[] = "/tmp/emu-qemu-XXXXXX";
    char *directory = mkdtemp(template);
    if (!directory ||
        snprintf(image->directory, sizeof image->directory, "%s",
                 directory) >= (int)sizeof image->directory) {
        free_staged(staged, prepared->chip_count);
        memset(image, 0, sizeof *image);
        return image_fail(error, EMU_ERR_IO,
                          "cannot create private QEMU session directory");
    }
    snprintf(image->device, sizeof image->device, "%s",
             prepared->selected_device);
    image->chip_count = prepared->chip_count;
    image->combined_size = prepared->source.size;
    int created = 0;
    for (size_t i = 0; i < prepared->chip_count; i++) {
        emu_qemu_chip_image_t *chip = &image->chips[i];
        if (snprintf(chip->path, sizeof chip->path, "%s/flash-%zu.bin",
                     image->directory, i) >= (int)sizeof chip->path) {
            code = image_fail(error, EMU_ERR_IO,
                              "QEMU chip path is too long");
            break;
        }
        snprintf(chip->model, sizeof chip->model, "%s",
                 prepared->chips[i].model);
        chip->source_offset = prepared->chips[i].source_offset;
        chip->size = prepared->chips[i].size;
        memcpy(chip->factory_uid, staged[i].factory_uid,
               sizeof chip->factory_uid);
        chip->factory_uid_set = staged[i].factory_uid_set;
        memcpy(chip->am29_secsi, staged[i].am29_secsi,
               sizeof chip->am29_secsi);
        chip->am29_secsi_set = staged[i].am29_secsi_set;
        if (fail_chip_create_after >= 0 &&
            created >= fail_chip_create_after) {
            fail_chip_create_after = -1;
            code = image_fail(error, EMU_ERR_IO,
                              "injected QEMU chip creation failure");
            break;
        }
        int fd = open(chip->path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            code = image_fail(error, EMU_ERR_IO,
                              "cannot create QEMU chip %zu: %s", i,
                              strerror(errno));
            break;
        }
        int failed = write_all(fd, staged[i].main_array, chip->size) ||
                     fsync(fd);
        int saved = errno;
        if (close(fd) && !failed) {
            failed = 1;
            saved = errno;
        }
        if (failed) {
            code = image_fail(error, EMU_ERR_IO,
                              "cannot write QEMU chip %zu: %s", i,
                              strerror(saved));
            break;
        }
        created++;
    }
    fail_chip_create_after = -1;
    free_staged(staged, prepared->chip_count);
    if (code != EMU_OK) {
        emu_qemu_image_destroy(image);
        return code;
    }
    image_ok(error);
    return EMU_OK;
}

emu_error_code_t emu_qemu_image_read(
        const emu_qemu_image_t *image, size_t offset, uint8_t *bytes,
        size_t size, emu_error_t *error) {
    if (!image || !image->chip_count || (!bytes && size) ||
        offset > image->combined_size || size > image->combined_size - offset)
        return image_fail(error, EMU_ERR_ARGUMENT,
                          "invalid QEMU combined flash read");
    while (size) {
        const emu_qemu_chip_image_t *chip = NULL;
        for (size_t i = 0; i < image->chip_count; i++)
            if (offset >= image->chips[i].source_offset &&
                offset - image->chips[i].source_offset <
                    image->chips[i].size) {
                chip = &image->chips[i];
                break;
            }
        if (!chip)
            return image_fail(error, EMU_ERR_IO,
                              "QEMU chip files do not cover combined image");
        size_t chip_offset = offset - chip->source_offset;
        size_t chunk = chip->size - chip_offset;
        if (chunk > size) chunk = size;
        int fd = open(chip->path, O_RDONLY);
        if (fd < 0 || read_all_at(fd, chip_offset, bytes, chunk)) {
            int saved = errno;
            if (fd >= 0) close(fd);
            return image_fail(error, EMU_ERR_IO,
                              "cannot read QEMU chip image: %s",
                              strerror(saved));
        }
        if (close(fd))
            return image_fail(error, EMU_ERR_IO,
                              "cannot close QEMU chip image: %s",
                              strerror(errno));
        bytes += chunk;
        offset += chunk;
        size -= chunk;
    }
    image_ok(error);
    return EMU_OK;
}

emu_error_code_t emu_qemu_image_export(
        const emu_qemu_image_t *image, const char *path,
        emu_error_t *error) {
    if (!image || !image->chip_count || !path || !path[0])
        return image_fail(error, EMU_ERR_ARGUMENT,
                          "invalid QEMU flash export");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return image_fail(error, EMU_ERR_IO,
                          "cannot create QEMU flash export: %s",
                          strerror(errno));
    uint8_t buffer[64u * 1024u];
    size_t offset = 0;
    emu_error_code_t code = EMU_OK;
    while (offset < image->combined_size) {
        size_t chunk = image->combined_size - offset;
        if (chunk > sizeof buffer) chunk = sizeof buffer;
        code = emu_qemu_image_read(image, offset, buffer, chunk, error);
        if (code != EMU_OK || write_all(fd, buffer, chunk)) {
            if (code == EMU_OK)
                code = image_fail(error, EMU_ERR_IO,
                                  "cannot write QEMU flash export: %s",
                                  strerror(errno));
            break;
        }
        offset += chunk;
    }
    if (code == EMU_OK && fsync(fd))
        code = image_fail(error, EMU_ERR_IO,
                          "cannot sync QEMU flash export: %s",
                          strerror(errno));
    if (close(fd) && code == EMU_OK)
        code = image_fail(error, EMU_ERR_IO,
                          "cannot close QEMU flash export: %s",
                          strerror(errno));
    if (code != EMU_OK) return code;
    image_ok(error);
    return EMU_OK;
}
