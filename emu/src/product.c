#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "emu_product.h"

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
#define MIB(n) ((size_t)(n) * 1024u * 1024u)
#define IMAGE_METADATA_PAGE_OFFSET 0xFF50u
#define IMAGE_METADATA_VIEW_OFFSET 0x7FF50u
#define IMAGE_FLASH_ID_OFFSET 0x7FE26u
#define IMAGE_BCORE_SW_OFFSET 0x032Cu
#define MAX_IMAGE_METADATA_INSTANCES 32u

#define COMMON_CAPS (EMU_PRODUCT_KEYS | EMU_PRODUCT_DISPLAY | EMU_PRODUCT_SIM)
#define SERIAL_CAPS (COMMON_CAPS | EMU_PRODUCT_SERIAL)
#define BATTERY_CAPS (COMMON_CAPS | EMU_PRODUCT_BATTERY)
#define SERIAL_BATTERY_CAPS (SERIAL_CAPS | EMU_PRODUCT_BATTERY)

static const emu_logical_key_t KEYS_COMMON[] = {
    {"0",0x020B,0x30},{"1",0x0107,0x31},{"2",0x010B,0x32},
    {"3",0x010D,0x33},{"4",0x0087,0x34},{"5",0x008B,0x35},
    {"6",0x008D,0x36},{"7",0x0047,0x37},{"8",0x004B,0x38},
    {"9",0x004D,0x39},{"star",0x0207,0x2A},{"hash",0x020D,0x23},
    {"up",0x008E,0x3B},{"down",0x004E,0x3C},
    {"soft-left",0x010E,0x01},{"soft-right",0x020E,0x04},
    {"send",0x0017,0x0B},{"power",0x000D,0x0C},
};

static const emu_logical_key_t KEYS_COLOR[] = {
    {"0",0x020B,0x30},{"1",0x0107,0x31},{"2",0x010B,0x32},
    {"3",0x010D,0x33},{"4",0x0087,0x34},{"5",0x008B,0x35},
    {"6",0x008D,0x36},{"7",0x0047,0x37},{"8",0x004B,0x38},
    {"9",0x004D,0x39},{"star",0x0207,0x2A},{"hash",0x020D,0x23},
    {"up",0x008E,0x3B},{"down",0x004E,0x3C},
    {"left",0x010E,0x3D},{"right",0x020E,0x3E},
    {"soft-left",0x001E,0x01},{"soft-right",0x001B,0x04},
    {"send",0x0017,0x0B},{"power",0x000D,0x0C},
};

static const emu_logical_key_t KEYS_M55_S55[] = {
    {"0",0x001B,0x30},{"1",0x0107,0x31},{"2",0x010B,0x32},
    {"3",0x010D,0x33},{"4",0x0087,0x34},{"5",0x008B,0x35},
    {"6",0x008D,0x36},{"7",0x0047,0x37},{"8",0x004B,0x38},
    {"9",0x004D,0x39},{"star",0x0017,0x2A},{"hash",0x001D,0x23},
    {"up",0x010E,0x3B},{"down",0x001E,0x3C},
    {"left",0x020B,0x3D},{"right",0x004E,0x3E},
    {"soft-left",0x0207,0x01},{"soft-right",0x008E,0x04},
    {"send",0x020E,0x0B},{"power",0x000D,0x0C},
};

static const emu_logical_key_t KEYS_SL55[] = {
    {"0",0x004D,0x30},{"1",0x001E,0x31},{"2",0x004E,0x32},
    {"3",0x008E,0x33},{"4",0x0017,0x34},{"5",0x0047,0x35},
    {"6",0x0087,0x36},{"7",0x001B,0x37},{"8",0x004B,0x38},
    {"9",0x008B,0x39},{"star",0x001D,0x2A},{"hash",0x008D,0x23},
    {"up",0x010E,0x3B},{"down",0x010D,0x3C},
    {"left",0x0107,0x3D},{"right",0x010B,0x3E},
    {"soft-left",0x0007,0x01},{"soft-right",0x000B,0x04},
    {"send",0x000E,0x0B},{"power",0x000D,0x0C},
};

#define MONO_DISPLAY {101,64,1,0,1,1}
#define COLOR_DISPLAY {101,80,24,0,1,0}
#define CHIP(model_, size_) \
    {"primary","flash",model_,{model_},1,size_}
#define CHIP_ALTERNATE(default_, alternate_, size_) \
    {"primary","flash",default_,{default_,alternate_},2,size_}
#define CHIP_4M CHIP("m58lw064d",MIB(4))
#define CHIP_8M CHIP("m58lw064d",MIB(8))
#define CHIP_AMD8 CHIP("am29lv640mh",MIB(8))
#define CHIP_AMD16 CHIP("am29lv128mh",MIB(16))
#define CHIP_W30_16 CHIP("w30-128mbit-top",MIB(16))
#define CHIP_AMD_W30_16 \
    CHIP_ALTERNATE("am29lv128mh","w30-128mbit-top",MIB(16))
#define DUAL_CHIPS \
    {{"primary","flash-primary","w30-64mbit-top", \
      {"w30-64mbit-top"},1,MIB(8)}, \
     {"secondary","flash-secondary","m58lw064d", \
      {"m58lw064d"},1,MIB(4)}}

static const emu_product_t PRODUCTS[] = {
    {"a52","A52",{CHIP_4M},1,0,0,MONO_DISPLAY,
     KEYS_COMMON,COUNT_OF(KEYS_COMMON),BATTERY_CAPS},
    {"a55","A55",{CHIP_8M},1,0,0,MONO_DISPLAY,
     KEYS_COMMON,COUNT_OF(KEYS_COMMON),BATTERY_CAPS},
    {"a60","A60",{CHIP_AMD8},1,0,0,COLOR_DISPLAY,
     KEYS_COLOR,COUNT_OF(KEYS_COLOR),SERIAL_CAPS},
    {"a62","A62",{CHIP_AMD8},1,0,0,COLOR_DISPLAY,
     KEYS_COLOR,COUNT_OF(KEYS_COLOR),SERIAL_CAPS},
    {"a65","A65",{CHIP_AMD_W30_16},1,0,0,COLOR_DISPLAY,
     KEYS_COLOR,COUNT_OF(KEYS_COLOR),SERIAL_CAPS},
    {"c55","C55",{CHIP_8M},1,0,0,MONO_DISPLAY,
     KEYS_COMMON,COUNT_OF(KEYS_COMMON),SERIAL_BATTERY_CAPS},
    {"c60","C60",{CHIP_AMD_W30_16},1,0,0,COLOR_DISPLAY,
     KEYS_COLOR,COUNT_OF(KEYS_COLOR),SERIAL_CAPS},
    {"cf62","CF62",{CHIP_AMD_W30_16},1,0,0,{130,130,0,0,0,0},
     KEYS_COLOR,COUNT_OF(KEYS_COLOR),SERIAL_CAPS},
    {"m55","M55",{CHIP_AMD16},1,0,0,COLOR_DISPLAY,
     KEYS_M55_S55,COUNT_OF(KEYS_M55_S55),SERIAL_CAPS},
    {"mc60","MC60",{CHIP_AMD16},1,0,0,COLOR_DISPLAY,
     KEYS_COMMON,COUNT_OF(KEYS_COMMON),SERIAL_CAPS},
    {"s55","S55",DUAL_CHIPS,2,0,0,{101,80,13,0,1,0},
     KEYS_M55_S55,COUNT_OF(KEYS_M55_S55),SERIAL_CAPS},
    {"sl55","SL55",DUAL_CHIPS,2,0,0,{101,80,1,0,1,0},
     KEYS_SL55,COUNT_OF(KEYS_SL55),SERIAL_CAPS},
};

typedef struct {
    emu_image_metadata_t metadata;
    size_t topology_size;
} emu_metadata_instance_t;

static emu_error_code_t product_fail(emu_error_t *error,
                                     emu_error_code_t code,
                                     const char *format, ...) {
    if (error) {
        va_list args;
        error->code = code;
        va_start(args, format);
        vsnprintf(error->message, sizeof error->message, format, args);
        va_end(args);
    }
    return code;
}

static void product_ok(emu_error_t *error) {
    if (error) { error->code = EMU_OK; error->message[0] = 0; }
}

static int text_equal_folded(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return !*a && !*b;
}

size_t emu_product_count(void) { return COUNT_OF(PRODUCTS); }

const emu_product_t *emu_product_at(size_t index) {
    return index < COUNT_OF(PRODUCTS) ? &PRODUCTS[index] : NULL;
}

const emu_product_t *emu_product_by_name(const char *name) {
    for (size_t i = 0; name && i < COUNT_OF(PRODUCTS); i++)
        if (text_equal_folded(PRODUCTS[i].name, name)) return &PRODUCTS[i];
    return NULL;
}

const emu_logical_key_t *emu_product_key(const emu_product_t *product,
                                         const char *name) {
    if (!product || !name) return NULL;
    for (size_t i = 0; i < product->key_count; i++)
        if (!strcmp(product->keys[i].name, name)) return &product->keys[i];
    return NULL;
}

int emu_product_chip_model_allowed(const emu_product_chip_t *chip,
                                   const char *model) {
    if (!chip || !model || !chip->allowed_model_count ||
        chip->allowed_model_count > EMU_MAX_PRODUCT_CHIP_MODELS)
        return 0;
    for (size_t i = 0; i < chip->allowed_model_count; i++)
        if (chip->allowed_models[i] &&
            !strcmp(chip->allowed_models[i], model))
            return 1;
    return 0;
}

const char *emu_identity_kind_name(emu_identity_kind_t kind) {
    if (kind == EMU_IDENTITY_FACTORY_UID) return "factory-uid";
    if (kind == EMU_IDENTITY_AM29_SECSI) return "am29-secsi";
    return "none";
}

static uint16_t read_le16(const uint8_t *data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static int bcd_decode(uint8_t raw) {
    unsigned high = raw >> 4;
    unsigned low = raw & 0x0Fu;
    return high <= 9 && low <= 9 ? (int)(high * 10 + low) : -1;
}

static int metadata_text(const uint8_t *field, size_t field_size,
                         char *out, size_t out_size) {
    size_t length = 0;
    while (length < field_size && field[length]) {
        if (field[length] < 0x20 || field[length] > 0x7E) return 0;
        length++;
    }
    if (!length || length == field_size || length >= out_size) return 0;
    memcpy(out, field, length);
    out[length] = 0;
    for (size_t i = length + 1; i < field_size; i++)
        if (field[i]) return 0;
    return 1;
}

static int valid_langpack(const char *text) {
    if (!text || !((text[0] == 'l' || text[0] == 'L') &&
                   (text[1] == 'g' || text[1] == 'G')))
        return 0;
    size_t i = 2;
    if (text[i] < '0' || text[i] > '9') return 0;
    for (; text[i]; i++)
        if (text[i] < '0' || text[i] > '9') return 0;
    return 1;
}

static int valid_model(const char *text) {
    if (!text || text[0] < 'A' || text[0] > 'Z') return 0;
    size_t length = 1;
    for (; text[length]; length++)
        if (!((text[length] >= 'A' && text[length] <= 'Z') ||
              (text[length] >= '0' && text[length] <= '9')))
            return 0;
    return length >= 2 && length < sizeof(((emu_image_metadata_t *)0)->model);
}

static int classify_flash(emu_image_metadata_t *metadata,
                          size_t *topology_size) {
    uint16_t manufacturer = metadata->flash_manufacturer_id;
    uint16_t device = metadata->flash_device_id;
    const char *vendor = manufacturer == 0x0001 ? "AMD" :
                         manufacturer == 0x0020 ? "ST" :
                         manufacturer == 0x002C ? "Micron" :
                         manufacturer == 0x0089 ? "Intel" : "unknown";
    const char *engine = NULL;
    const char *classification = "exact";
    size_t size = 0;
    if (manufacturer == 0x0020 && device == 0x0017) {
        engine = "m58lw064d"; size = MIB(8);
    } else if ((manufacturer == 0x0089 || manufacturer == 0x002C) &&
               device == 0x0016) {
        engine = "m58lw064d"; size = MIB(4); classification = "compatible";
    } else if ((manufacturer == 0x0089 || manufacturer == 0x002C) &&
               device == 0x0017) {
        engine = "m58lw064d"; size = MIB(8); classification = "compatible";
    } else if (manufacturer == 0x0001 && device == 0x220C) {
        engine = "am29lv640mh"; size = MIB(8);
    } else if (manufacturer == 0x0001 && device == 0x2212) {
        engine = "am29lv128mh"; size = MIB(16);
    } else if (manufacturer == 0x0089 && device == 0x8854) {
        engine = "w30-64mbit-top"; size = MIB(8);
    } else if (manufacturer == 0x0020 && device == 0x8810) {
        engine = "w30-64mbit-top"; size = MIB(8); classification = "compatible";
    } else if (manufacturer == 0x0089 && device == 0x8856) {
        engine = "w30-128mbit-top"; size = MIB(16);
    }
    if (!engine) return 0;
    snprintf(metadata->flash_vendor, sizeof metadata->flash_vendor, "%s", vendor);
    snprintf(metadata->flash_engine, sizeof metadata->flash_engine, "%s", engine);
    snprintf(metadata->flash_classification,
             sizeof metadata->flash_classification, "%s", classification);
    *topology_size = size;
    return 1;
}

static emu_error_code_t scan_metadata(const uint8_t *data, size_t size,
                                      emu_metadata_instance_t *instances,
                                      size_t *instance_count,
                                      emu_error_t *error) {
    static const uint8_t header[] =
        {0xFF,0x0A,0x50,0x14,0x14,0x01,0x00,0xD5,0x21};
    size_t count = 0;
    for (size_t offset = IMAGE_METADATA_PAGE_OFFSET;
         offset <= size && size - offset >= 0x90u;
         offset += 0x10000u) {
        const uint8_t *record = data + offset;
        if (memcmp(record + 1, header, sizeof header) ||
            bcd_decode(record[0]) < 0 ||
            record[12] != 0xFF || record[13] != 0xFF ||
            record[14] != 0xFF || record[15] != 0xFF)
            continue;
        emu_metadata_instance_t found;
        memset(&found, 0, sizeof found);
        if (!metadata_text(record + 0x10, 16, found.metadata.langpack,
                           sizeof found.metadata.langpack) ||
            !metadata_text(record + 0x20, 16, found.metadata.model,
                           sizeof found.metadata.model) ||
            !valid_langpack(found.metadata.langpack) ||
            !valid_model(found.metadata.model) ||
            memcmp(record + 0x30, "SIEMENS\0", 8))
            continue;
        if (offset < IMAGE_METADATA_VIEW_OFFSET) continue;
        size_t view = offset - IMAGE_METADATA_VIEW_OFFSET;
        if (view > size || size - view <= IMAGE_BCORE_SW_OFFSET ||
            size - view < IMAGE_FLASH_ID_OFFSET + 4u)
            continue;
        found.metadata.software_version_raw = record[0];
        found.metadata.software_version = bcd_decode(record[0]);
        found.metadata.bcore_software_version_raw =
            data[view + IMAGE_BCORE_SW_OFFSET];
        found.metadata.bcore_software_version =
            bcd_decode(found.metadata.bcore_software_version_raw);
        found.metadata.flash_manufacturer_id =
            read_le16(data + view + IMAGE_FLASH_ID_OFFSET);
        found.metadata.flash_device_id =
            read_le16(data + view + IMAGE_FLASH_ID_OFFSET + 2u);
        found.metadata.metadata_view_offset = view;
        if (count == MAX_IMAGE_METADATA_INSTANCES)
            return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                "too many firmware metadata instances");
        if (!classify_flash(&found.metadata, &found.topology_size))
            return product_fail(
                error, EMU_ERR_INVALID_PREPARED_SESSION,
                "unsupported flash metadata %04X/%04X for image model %s at metadata-view offset 0x%zX",
                found.metadata.flash_manufacturer_id,
                found.metadata.flash_device_id, found.metadata.model, view);
        instances[count++] = found;
    }
    if (!count)
        return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "no valid Siemens firmware metadata found");
    *instance_count = count;
    return EMU_OK;
}

static int same_metadata(const emu_image_metadata_t *a,
                         const emu_image_metadata_t *b) {
    return !strcmp(a->model, b->model) && !strcmp(a->langpack, b->langpack) &&
           a->software_version_raw == b->software_version_raw &&
           a->bcore_software_version_raw == b->bcore_software_version_raw &&
           a->flash_manufacturer_id == b->flash_manufacturer_id &&
           a->flash_device_id == b->flash_device_id;
}

static int reset_mapping_matches(const emu_product_t *product,
                                 const uint8_t *data, size_t size,
                                 size_t primary_offset) {
    size_t chip_size = product->chips[product->primary_chip_index].size;
    uint8_t segment = chip_size == MIB(16) ? 0x00 : 0x80;
    if ((chip_size != MIB(4) && chip_size != MIB(8) &&
         chip_size != MIB(16)) || primary_offset > size ||
        size - primary_offset < 4u)
        return 0;
    return data[primary_offset] == 0xFA &&
           data[primary_offset + 1u] == segment;
}

static emu_identity_kind_t identity_kind_for_engine(const char *engine) {
    return engine && !strncmp(engine, "am29lv", 6)
         ? EMU_IDENTITY_AM29_SECSI : EMU_IDENTITY_FACTORY_UID;
}

emu_error_code_t emu_product_validate_prepared_storage(
        const emu_prepared_session_t *prepared, emu_error_t *error) {
    if (!prepared)
        return product_fail(error, EMU_ERR_ARGUMENT,
                            "missing prepared product storage");
    const emu_product_t *product =
        emu_product_by_name(prepared->selected_device);
    if (!product)
        return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "unknown prepared device: %s",
                            prepared->selected_device);
    if (prepared->chip_count != product->chip_count)
        return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "%s requires %zu physical flash chip%s",
                            product->name, product->chip_count,
                            product->chip_count == 1 ? "" : "s");
    if (prepared->identity_chip_index != product->identity_chip_index)
        return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "%s identity owner is physical chip %zu",
                            product->name, product->identity_chip_index);
    for (size_t i = 0; i < product->chip_count; i++) {
        const emu_product_chip_t *expected = &product->chips[i];
        const emu_chip_view_t *actual = &prepared->chips[i];
        if (strcmp(actual->role, expected->name))
            return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                "%s physical chip %zu has role %s, expected %s",
                                product->name, i, actual->role,
                                expected->name);
        if (actual->size != expected->size)
            return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                "%s physical chip %zu has invalid capacity",
                                product->name, i);
        if (!emu_product_chip_model_allowed(expected, actual->model))
            return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                "%s physical chip %zu does not permit model %s",
                                product->name, i, actual->model);
    }
    const char *identity_model =
        prepared->chips[prepared->identity_chip_index].model;
    emu_identity_kind_t expected_kind =
        identity_kind_for_engine(identity_model);
    if (prepared->identity_kind != expected_kind)
        return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "%s identity kind does not match owner model %s",
                            product->name, identity_model);
    if (product->primary_chip_index >= prepared->chip_count ||
        strcmp(prepared->metadata.flash_engine,
               prepared->chips[product->primary_chip_index].model))
        return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "%s primary model conflicts with image metadata",
                            product->name);
    product_ok(error);
    return EMU_OK;
}

emu_error_code_t emu_product_prepare_image(emu_prepared_session_t *prepared,
                                            const char *requested_device,
                                            emu_error_t *error) {
    if (!prepared || !prepared->source.bytes || !prepared->source.size ||
        prepared->chip_count || prepared->operation_count)
        return product_fail(error, EMU_ERR_ARGUMENT,
                            "image preparation requires one unprepared source");
    emu_metadata_instance_t instances[MAX_IMAGE_METADATA_INSTANCES];
    size_t instance_count = 0;
    emu_error_code_t rc = scan_metadata(prepared->source.bytes,
                                        prepared->source.size, instances,
                                        &instance_count, error);
    if (rc != EMU_OK) return rc;

    const emu_product_t *requested = NULL;
    if (requested_device) {
        requested = emu_product_by_name(requested_device);
        if (!requested)
            return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                "unknown device: %s", requested_device);
    }
    const emu_metadata_instance_t *selected = NULL;
    const emu_product_t *product = requested;
    size_t selected_count = 0;
    for (size_t i = 0; i < instance_count; i++) {
        const emu_product_t *metadata_product =
            emu_product_by_name(instances[i].metadata.model);
        if (requested && metadata_product && metadata_product != requested)
            return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                "image model %s conflicts with device override %s",
                                instances[i].metadata.model, requested->name);
        const emu_product_t *candidate = requested ? requested : metadata_product;
        if (!candidate ||
            instances[i].topology_size !=
                candidate->chips[candidate->primary_chip_index].size ||
            !emu_product_chip_model_allowed(
                &candidate->chips[candidate->primary_chip_index],
                instances[i].metadata.flash_engine))
            continue;
        if (selected && !same_metadata(&selected->metadata,
                                       &instances[i].metadata))
            return product_fail(
                error, EMU_ERR_INVALID_PREPARED_SESSION,
                "conflicting firmware metadata instances: %s SW%d %s %04X/%04X and %s SW%d %s %04X/%04X",
                selected->metadata.model, selected->metadata.software_version,
                selected->metadata.langpack,
                selected->metadata.flash_manufacturer_id,
                selected->metadata.flash_device_id,
                instances[i].metadata.model,
                instances[i].metadata.software_version,
                instances[i].metadata.langpack,
                instances[i].metadata.flash_manufacturer_id,
                instances[i].metadata.flash_device_id);
        if (!selected) { selected = &instances[i]; product = candidate; }
        selected_count++;
    }
    if (!selected) {
        if (!requested)
            return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                                "unsupported image model: %s",
                                instances[0].metadata.model);
        return product_fail(
            error, EMU_ERR_INVALID_PREPARED_SESSION,
            "image flash metadata does not match %s primary topology (%zu MiB)",
            requested->name,
            requested->chips[requested->primary_chip_index].size / MIB(1));
    }

    size_t physical_size = 0;
    for (size_t i = 0; i < product->chip_count; i++)
        physical_size += product->chips[i].size;
    if (physical_size != prepared->source.size)
        return product_fail(
            error, EMU_ERR_INVALID_PREPARED_SESSION,
            prepared->source.size > physical_size
                ? "flash image exceeds configured physical flash size"
                : "flash image is smaller than configured physical flash size");

    size_t primary_offsets[EMU_MAX_CHIPS] = {0};
    size_t candidate_count = 1;
    if (product->chip_count == 2) {
        size_t secondary = product->primary_chip_index == 0 ? 1u : 0u;
        primary_offsets[1] = product->chips[secondary].size;
        candidate_count = 2;
    }
    size_t primary_offset = 0;
    size_t reset_count = 0;
    for (size_t i = 0; i < candidate_count; i++)
        if (reset_mapping_matches(product, prepared->source.bytes,
                                  prepared->source.size, primary_offsets[i])) {
            primary_offset = primary_offsets[i];
            reset_count++;
        }
    if (!reset_count)
        return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "missing flash-layout reset signature: expected bootable JMPS at logical address 0");
    if (reset_count != 1)
        return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "ambiguous flash layout: bootable reset JMPS occurs in multiple chip orders");

    size_t primary_size = product->chips[product->primary_chip_index].size;
    size_t expected_view = primary_offset +
        (primary_size > MIB(8) ? primary_size - MIB(8) : 0);
    const emu_metadata_instance_t *resolved = NULL;
    for (size_t i = 0; i < instance_count; i++)
        if (instances[i].topology_size == primary_size &&
            same_metadata(&instances[i].metadata, &selected->metadata) &&
            instances[i].metadata.metadata_view_offset == expected_view) {
            resolved = &instances[i];
            break;
        }
    if (!resolved)
        return product_fail(error, EMU_ERR_INVALID_PREPARED_SESSION,
                            "flash metadata conflicts with reset layout: expected metadata-view offset 0x%zX",
                            expected_view);

    prepared->metadata = resolved->metadata;
    prepared->metadata.metadata_instance_count = selected_count;
    snprintf(prepared->metadata.flash_file_order,
             sizeof prepared->metadata.flash_file_order, "%s",
             product->chip_count == 1 ? "single" :
             primary_offset == 0 ? "primary-first" : "secondary-first");
    snprintf(prepared->selected_device, sizeof prepared->selected_device,
             "%s", product->name);
    prepared->chip_count = product->chip_count;
    for (size_t i = 0; i < product->chip_count; i++) {
        emu_chip_view_t *chip = &prepared->chips[i];
        const emu_product_chip_t *catalog_chip = &product->chips[i];
        size_t offset;
        if (i == product->primary_chip_index) offset = primary_offset;
        else { offset = primary_offset ? 0 : primary_size; }
        snprintf(chip->role, sizeof chip->role, "%s", catalog_chip->name);
        snprintf(chip->model, sizeof chip->model, "%s",
                 i == product->primary_chip_index
                    ? resolved->metadata.flash_engine
                    : catalog_chip->default_model);
        chip->source_offset = offset;
        chip->size = catalog_chip->size;
        if (!offset && chip->size == prepared->source.size) {
            memcpy(chip->sha256, prepared->source.sha256,
                   sizeof chip->sha256);
            memcpy(chip->sha256_hex, prepared->source.sha256_hex,
                   sizeof chip->sha256_hex);
        } else {
            emu_sha256(prepared->source.bytes + offset, chip->size,
                       chip->sha256);
            emu_sha256_hex(chip->sha256, chip->sha256_hex);
        }
    }
    prepared->identity_kind = identity_kind_for_engine(
        resolved->metadata.flash_engine);
    prepared->identity_chip_index = product->identity_chip_index;
    return emu_product_validate_prepared_storage(prepared, error);
}
