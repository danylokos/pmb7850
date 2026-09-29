#include <stdlib.h>
#include <string.h>

#include "test_support.h"
#include "emu_product.h"

#define MIB(n) ((size_t)(n) * 1024u * 1024u)

static void seed_metadata(uint8_t *image, size_t size, size_t view,
                          const char *model, uint8_t sw, uint8_t bcore,
                          uint16_t manufacturer, uint16_t device) {
    static const uint8_t fixed[] =
        {0xFF,0x0A,0x50,0x14,0x14,0x01,0x00,0xD5,0x21};
    size_t record = view + 0x7FF50u;
    EMU_CHECK(record + 0x40u <= size);
    image[record] = sw;
    memcpy(image + record + 1, fixed, sizeof fixed);
    memset(image + record + 10, 0, 2);
    memset(image + record + 12, 0xFF, 4);
    memset(image + record + 0x10, 0, 0x30);
    memcpy(image + record + 0x10, "lg1", 3);
    memcpy(image + record + 0x20, model, strlen(model));
    memcpy(image + record + 0x30, "SIEMENS", 7);
    image[view + 0x032C] = bcore;
    image[view + 0x7FE26] = (uint8_t)manufacturer;
    image[view + 0x7FE27] = (uint8_t)(manufacturer >> 8);
    image[view + 0x7FE28] = (uint8_t)device;
    image[view + 0x7FE29] = (uint8_t)(device >> 8);
}

static void seed_reset(uint8_t *image, size_t offset, size_t chip_size) {
    image[offset] = 0xFA;
    image[offset + 1] = chip_size == MIB(16) ? 0x00 : 0x80;
    image[offset + 2] = 0xC4;
    image[offset + 3] = 0x2F;
}

static void set_source(emu_prepared_session_t *prepared,
                       uint8_t *image, size_t size) {
    emu_prepared_init(prepared);
    prepared->source.locator = malloc(8);
    EMU_CHECK(prepared->source.locator != NULL);
    memcpy(prepared->source.locator, "memory:", 8);
    prepared->source.bytes = image;
    prepared->source.size = size;
    emu_sha256(image, size, prepared->source.sha256);
    emu_sha256_hex(prepared->source.sha256,
                   prepared->source.sha256_hex);
}

static void check_catalog(void) {
    static const char *const names[] = {
        "a52","a55","a60","a62","a65","c55",
        "c60","cf62","m55","mc60","s55","sl55"
    };
    EMU_CHECK(emu_product_count() == 12);
    for (size_t i = 0; i < 12; i++) {
        const emu_product_t *product = emu_product_at(i);
        EMU_CHECK(product != NULL && !strcmp(product->name, names[i]));
        EMU_CHECK(emu_product_by_name(product->image_model) == product);
        EMU_CHECK(product->chip_count >= 1 && product->chip_count <= 2);
        EMU_CHECK(product->primary_chip_index < product->chip_count);
        EMU_CHECK(product->identity_chip_index < product->chip_count);
        for (size_t j = 0; j < product->chip_count; j++) {
            const emu_product_chip_t *chip = &product->chips[j];
            EMU_CHECK(chip->role && chip->name && chip->default_model);
            EMU_CHECK(chip->allowed_model_count >= 1);
            EMU_CHECK(emu_product_chip_model_allowed(
                chip, chip->default_model));
        }
        EMU_CHECK(product->display.width && product->display.height);
        EMU_CHECK(product->capabilities & EMU_PRODUCT_KEYS);
        EMU_CHECK(product->capabilities & EMU_PRODUCT_DISPLAY);
        EMU_CHECK(product->capabilities & EMU_PRODUCT_SIM);
        EMU_CHECK(product->key_count == 18 || product->key_count == 20);
        EMU_CHECK(emu_product_key(product, "power") != NULL);
        EMU_CHECK(emu_product_key(product, "not-a-key") == NULL);
    }
    EMU_CHECK(emu_product_at(12) == NULL);
    EMU_CHECK(emu_product_by_name("C55") == emu_product_at(5));
    EMU_CHECK(emu_product_by_name("s56") == NULL);
    EMU_CHECK(emu_product_at(0)->display.width == 101);
    EMU_CHECK(emu_product_at(0)->display.height == 64);
    EMU_CHECK(emu_product_by_name("cf62")->display.width == 130);
    EMU_CHECK(emu_product_by_name("s55")->chip_count == 2);
    EMU_CHECK(emu_product_by_name("a52")->chips[0].size == MIB(4));
    EMU_CHECK(emu_product_by_name("a65")->chips[0].size == MIB(16));
    EMU_CHECK(!strcmp(emu_product_by_name("c60")->chips[0].default_model,
                      "am29lv128mh"));
    EMU_CHECK(emu_product_chip_model_allowed(
        &emu_product_by_name("a65")->chips[0], "am29lv128mh"));
    EMU_CHECK(emu_product_chip_model_allowed(
        &emu_product_by_name("a65")->chips[0], "w30-128mbit-top"));
    EMU_CHECK(emu_product_chip_model_allowed(
        &emu_product_by_name("cf62")->chips[0], "am29lv128mh"));
    EMU_CHECK(emu_product_chip_model_allowed(
        &emu_product_by_name("cf62")->chips[0], "w30-128mbit-top"));
    EMU_CHECK(emu_product_chip_model_allowed(
        &emu_product_by_name("c60")->chips[0], "am29lv128mh"));
    EMU_CHECK(emu_product_chip_model_allowed(
        &emu_product_by_name("c60")->chips[0], "w30-128mbit-top"));
    EMU_CHECK(!(emu_product_by_name("a55")->capabilities &
                EMU_PRODUCT_SERIAL));
    EMU_CHECK(emu_product_by_name("c55")->capabilities &
              EMU_PRODUCT_BATTERY);
}

typedef struct {
    const char *device;
    uint16_t manufacturer;
    uint16_t flash_id;
    const char *model;
} topology_case_t;

static void check_all_product_topologies(void) {
    static const topology_case_t cases[] = {
        {"a52", 0x0089, 0x0016, "m58lw064d"},
        {"a55", 0x0020, 0x0017, "m58lw064d"},
        {"a60", 0x0001, 0x220C, "am29lv640mh"},
        {"a62", 0x0001, 0x220C, "am29lv640mh"},
        {"a65", 0x0001, 0x2212, "am29lv128mh"},
        {"c55", 0x0020, 0x0017, "m58lw064d"},
        {"c60", 0x0001, 0x2212, "am29lv128mh"},
        {"c60", 0x0089, 0x8856, "w30-128mbit-top"},
        {"cf62", 0x0001, 0x2212, "am29lv128mh"},
        {"m55", 0x0001, 0x2212, "am29lv128mh"},
        {"mc60", 0x0001, 0x2212, "am29lv128mh"},
        {"s55", 0x0089, 0x8854, "w30-64mbit-top"},
        {"sl55", 0x0089, 0x8854, "w30-64mbit-top"},
        {"a65", 0x0089, 0x8856, "w30-128mbit-top"},
        {"cf62", 0x0089, 0x8856, "w30-128mbit-top"},
    };
    emu_error_t error = {0};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const topology_case_t *test = &cases[i];
        const emu_product_t *product = emu_product_by_name(test->device);
        EMU_CHECK(product != NULL);
        size_t source_size = 0;
        for (size_t j = 0; j < product->chip_count; j++)
            source_size += product->chips[j].size;
        uint8_t *image = calloc(source_size, 1);
        EMU_CHECK(image != NULL);
        size_t primary_size =
            product->chips[product->primary_chip_index].size;
        seed_reset(image, 0, primary_size);
        size_t metadata_view = primary_size > MIB(8)
                             ? primary_size - MIB(8) : 0;
        seed_metadata(image, source_size, metadata_view,
                      product->image_model, 0x12, 0x10,
                      test->manufacturer, test->flash_id);
        emu_prepared_session_t prepared;
        set_source(&prepared, image, source_size);
        EMU_CHECK(emu_product_prepare_image(
            &prepared, NULL, &error) == EMU_OK);
        EMU_CHECK(!strcmp(prepared.selected_device, test->device));
        EMU_CHECK(!strcmp(prepared.chips[product->primary_chip_index].model,
                          test->model));
        EMU_CHECK(emu_product_validate_prepared_storage(
            &prepared, &error) == EMU_OK);
        emu_prepared_free(&prepared);
    }

    uint8_t *image = calloc(MIB(16), 1);
    EMU_CHECK(image != NULL);
    seed_reset(image, 0, MIB(16));
    seed_metadata(image, MIB(16), MIB(8), "C60", 0x12, 0x10,
                  0x0001, 0x2212);
    emu_prepared_session_t prepared;
    set_source(&prepared, image, MIB(16));
    EMU_CHECK(emu_product_prepare_image(
        &prepared, NULL, &error) == EMU_OK);
    EMU_CHECK(!strcmp(prepared.chips[0].model, "am29lv128mh"));
    emu_prepared_free(&prepared);
}

static void check_single_and_errors(void) {
    emu_error_t error = {0};
    uint8_t *image = calloc(MIB(8), 1);
    EMU_CHECK(image != NULL);
    seed_reset(image, 0, MIB(8));
    seed_metadata(image, MIB(8), 0, "C55", 0x24, 0x18, 0x0020, 0x0017);
    emu_prepared_session_t prepared;
    set_source(&prepared, image, MIB(8));
    EMU_CHECK(emu_product_prepare_image(&prepared, NULL, &error) == EMU_OK);
    EMU_CHECK(!strcmp(prepared.selected_device, "c55"));
    EMU_CHECK(!strcmp(prepared.metadata.flash_engine, "m58lw064d"));
    EMU_CHECK(prepared.metadata.software_version_raw == 0x24);
    EMU_CHECK(prepared.metadata.software_version == 24);
    EMU_CHECK(prepared.metadata.bcore_software_version_raw == 0x18);
    EMU_CHECK(prepared.metadata.bcore_software_version == 18);
    EMU_CHECK(prepared.identity_kind == EMU_IDENTITY_FACTORY_UID);
    EMU_CHECK(prepared.chip_count == 1 && prepared.chips[0].size == MIB(8));
    EMU_CHECK(emu_prepared_validate(&prepared, 0, &error) == EMU_OK);
    emu_prepared_free(&prepared);

    image = calloc(MIB(8), 1);
    EMU_CHECK(image != NULL);
    seed_reset(image, 0, MIB(8));
    seed_metadata(image, MIB(8), 0, "C55", 0x24, 0x18, 0x0020, 0x0017);
    set_source(&prepared, image, MIB(8));
    EMU_CHECK(emu_product_prepare_image(&prepared, "a55", &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "conflicts with device override"));
    emu_prepared_free(&prepared);

    image = calloc(MIB(8), 1);
    EMU_CHECK(image != NULL);
    seed_reset(image, 0, MIB(8));
    seed_metadata(image, MIB(8), 0, "C55", 0x24, 0x18, 0x0020, 0x0017);
    seed_metadata(image, MIB(8), 0x10000, "C55", 0x25, 0x18,
                  0x0020, 0x0017);
    set_source(&prepared, image, MIB(8));
    EMU_CHECK(emu_product_prepare_image(&prepared, NULL, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "conflicting firmware metadata"));
    emu_prepared_free(&prepared);

    image = calloc(MIB(8), 1);
    EMU_CHECK(image != NULL);
    set_source(&prepared, image, MIB(8));
    EMU_CHECK(emu_product_prepare_image(&prepared, NULL, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "no valid Siemens firmware metadata"));
    emu_prepared_free(&prepared);

    image = calloc(MIB(8), 1);
    EMU_CHECK(image != NULL);
    seed_metadata(image, MIB(8), 0, "C55", 0x24, 0x18, 0x0020, 0x0017);
    set_source(&prepared, image, MIB(8));
    EMU_CHECK(emu_product_prepare_image(&prepared, NULL, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "missing flash-layout reset signature"));
    emu_prepared_free(&prepared);
}

static void check_dual_orders(void) {
    emu_error_t error = {0};
    for (size_t primary_offset = 0; primary_offset <= MIB(4);
         primary_offset += MIB(4)) {
        uint8_t *image = calloc(MIB(12), 1);
        EMU_CHECK(image != NULL);
        seed_reset(image, primary_offset, MIB(8));
        seed_metadata(image, MIB(12), primary_offset, "S55", 0x91, 0x12,
                      0x0020, 0x8810);
        emu_prepared_session_t prepared;
        set_source(&prepared, image, MIB(12));
        EMU_CHECK(emu_product_prepare_image(&prepared, NULL, &error) == EMU_OK);
        EMU_CHECK(prepared.chip_count == 2);
        EMU_CHECK(prepared.chips[0].source_offset == primary_offset);
        EMU_CHECK(prepared.chips[1].source_offset ==
                  (primary_offset ? 0 : MIB(8)));
        EMU_CHECK(!strcmp(prepared.metadata.flash_file_order,
                          primary_offset ? "secondary-first" : "primary-first"));
        emu_prepared_free(&prepared);
    }

    uint8_t *image = calloc(MIB(12), 1);
    EMU_CHECK(image != NULL);
    seed_reset(image, 0, MIB(8));
    seed_reset(image, MIB(4), MIB(8));
    seed_metadata(image, MIB(12), 0, "S55", 0x91, 0x12, 0x0020, 0x8810);
    emu_prepared_session_t prepared;
    set_source(&prepared, image, MIB(12));
    EMU_CHECK(emu_product_prepare_image(&prepared, NULL, &error) ==
              EMU_ERR_INVALID_PREPARED_SESSION);
    EMU_CHECK(strstr(error.message, "ambiguous flash layout"));
    emu_prepared_free(&prepared);

    image = calloc(MIB(12), 1);
    EMU_CHECK(image != NULL);
    seed_reset(image, 0, MIB(8));
    seed_metadata(image, MIB(12), 0, "S56", 0x06, 0x06, 0x0020, 0x8810);
    set_source(&prepared, image, MIB(12));
    EMU_CHECK(emu_product_prepare_image(&prepared, "s55", &error) == EMU_OK);
    EMU_CHECK(!strcmp(prepared.selected_device, "s55"));
    EMU_CHECK(!strcmp(prepared.metadata.model, "S56"));
    emu_prepared_free(&prepared);
}

int main(void) {
    check_catalog();
    check_all_product_topologies();
    check_single_and_errors();
    check_dual_orders();
    puts("host product catalog and selection: PASS");
    return 0;
}
