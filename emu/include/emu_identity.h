#ifndef EMU_IDENTITY_H
#define EMU_IDENTITY_H

#include "emu_eeprom.h"
#include "emu_engine.h"

#define EMU_IDENTITY_BLOCK_COUNT 5u
#define EMU_IDENTITY_MAX_BLOCK_SIZE 232u

typedef struct {
    uint16_t id;
    uint16_t length;
    uint8_t bytes[EMU_IDENTITY_MAX_BLOCK_SIZE];
} emu_identity_block_t;

typedef struct {
    char imei[15];
    uint32_t fsn;
    emu_identity_block_t blocks[EMU_IDENTITY_BLOCK_COUNT];
} emu_identity_bundle_t;

typedef struct {
    char source[128];
    uint32_t fsn;
    char imei[15];
    size_t record_count;
    int fsn_only_fallback;
} emu_identity_plan_result_t;

const emu_identity_bundle_t *emu_identity_default_bundle(void);
emu_error_code_t emu_identity_bundle_parse(
    const char *document, size_t size, emu_identity_bundle_t *bundle,
    emu_error_t *error);
emu_error_code_t emu_identity_bundle_load(
    const char *path, emu_identity_bundle_t *bundle, emu_error_t *error);

uint32_t emu_identity_fsn_from_factory_uid(
    const uint8_t uid[8], uint16_t manufacturer, uint16_t device);
void emu_identity_factory_uid_from_fsn(
    uint32_t fsn, uint16_t manufacturer, uint16_t device, uint8_t uid[8]);
uint32_t emu_identity_fsn_from_am29_secsi(
    const uint8_t secsi[8], uint16_t manufacturer, uint16_t device);
void emu_identity_am29_secsi_from_fsn(
    uint32_t fsn, uint16_t manufacturer, uint16_t device, uint8_t secsi[8]);
void emu_identity_imei_mirror(const char imei[15], uint8_t mirror[8]);

emu_error_code_t emu_identity_plan_fsn(
    emu_prepared_session_t *prepared, uint32_t fsn, const char *provenance,
    emu_identity_plan_result_t *result, emu_error_t *error);
emu_error_code_t emu_identity_plan_fsn_imei(
    emu_prepared_session_t *prepared, uint32_t fsn, const char imei[15],
    const char *provenance, emu_identity_plan_result_t *result,
    emu_error_t *error);
emu_error_code_t emu_identity_plan_bundle(
    emu_prepared_session_t *prepared, const emu_identity_bundle_t *bundle,
    const char *provenance, int allow_fsn_only_fallback,
    emu_identity_plan_result_t *result, emu_error_t *error);
emu_error_code_t emu_identity_plan_file(
    emu_prepared_session_t *prepared, const char *path,
    emu_identity_plan_result_t *result, emu_error_t *error);
emu_error_code_t emu_identity_plan_default(
    emu_prepared_session_t *prepared, emu_identity_plan_result_t *result,
    emu_error_t *error);

#endif
