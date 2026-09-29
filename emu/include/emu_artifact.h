#ifndef EMU_ARTIFACT_H
#define EMU_ARTIFACT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define EMU_ARTIFACT_PATH_MAX 720u
#define EMU_ARTIFACT_REGISTRY_MAX 32u

typedef struct {
    FILE *file;
    unsigned width;
    unsigned height;
    uint64_t frame_count;
    int failed;
} emu_gif_writer_t;

typedef struct {
    char kind[32];
    char path[EMU_ARTIFACT_PATH_MAX];
} emu_artifact_registration_t;

typedef struct {
    char root[EMU_ARTIFACT_PATH_MAX];
    char path[EMU_ARTIFACT_PATH_MAX];
    char device[32];
    char mmdd[8];
    int explicit_label;
    int finalized;
    emu_artifact_registration_t registrations[EMU_ARTIFACT_REGISTRY_MAX];
    size_t registration_count;
} emu_artifact_run_t;

typedef enum {
    EMU_CAPTURE_STRICT = 0,
    EMU_CAPTURE_RAW_DDRAM,
} emu_capture_kind_t;

typedef struct {
    char strict_dir[EMU_ARTIFACT_PATH_MAX];
    char raw_dir[EMU_ARTIFACT_PATH_MAX];
    int want_strict;
    int want_raw;
    uint64_t strict_written;
    uint64_t raw_written;
    emu_gif_writer_t gif;
    int gif_open;
    int failed;
} emu_image_capture_t;

int emu_png_write_rgb(const char *, const uint8_t *, unsigned, unsigned);
int emu_gif_open(emu_gif_writer_t *, const char *, unsigned, unsigned);
int emu_gif_write_rgb(emu_gif_writer_t *, const uint8_t *, unsigned);
int emu_gif_close(emu_gif_writer_t *);

void emu_artifact_today_mmdd(char *, size_t);
int emu_artifact_run_open(emu_artifact_run_t *, const char *, const char *,
                          const char *, const char *);
int emu_artifact_run_subdir(emu_artifact_run_t *, const char *,
                            char *, size_t);
int emu_artifact_run_path(emu_artifact_run_t *, const char *, const char *,
                          char *, size_t);
int emu_artifact_run_finalize(emu_artifact_run_t *, uint64_t);

int emu_image_capture_open(emu_image_capture_t *, const char *, const char *,
                           int, int, unsigned, unsigned);
int emu_image_capture_due(const emu_image_capture_t *, emu_capture_kind_t,
                          uint64_t);
int emu_image_capture_write(emu_image_capture_t *, emu_capture_kind_t,
                            uint64_t, uint64_t, const uint8_t *,
                            unsigned, unsigned);
int emu_image_capture_close(emu_image_capture_t *);

#endif
