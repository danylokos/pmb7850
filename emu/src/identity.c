#include "emu_identity.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *cursor;
    const char *end;
    emu_error_t *error;
} json_cursor_t;

typedef struct {
    uint16_t id;
    uint16_t length;
    int required;
} block_description_t;

static const block_description_t BLOCKS[EMU_IDENTITY_BLOCK_COUNT] = {
    {76, 10, 1}, {5008, 224, 1}, {5009, 10, 1},
    {5077, 232, 1}, {67, 20, 0},
};

static const emu_identity_bundle_t DEFAULT_BUNDLE = {
    .imei = "11223344556677",
    .fsn = 0x1234ABCDu,
    .blocks = {
        {.id = 76, .length = 10, .bytes = {
            0x36,0xE6,0x0C,0x6E,0x2E,0x07,0x60,0xFE,0x83,0x7D,
        }},
        {.id = 5008, .length = 224, .bytes = {
            0xDD,0xEF,0x51,0xEC,0x17,0xEA,0xAE,0x91,0x32,0x24,0x3A,0x4E,
            0x16,0x8D,0x2D,0x6F,0x70,0xB0,0xFD,0xD1,0xE1,0x80,0xA8,0x5A,
            0xDF,0x7A,0x14,0xDC,0xFC,0x37,0x70,0xD0,0xDD,0xEF,0x51,0xEC,
            0x17,0xEA,0xAE,0x91,0xCD,0x90,0x8B,0xA4,0x9B,0x65,0xFF,0xCE,
            0xDE,0x72,0x01,0xD9,0xFE,0x8B,0x87,0x7D,0x9B,0x6D,0x6F,0x11,
            0x25,0xEE,0x3F,0xE0,0xAB,0x4A,0x65,0x09,0xDC,0xCF,0x66,0xD7,
            0xD2,0xD5,0x99,0x5F,0xCD,0x72,0x92,0x8D,0xF9,0x24,0x08,0x26,
            0x11,0xEF,0xDC,0x6E,0x31,0x92,0xD7,0x1C,0x91,0x46,0x33,0x13,
            0x6E,0x06,0xB2,0x64,0xFA,0xCA,0x04,0x8B,0x9E,0x87,0x4E,0x3E,
            0x65,0x6F,0xD6,0xE3,0x56,0x11,0xCB,0xB8,0xBB,0x16,0x1B,0x86,
            0x54,0x52,0x76,0x5A,0x6A,0xE0,0x66,0x91,0x6F,0xE6,0x08,0x37,
            0xBB,0x8C,0xD1,0x33,0x63,0xA5,0x01,0xEF,0x2B,0x19,0xD4,0xAE,
            0x83,0x51,0x10,0x54,0xF1,0xB5,0x11,0x33,0x04,0x46,0xBF,0x06,
            0x98,0x8A,0x61,0x0D,0x89,0xA6,0x51,0xCC,0xD6,0x27,0x57,0x93,
            0x31,0x78,0x14,0x4E,0x0D,0x89,0x1F,0xD1,0x1A,0x57,0x08,0xEE,
            0x9D,0x05,0xCE,0xF7,0x32,0xCF,0xC3,0xE3,0xAF,0xAA,0x57,0xE5,
            0x39,0x05,0x51,0x30,0xE5,0xEE,0xF5,0x89,0x8C,0xFD,0x9D,0xA5,
            0x8D,0x9E,0x9D,0x98,0x48,0x76,0xAC,0x6F,0x3C,0xBC,0x44,0x39,
            0xCC,0x08,0xDC,0xC8,0xD7,0x39,0x2A,0xF7,
        }},
        {.id = 5009, .length = 10, .bytes = {
            0x76,0xCA,0xAC,0x2C,0xB4,0xA0,0xC4,0x03,0xA6,0xB9,
        }},
        {.id = 5077, .length = 232, .bytes = {
            0xDD,0xEF,0x51,0xEC,0x17,0xEA,0xAE,0x91,0xCD,0x90,0x8B,0xA4,
            0x9B,0x65,0xFF,0xCE,0xDE,0x72,0x01,0xD9,0xFE,0x8B,0x87,0x7D,
            0x9B,0x6D,0x6F,0x11,0x25,0xEE,0x3F,0xE0,0xAB,0x4A,0x65,0x09,
            0xDC,0xCF,0x66,0xD7,0xD2,0xD5,0x99,0x5F,0xCD,0x72,0x92,0x8D,
            0xF9,0x24,0x08,0x26,0x11,0xEF,0xDC,0x6E,0x31,0x92,0xD7,0x1C,
            0x91,0x46,0x33,0x13,0x6E,0x06,0xB2,0x64,0xFA,0xCA,0x04,0x8B,
            0x9E,0x87,0x4E,0x3E,0x65,0x6F,0xD6,0xE3,0x56,0x11,0xCB,0xB8,
            0xBB,0x16,0x1B,0x86,0x54,0x52,0x76,0x5A,0x6A,0xE0,0x66,0x91,
            0x6F,0xE6,0x08,0x37,0xBB,0x8C,0xD1,0x33,0x63,0xA5,0x01,0xEF,
            0x2B,0x19,0xD4,0xAE,0x83,0x51,0x10,0x54,0xF1,0xB5,0x11,0x33,
            0x04,0x46,0xBF,0x06,0x98,0x8A,0x61,0x0D,0x89,0xA6,0x51,0xCC,
            0xD6,0x27,0x57,0x93,0x31,0x78,0x14,0x4E,0x0D,0x89,0x1F,0xD1,
            0x1A,0x57,0x08,0xEE,0x9D,0x05,0xCE,0xF7,0x32,0xCF,0xC3,0xE3,
            0xAF,0xAA,0x57,0xE5,0x39,0xE5,0xF5,0x27,0xCF,0xEE,0x1A,0x39,
            0xAF,0x15,0xC2,0xA4,0x82,0xAF,0xE5,0xAE,0xDF,0x09,0x31,0x61,
            0x0B,0x8B,0x38,0xCF,0x1D,0x04,0x20,0x97,0xB5,0xF0,0x7E,0xDE,
            0x61,0xE5,0x26,0xA6,0xC0,0xD8,0x5D,0x60,0xBC,0x9A,0xF0,0x83,
            0xA5,0x05,0x74,0x87,0x1A,0xBB,0x61,0x2D,0x4C,0x23,0x0A,0xFF,
            0x51,0xB7,0x0D,0xEA,0x23,0xF8,0x4D,0x4C,0xED,0x30,0x47,0x72,
            0xB1,0x79,0xFF,0xC6,
        }},
    },
};

static emu_error_code_t identity_fail(emu_error_t *error,
                                      emu_error_code_t code,
                                      const char *format, ...) {
    if (error) {
        va_list arguments;
        va_start(arguments, format);
        error->code = code;
        vsnprintf(error->message, sizeof error->message, format, arguments);
        va_end(arguments);
    }
    return code;
}

static void identity_ok(emu_error_t *error) {
    if (error) {
        error->code = EMU_OK;
        error->message[0] = 0;
    }
}

const emu_identity_bundle_t *emu_identity_default_bundle(void) {
    return &DEFAULT_BUNDLE;
}

static emu_error_code_t json_fail(json_cursor_t *json,
                                  const char *message) {
    return identity_fail(json->error, EMU_ERR_INVALID_PREPARED_SESSION,
                         "invalid overlay JSON: %s", message);
}

static void json_whitespace(json_cursor_t *json) {
    while (json->cursor < json->end &&
           (*json->cursor == ' ' || *json->cursor == '\t' ||
            *json->cursor == '\r' || *json->cursor == '\n'))
        json->cursor++;
}

static int json_take(json_cursor_t *json, char expected) {
    json_whitespace(json);
    if (json->cursor >= json->end || *json->cursor != expected) {
        json_fail(json, "unexpected token");
        return 0;
    }
    json->cursor++;
    return 1;
}

static int json_string(json_cursor_t *json, char *out, size_t capacity) {
    json_whitespace(json);
    if (json->cursor >= json->end || *json->cursor++ != '"') {
        json_fail(json, "expected string");
        return 0;
    }
    size_t length = 0;
    while (json->cursor < json->end && *json->cursor != '"') {
        unsigned char byte = (unsigned char)*json->cursor++;
        if (byte < 0x20 || byte == '\\') {
            json_fail(json, "string escapes/control bytes are not accepted");
            return 0;
        }
        if (length + 1u >= capacity) {
            json_fail(json, "string is too long");
            return 0;
        }
        out[length++] = (char)byte;
    }
    if (json->cursor >= json->end) {
        json_fail(json, "unterminated string");
        return 0;
    }
    json->cursor++;
    out[length] = 0;
    return 1;
}

static int json_uint(json_cursor_t *json, uint32_t *out) {
    json_whitespace(json);
    if (json->cursor >= json->end || *json->cursor < '0' ||
        *json->cursor > '9') {
        json_fail(json, "expected unsigned integer");
        return 0;
    }
    uint64_t value = 0;
    do {
        value = value * 10u + (unsigned)(*json->cursor++ - '0');
        if (value > UINT32_MAX) {
            json_fail(json, "integer is too large");
            return 0;
        }
    } while (json->cursor < json->end && *json->cursor >= '0' &&
             *json->cursor <= '9');
    *out = (uint32_t)value;
    return 1;
}

static int hex_nibble(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
    return -1;
}

static int json_hex(json_cursor_t *json, uint8_t *out, size_t expected) {
    json_whitespace(json);
    if (json->cursor >= json->end || *json->cursor++ != '"') {
        json_fail(json, "expected hex string");
        return 0;
    }
    for (size_t i = 0; i < expected; i++) {
        if ((size_t)(json->end - json->cursor) < 2u) {
            json_fail(json, "hex payload is too short");
            return 0;
        }
        int high = hex_nibble(json->cursor[0]);
        int low = hex_nibble(json->cursor[1]);
        if (high < 0 || low < 0) {
            json_fail(json, "hex payload contains a non-hex digit");
            return 0;
        }
        out[i] = (uint8_t)((high << 4) | low);
        json->cursor += 2;
    }
    if (json->cursor >= json->end || *json->cursor++ != '"') {
        json_fail(json, "hex payload has the wrong length");
        return 0;
    }
    return 1;
}

static int block_index(const char *key) {
    for (size_t i = 0; i < EMU_IDENTITY_BLOCK_COUNT; i++) {
        char expected[8];
        snprintf(expected, sizeof expected, "%u", BLOCKS[i].id);
        if (!strcmp(key, expected)) return (int)i;
    }
    return -1;
}

static int parse_blocks(json_cursor_t *json,
                        emu_identity_bundle_t *bundle) {
    if (!json_take(json, '{')) return 0;
    unsigned seen = 0;
    for (;;) {
        json_whitespace(json);
        if (json->cursor < json->end && *json->cursor == '}') {
            json->cursor++;
            break;
        }
        if (seen && !json_take(json, ',')) return 0;
        char key[16];
        if (!json_string(json, key, sizeof key) || !json_take(json, ':'))
            return 0;
        int index = block_index(key);
        if (index < 0) {
            json_fail(json, "unknown block id");
            return 0;
        }
        unsigned bit = 1u << (unsigned)index;
        if (seen & bit) {
            json_fail(json, "duplicate block id");
            return 0;
        }
        emu_identity_block_t *block = &bundle->blocks[index];
        block->id = BLOCKS[index].id;
        block->length = BLOCKS[index].length;
        if (!json_hex(json, block->bytes, block->length)) return 0;
        seen |= bit;
    }
    for (size_t i = 0; i < EMU_IDENTITY_BLOCK_COUNT; i++) {
        if (BLOCKS[i].required && !(seen & (1u << i))) {
            json_fail(json, "missing required block id");
            return 0;
        }
    }
    return 1;
}

static int parse_root(json_cursor_t *json,
                      emu_identity_bundle_t *bundle) {
    enum {
        FIELD_SCHEMA = 1u << 0,
        FIELD_SCHEMA_VERSION = 1u << 1,
        FIELD_PROFILE = 1u << 2,
        FIELD_PROFILE_VERSION = 1u << 3,
        FIELD_MODEL = 1u << 4,
        FIELD_IMEI = 1u << 5,
        FIELD_FSN = 1u << 6,
        FIELD_BLOCKS = 1u << 7,
    };
    const unsigned all_fields = (1u << 8) - 1u;
    unsigned seen = 0;
    char schema[64] = "";
    char profile[64] = "";
    uint32_t schema_version = 0;
    uint32_t profile_version = 0;
    uint32_t model = 0;
    uint8_t fsn_bytes[4] = {0};

    if (!json_take(json, '{')) return 0;
    for (;;) {
        json_whitespace(json);
        if (json->cursor < json->end && *json->cursor == '}') {
            json->cursor++;
            break;
        }
        if (seen && !json_take(json, ',')) return 0;
        char key[64];
        if (!json_string(json, key, sizeof key) || !json_take(json, ':'))
            return 0;
        unsigned bit = 0;
        int ok = 0;
        if (!strcmp(key, "schema")) {
            bit = FIELD_SCHEMA;
            ok = json_string(json, schema, sizeof schema);
        } else if (!strcmp(key, "schema_version")) {
            bit = FIELD_SCHEMA_VERSION;
            ok = json_uint(json, &schema_version);
        } else if (!strcmp(key, "profile")) {
            bit = FIELD_PROFILE;
            ok = json_string(json, profile, sizeof profile);
        } else if (!strcmp(key, "profile_version")) {
            bit = FIELD_PROFILE_VERSION;
            ok = json_uint(json, &profile_version);
        } else if (!strcmp(key, "crypto_model_id")) {
            bit = FIELD_MODEL;
            ok = json_uint(json, &model);
        } else if (!strcmp(key, "imei")) {
            bit = FIELD_IMEI;
            ok = json_string(json, bundle->imei, sizeof bundle->imei);
        } else if (!strcmp(key, "fsn")) {
            bit = FIELD_FSN;
            ok = json_hex(json, fsn_bytes, sizeof fsn_bytes);
        } else if (!strcmp(key, "blocks")) {
            bit = FIELD_BLOCKS;
            ok = parse_blocks(json, bundle);
        } else {
            json_fail(json, "unknown root field");
            return 0;
        }
        if (seen & bit) {
            json_fail(json, "duplicate root field");
            return 0;
        }
        if (!ok) return 0;
        seen |= bit;
    }
    json_whitespace(json);
    if (json->cursor != json->end) {
        json_fail(json, "trailing content");
        return 0;
    }
    if (seen != all_fields) {
        json_fail(json, "missing required root field");
        return 0;
    }
    if (strcmp(schema, "cemu.eeprom-identity-overlay") ||
        schema_version != 2) {
        json_fail(json, "unsupported schema/version");
        return 0;
    }
    if (strcmp(profile, "freia15-model7-unlocked") ||
        profile_version != 1 || model != 7) {
        json_fail(json, "unsupported profile/model");
        return 0;
    }
    for (size_t i = 0; i < 14; i++) {
        if (bundle->imei[i] < '0' || bundle->imei[i] > '9') {
            json_fail(json, "IMEI is not exactly 14 decimal digits");
            return 0;
        }
    }
    bundle->fsn = ((uint32_t)fsn_bytes[0] << 24) |
                  ((uint32_t)fsn_bytes[1] << 16) |
                  ((uint32_t)fsn_bytes[2] << 8) | fsn_bytes[3];
    return 1;
}

emu_error_code_t emu_identity_bundle_parse(
        const char *document, size_t size, emu_identity_bundle_t *bundle,
        emu_error_t *error) {
    if (!document || !bundle)
        return identity_fail(error, EMU_ERR_ARGUMENT,
                             "invalid overlay arguments");
    emu_identity_bundle_t parsed;
    memset(&parsed, 0, sizeof parsed);
    json_cursor_t json = {document, document + size, error};
    if (!parse_root(&json, &parsed))
        return error ? error->code : EMU_ERR_INVALID_PREPARED_SESSION;
    *bundle = parsed;
    identity_ok(error);
    return EMU_OK;
}

emu_error_code_t emu_identity_bundle_load(
        const char *path, emu_identity_bundle_t *bundle,
        emu_error_t *error) {
    if (!path || !bundle)
        return identity_fail(error, EMU_ERR_ARGUMENT,
                             "invalid overlay arguments");
    FILE *file = fopen(path, "rb");
    if (!file)
        return identity_fail(error, EMU_ERR_IO,
                             "cannot open overlay bundle: %s", path);
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        fclose(file);
        return identity_fail(error, EMU_ERR_IO,
                             "cannot read overlay bundle");
    }
    long length = ftell(file);
    if (length > 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return identity_fail(error, EMU_ERR_IO,
                             "overlay bundle has invalid size");
    }
    char *document = malloc((size_t)length + 1u);
    if (!document) {
        fclose(file);
        return identity_fail(error, EMU_ERR_NOMEM,
                             "out of memory reading overlay bundle");
    }
    if (fread(document, 1, (size_t)length, file) != (size_t)length) {
        free(document);
        fclose(file);
        return identity_fail(error, EMU_ERR_IO,
                             "cannot read overlay bundle");
    }
    fclose(file);
    document[length] = 0;
    emu_error_code_t code = emu_identity_bundle_parse(
        document, (size_t)length, bundle, error);
    free(document);
    return code;
}

static uint16_t read16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint16_t rotate_left16(uint16_t value, unsigned count) {
    return (uint16_t)((value << count) | (value >> (16u - count)));
}

static uint16_t rotate_right16(uint16_t value, unsigned count) {
    return (uint16_t)((value >> count) | (value << (16u - count)));
}

static uint32_t fsn_from_identity(const uint8_t identity[8],
                                  uint16_t manufacturer, uint16_t device) {
    uint16_t word0 = read16(identity);
    uint16_t word1 = read16(identity + 2);
    uint16_t word2 = read16(identity + 4);
    uint16_t word3 = read16(identity + 6);
    uint16_t low = rotate_right16(
        (uint16_t)(rotate_right16((uint16_t)(manufacturer ^ word0), 5) ^
                   word2), 3);
    uint16_t high = rotate_left16(
        (uint16_t)(rotate_left16((uint16_t)(device ^ word1), 4) ^ word3),
        2);
    return ((uint32_t)high << 16) | low;
}

static void identity_from_fsn(uint32_t fsn, uint16_t manufacturer,
                              uint16_t device, uint8_t identity[8]) {
    uint16_t word2 = (uint16_t)(rotate_left16((uint16_t)fsn, 3) ^
                                  rotate_right16(manufacturer, 5));
    uint16_t word3 = (uint16_t)(rotate_right16((uint16_t)(fsn >> 16), 2) ^
                                  rotate_left16(device, 4));
    const uint16_t words[4] = {0, 0, word2, word3};
    for (size_t i = 0; i < 4; i++) {
        identity[2u * i] = (uint8_t)words[i];
        identity[2u * i + 1u] = (uint8_t)(words[i] >> 8);
    }
}

uint32_t emu_identity_fsn_from_factory_uid(
        const uint8_t uid[8], uint16_t manufacturer, uint16_t device) {
    return fsn_from_identity(uid, manufacturer, device);
}

void emu_identity_factory_uid_from_fsn(
        uint32_t fsn, uint16_t manufacturer, uint16_t device,
        uint8_t uid[8]) {
    identity_from_fsn(fsn, manufacturer, device, uid);
}

uint32_t emu_identity_fsn_from_am29_secsi(
        const uint8_t secsi[8], uint16_t manufacturer, uint16_t device) {
    return fsn_from_identity(secsi, manufacturer, device);
}

void emu_identity_am29_secsi_from_fsn(
        uint32_t fsn, uint16_t manufacturer, uint16_t device,
        uint8_t secsi[8]) {
    identity_from_fsn(fsn, manufacturer, device, secsi);
}

void emu_identity_imei_mirror(const char imei[15], uint8_t mirror[8]) {
    unsigned total = 0;
    for (size_t i = 0; i < 14; i++) {
        unsigned digit = (unsigned)(imei[i] - '0');
        unsigned value = digit * ((i & 1u) ? 2u : 1u);
        total += value / 10u + value % 10u;
    }
    for (size_t i = 0; i < 7; i++)
        mirror[i] = (uint8_t)((imei[2u * i] - '0') |
                              ((imei[2u * i + 1u] - '0') << 4));
    mirror[7] = (uint8_t)(((10u - total % 10u) % 10u) << 4);
}

static void discard_operations(emu_prepared_session_t *prepared,
                               size_t first) {
    while (prepared->operation_count > first) {
        emu_storage_operation_t *operation =
            &prepared->operations[--prepared->operation_count];
        free(operation->expected);
        free(operation->replacement);
        memset(operation, 0, sizeof *operation);
    }
}

static int identity_parameters(const emu_prepared_session_t *prepared,
                               uint16_t *manufacturer, uint16_t *device) {
    if (prepared->identity_chip_index >= prepared->chip_count) return 0;
    const char *model = prepared->chips[prepared->identity_chip_index].model;
    if (!strcmp(model, "m58lw064d")) {
        *manufacturer = 0x0020;
        *device = 0x0017;
    } else if (!strcmp(model, "w30-64mbit-top")) {
        *manufacturer = 0x0089;
        *device = 0x8854;
    } else if (!strcmp(model, "w30-128mbit-top")) {
        *manufacturer = 0x0089;
        *device = 0x8856;
    } else if (!strcmp(model, "am29lv640mh") ||
               !strcmp(model, "am29lv128mh")) {
        *manufacturer = 0x0001;
        *device = 0x227E;
    } else {
        return 0;
    }
    return 1;
}

static void set_plan_result(emu_identity_plan_result_t *result,
                            const char *source, uint32_t fsn,
                            const char *imei, size_t record_count,
                            int fallback) {
    if (!result) return;
    memset(result, 0, sizeof *result);
    snprintf(result->source, sizeof result->source, "%s", source);
    result->fsn = fsn;
    if (imei) snprintf(result->imei, sizeof result->imei, "%s", imei);
    result->record_count = record_count;
    result->fsn_only_fallback = fallback;
}

static size_t next_storage_group(const emu_prepared_session_t *prepared) {
    return prepared->operation_count
         ? prepared->operations[prepared->operation_count - 1u].group + 1u
         : 1u;
}

static emu_error_code_t append_identity_operations(
        emu_prepared_session_t *prepared, uint32_t fsn, const char *imei,
        const char *provenance, size_t group, emu_error_t *error) {
    uint16_t manufacturer = 0;
    uint16_t device = 0;
    if (!prepared || !prepared->source.bytes || !provenance ||
        !provenance[0] || prepared->identity_kind == EMU_IDENTITY_NONE ||
        !identity_parameters(prepared, &manufacturer, &device))
        return identity_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                             "identity mapping not yet proven for this device");
    uint8_t identity[8];
    emu_storage_space_t space;
    if (prepared->identity_kind == EMU_IDENTITY_FACTORY_UID) {
        emu_identity_factory_uid_from_fsn(fsn, manufacturer, device,
                                          identity);
        if (emu_identity_fsn_from_factory_uid(
                identity, manufacturer, device) != fsn)
            return identity_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                 "cannot prepare factory-UID identity from FSN");
        space = EMU_STORAGE_FACTORY_UID;
    } else if (prepared->identity_kind == EMU_IDENTITY_AM29_SECSI) {
        emu_identity_am29_secsi_from_fsn(fsn, manufacturer, device,
                                         identity);
        if (emu_identity_fsn_from_am29_secsi(
                identity, manufacturer, device) != fsn)
            return identity_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                 "cannot prepare AM29 SecSi identity from FSN");
        space = EMU_STORAGE_AM29_FACTORY_SECSI;
    } else {
        return identity_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                             "identity mapping not yet proven for this device");
    }
    emu_error_code_t code = emu_prepared_add_grouped_storage_operation(
        prepared, EMU_STORAGE_STAGE_PRE_RESET, group,
        prepared->identity_chip_index, space, 0, NULL, 0, identity,
        sizeof identity, provenance, error);
    if (code != EMU_OK || !imei ||
        prepared->identity_kind != EMU_IDENTITY_AM29_SECSI)
        return code;
    uint8_t mirror[8];
    emu_identity_imei_mirror(imei, mirror);
    return emu_prepared_add_grouped_storage_operation(
        prepared, EMU_STORAGE_STAGE_PRE_RESET, group,
        prepared->identity_chip_index, EMU_STORAGE_AM29_CUSTOMER_SECSI,
        0x10, NULL, 0, mirror, sizeof mirror, provenance, error);
}

emu_error_code_t emu_identity_plan_fsn(
        emu_prepared_session_t *prepared, uint32_t fsn,
        const char *provenance, emu_identity_plan_result_t *result,
        emu_error_t *error) {
    if (!prepared || !provenance || !provenance[0])
        return identity_fail(error, EMU_ERR_ARGUMENT,
                             "invalid FSN identity planning arguments");
    size_t first = prepared->operation_count;
    size_t group = next_storage_group(prepared);
    emu_error_code_t code = append_identity_operations(
        prepared, fsn, NULL, provenance, group, error);
    if (code != EMU_OK) {
        discard_operations(prepared, first);
        return code;
    }
    set_plan_result(result, provenance, fsn, NULL, 0, 0);
    identity_ok(error);
    return EMU_OK;
}

emu_error_code_t emu_identity_plan_fsn_imei(
        emu_prepared_session_t *prepared, uint32_t fsn,
        const char imei[15], const char *provenance,
        emu_identity_plan_result_t *result, emu_error_t *error) {
    if (!prepared || !imei || strlen(imei) != 14u ||
        strspn(imei, "0123456789") != 14u || !provenance || !provenance[0])
        return identity_fail(error, EMU_ERR_ARGUMENT,
                             "invalid FSN/IMEI identity planning arguments");
    if (prepared->identity_kind != EMU_IDENTITY_AM29_SECSI)
        return identity_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                             "--imei applies only to AM29 SecSi devices");
    size_t first = prepared->operation_count;
    size_t group = next_storage_group(prepared);
    emu_error_code_t code = append_identity_operations(
        prepared, fsn, imei, provenance, group, error);
    if (code != EMU_OK) {
        discard_operations(prepared, first);
        return code;
    }
    set_plan_result(result, provenance, fsn, imei, 0, 0);
    identity_ok(error);
    return EMU_OK;
}

static emu_error_code_t resolve_bundle_targets(
        const uint8_t *chip, size_t chip_size,
        const emu_identity_bundle_t *bundle,
        emu_eeprom_location_t locations[EMU_IDENTITY_BLOCK_COUNT],
        emu_error_t *error) {
    memset(locations, 0,
           EMU_IDENTITY_BLOCK_COUNT * sizeof *locations);
    emu_eeprom_catalog_t catalog = {0};
    emu_error_code_t code = emu_eeprom_catalog_parse(
        chip, chip_size, &catalog, error);
    if (code != EMU_OK) return code;
    for (size_t i = 0; i < EMU_IDENTITY_BLOCK_COUNT; i++) {
        int requested = BLOCKS[i].required || bundle->blocks[i].id;
        if (!requested) continue;
        const emu_eeprom_record_t *record =
            emu_eeprom_catalog_resolve(&catalog, BLOCKS[i].id);
        if (!record) {
            emu_eeprom_catalog_free(&catalog);
            return identity_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                 "EEPROM block %u is missing", BLOCKS[i].id);
        }
        locations[i] = *record;
        if (locations[i].length != BLOCKS[i].length) {
            emu_eeprom_catalog_free(&catalog);
            return identity_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                 "EEPROM block %u length is %u, expected %u",
                                 locations[i].id, locations[i].length,
                                 BLOCKS[i].length);
        }
    }
    emu_eeprom_catalog_free(&catalog);
    return EMU_OK;
}

emu_error_code_t emu_identity_plan_bundle(
        emu_prepared_session_t *prepared,
        const emu_identity_bundle_t *bundle, const char *provenance,
        int allow_fsn_only_fallback, emu_identity_plan_result_t *result,
        emu_error_t *error) {
    if (!prepared || !prepared->source.bytes || !bundle || !provenance ||
        !provenance[0] || !prepared->chip_count)
        return identity_fail(error, EMU_ERR_ARGUMENT,
                             "invalid EEPROM identity planning arguments");
    size_t first = prepared->operation_count;
    size_t group = next_storage_group(prepared);
    size_t directory_chip = SIZE_MAX;
    emu_eeprom_location_t locations[EMU_IDENTITY_BLOCK_COUNT];
    for (size_t i = 0; i < prepared->chip_count; i++) {
        const emu_chip_view_t *chip = &prepared->chips[i];
        emu_error_t ignored = {0};
        if (resolve_bundle_targets(
                prepared->source.bytes + chip->source_offset, chip->size,
                bundle, locations, &ignored) == EMU_OK) {
            directory_chip = i;
            break;
        }
    }
    if (directory_chip == SIZE_MAX) {
        if (!allow_fsn_only_fallback)
            return identity_fail(
                error, EMU_ERR_INVALID_PREPARED_SESSION,
                "could not resolve EEPROM blocks in configured flash chips");
        emu_error_code_t code = append_identity_operations(
            prepared, bundle->fsn, NULL, provenance, group, error);
        if (code != EMU_OK) {
            discard_operations(prepared, first);
            return code;
        }
        set_plan_result(result, provenance, bundle->fsn, NULL, 0, 1);
        identity_ok(error);
        return EMU_OK;
    }

    size_t record_count = 0;
    const emu_chip_view_t *chip = &prepared->chips[directory_chip];
    const uint8_t *chip_bytes =
        prepared->source.bytes + chip->source_offset;
    for (size_t i = 0; i < EMU_IDENTITY_BLOCK_COUNT; i++) {
        const emu_identity_block_t *block = &bundle->blocks[i];
        if (!block->id) continue;
        if (block->id != locations[i].id ||
            block->length != locations[i].length) {
            discard_operations(prepared, first);
            return identity_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                 "cannot route EEPROM block %u", block->id);
        }
        emu_error_code_t code = emu_prepared_add_grouped_storage_operation(
            prepared, EMU_STORAGE_STAGE_PRE_RESET, group, directory_chip,
            EMU_STORAGE_MAIN_ARRAY, locations[i].chip_offset,
            chip_bytes + locations[i].chip_offset, locations[i].length,
            block->bytes, block->length, provenance, error);
        if (code != EMU_OK) {
            discard_operations(prepared, first);
            return code;
        }
        record_count++;
    }
    emu_error_code_t code = append_identity_operations(
        prepared, bundle->fsn, bundle->imei, provenance, group, error);
    if (code != EMU_OK) {
        discard_operations(prepared, first);
        return code;
    }
    set_plan_result(result, provenance, bundle->fsn, bundle->imei,
                    record_count, 0);
    identity_ok(error);
    return EMU_OK;
}

emu_error_code_t emu_identity_plan_file(
        emu_prepared_session_t *prepared, const char *path,
        emu_identity_plan_result_t *result, emu_error_t *error) {
    emu_identity_bundle_t bundle;
    emu_error_code_t code = emu_identity_bundle_load(path, &bundle, error);
    if (code != EMU_OK) return code;
    char provenance[128];
    snprintf(provenance, sizeof provenance, "overlay:%s", path);
    return emu_identity_plan_bundle(prepared, &bundle, provenance, 0,
                                    result, error);
}

emu_error_code_t emu_identity_plan_default(
        emu_prepared_session_t *prepared,
        emu_identity_plan_result_t *result, emu_error_t *error) {
    return emu_identity_plan_bundle(prepared, &DEFAULT_BUNDLE,
                                    "built-in", 1, result, error);
}
