/* Memory-controller ownership, routing, endpoint, and observability contract. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc.h"
#include "synth.h"

#define FLEN (8u * 1024u * 1024u)
#define TEST_XIC 0xFF6Eu

static int g_fail;
static int g_total_fail;
static int g_total_run;

#define CHECK(cond) do { \
    if (!(cond)) { \
        g_fail++; \
        printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static uint8_t *flash;
static soc_t soc;

static void setup(void) {
    if (!flash) {
        flash = malloc(FLEN);
        memset(flash, 0xFF, FLEN);
        flash[0] = 0xFA;
        flash[1] = 0x80;
        flash[2] = 0xC4;
        flash[3] = 0x2F;
        flash[0x1234] = 0x5A;
    }
    device_config_t cfg = *cemu_device_by_name("c55");
    cemu_soc_init(&soc, flash, FLEN, &cfg, synth_defaults(), 0);
}

static void test_owns_storage_and_cpu_bus_endpoint(void) {
    setup();
    memory_controller_t *mc = &soc.memory;
    CHECK(mc->soc == &soc);
    CHECK(mc->ram != NULL && mc->present != NULL);
    CHECK(mc->lm != NULL && mc->lm_size == LM_SIZE);
    CHECK(mc->flash_data == flash && mc->flash_len == FLEN);
    CHECK(mc->n_flash_chips == 1 && mc->flash_endpoints[0] != NULL);
    CHECK(soc.bus.ctx == mc);
    cemu_soc_free(&soc);
}

static void test_sfr_mutation_and_debug_poke_resynchronize_interrupts(void) {
    setup();
    interrupt_request_t request;
    CHECK(!cemu_interrupt_subsystem_pending(&soc.interrupts, &request));
    uint64_t generation = soc.interrupts.generation;

    cemu_memory_controller_sfr_put(
        &soc.memory, TEST_XIC, XIC_IR_BIT | XIC_IE_BIT | (6u << 2));
    CHECK(soc.interrupts.generation == generation + 1);
    CHECK(cemu_interrupt_subsystem_pending(&soc.interrupts, &request));
    CHECK(request.ic_addr == TEST_XIC && request.ilvl == 6);

    cemu_memory_controller_poke16(&soc.memory, TEST_XIC, 0);
    CHECK(!cemu_interrupt_subsystem_pending(&soc.interrupts, &request));
    cemu_soc_free(&soc);
}

static void test_unmapped_write_creates_ram_and_peeks_are_event_free(void) {
    setup();
    cemu_memory_controller_enable_stats(&soc.memory);
    cemu_event_set_statistics(&soc.instrumentation, 1);

    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x700000) == 0xFF);
    cemu_memory_controller_poke16(&soc.memory, 0x700000, 0xBEEF);
    CHECK(memory_controller_ram_present(&soc.memory, 0x700000));
    CHECK(cemu_memory_controller_peek16(&soc.memory, 0x700000) == 0xBEEF);
    CHECK(soc.memory.mem_write_seq == 0);

    bus_write16(&soc.bus, 0xE102, 0x1234);
    CHECK(soc.memory.mem_write_seq == 1);
    uint64_t writes = soc.memory.mem_write_seq;
    CHECK(cemu_memory_controller_peek16(&soc.memory, 0xE102) == 0x1234);
    CHECK(soc.memory.mem_write_seq == writes);
    cemu_soc_free(&soc);
}

static void test_local_memory_aliases_follow_syscon(void) {
    setup();
    cemu_memory_controller_sfr_put(&soc.memory, SYSCON_ADDR, SYSCON_ROMEN);
    cemu_memory_controller_poke8(&soc.memory, 0x001234, 0xA5);
    cemu_memory_controller_poke8(&soc.memory, 0x019234, 0xB6);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x001234) == 0xA5);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x011234) == 0xFF);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x019234) == 0xB6);

    cemu_memory_controller_sfr_put(
        &soc.memory, SYSCON_ADDR, SYSCON_ROMEN | SYSCON_ROMS1);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x001234) == flash[0x1234]);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x011234) == 0xA5);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x019234) == 0xB6);
    cemu_soc_free(&soc);
}

static void test_flash_translation_mirrors_and_command_visibility(void) {
    setup();
    memory_flash_target_t low = {0};
    memory_flash_target_t native = {0};
    CHECK(cemu_memory_controller_flash_translate(
        &soc.memory, 0x001234, &low));
    CHECK(cemu_memory_controller_flash_translate(
        &soc.memory, 0x801234, &native));
    CHECK(low.chip_index == 0 && native.chip_index == 0);
    CHECK(low.chip_offset == 0x1234 && native.chip_offset == 0x1234);
    CHECK(!low.command_visible && native.command_visible);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x001234) == 0x5A);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x801234) == 0x5A);

    flash_state_t *state = cemu_memory_controller_flash_state(&soc.memory, 0);
    CHECK(!strcmp(cemu_flash_mode_str(state), "array"));
    bus_write8(&soc.bus, 0x000000, 0x90);
    CHECK(!strcmp(cemu_flash_mode_str(state), "array"));
    bus_write8(&soc.bus, 0x800000, 0x90);
    CHECK(!strcmp(cemu_flash_mode_str(state), "id"));
    cemu_soc_free(&soc);
}

static void test_dual_flash_windows_keep_chip_state_isolated(void) {
    const size_t length = 12u * 1024u * 1024u;
    uint8_t *image = malloc(length);
    CHECK(image != NULL);
    if (!image) return;
    memset(image, 0xFF, length);
    memcpy(image + 0x400000, (uint8_t[]){0xFA, 0x80, 0x2C, 0x29}, 4);
    device_config_t cfg = *cemu_device_by_name("s55");
    cfg.flash_image = (device_flash_image_mapping_t){
        .file_offsets = {0x400000, 0}, .count = 2};
    CHECK(!strcmp(cfg.name, "s55"));
    cemu_soc_init(&soc, image, length, &cfg, synth_defaults(), 0);

    memory_flash_target_t target = {0};
    CHECK(cemu_memory_controller_flash_translate(
        &soc.memory, 0x000000, &target));
    CHECK(target.chip_index == 0 && !target.command_visible);
    CHECK(cemu_memory_controller_flash_translate(
        &soc.memory, 0x400000, &target));
    CHECK(target.chip_index == 1 && target.command_visible);
    CHECK(cemu_memory_controller_flash_translate(
        &soc.memory, 0x800000, &target));
    CHECK(target.chip_index == 0 && target.command_visible);

    flash_state_t *secondary =
        cemu_memory_controller_flash_state(&soc.memory, 1);
    flash_state_t *primary =
        cemu_memory_controller_flash_state(&soc.memory, 0);
    bus_write8(&soc.bus, 0x400000, 0x90);
    CHECK(!strcmp(cemu_flash_mode_str(secondary), "id"));
    CHECK(!strcmp(cemu_flash_mode_str(primary), "array"));
    bus_write8(&soc.bus, 0x800000, 0x70);
    CHECK(!strcmp(cemu_flash_mode_str(secondary), "id"));
    CHECK(!strcmp(cemu_flash_mode_str(primary), "status"));

    cemu_soc_free(&soc);
    free(image);
}

static void test_a60_a62_flash_aliases_and_addrsel1_ram(void) {
    const size_t length = 8u * 1024u * 1024u;
    uint8_t *image = malloc(length);
    CHECK(image != NULL);
    if (!image) return;
    memset(image, 0xFF, length);
    memcpy(image, (uint8_t[]){0xFA,0x80,0x46,0x32}, 4);
    image[0x001234] = 0x5A;

    static const char *devices[] = {"a60", "a62"};
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++) {
        const device_config_t *cfg = cemu_device_by_name(devices[i]);
        cemu_soc_init(&soc, image, length, cfg, synth_defaults(), 0);
        memory_flash_target_t low = {0};
        memory_flash_target_t high = {0};
        CHECK(cemu_memory_controller_flash_translate(&soc.memory, 0x001234, &low));
        CHECK(cemu_memory_controller_flash_translate(&soc.memory, 0x801234, &high));
        CHECK(low.chip_index == 0 && high.chip_index == 0);
        CHECK(low.chip_offset == 0x1234 && high.chip_offset == 0x1234);
        CHECK(low.command_visible && high.command_visible);
        CHECK(cemu_memory_controller_peek8(&soc.memory, 0x001234) == 0x5A);
        CHECK(cemu_memory_controller_peek8(&soc.memory, 0x801234) == 0x5A);

        external_ram_state_t *ram =
            cemu_memory_controller_external_ram_state(&soc.memory, 0);
        CHECK(ram != NULL && ram->addrsel_index == 1);
        CHECK(ram != NULL && ram->chip_size == 0x200000);
        bus_write16(&soc.bus, ADDRSEL1_ADDR, 0x2009);
        bus_write16(&soc.bus, BUSCON1_ADDR, BUSCON_BUSACT);
        bus_write8(&soc.bus, 0x234567, 0x6C);
        CHECK(ram != NULL && ram->bytes[0x34567] == 0x6C);
        CHECK(cemu_memory_controller_peek8(&soc.memory, 0x234567) == 0x6C);
        bus_write8(&soc.bus, 0x334567, 0xA5);
        CHECK(ram != NULL && ram->bytes[0x134567] == 0xA5);
        CHECK(cemu_memory_controller_peek8(&soc.memory, 0x234567) == 0x6C);
        CHECK(cemu_memory_controller_peek8(&soc.memory, 0x334567) == 0xA5);

        cemu_soc_free(&soc);
    }
    free(image);
}

static void test_a65_linear_flash_and_addrsel1_ram(void) {
    const size_t length = 16u * 1024u * 1024u;
    uint8_t *image = malloc(length);
    CHECK(image != NULL);
    if (!image) return;
    memset(image, 0xFF, length);
    memcpy(image, (uint8_t[]){0xFA,0x00,0x00,0x08}, 4);
    image[0x001234] = 0x5A;
    image[0x801234] = 0xA5;

    const device_config_t *cfg = cemu_device_by_name("a65");
    cemu_soc_init(&soc, image, length, cfg, synth_defaults(), 0);
    memory_flash_target_t low = {0};
    memory_flash_target_t high = {0};
    CHECK(cemu_memory_controller_flash_translate(&soc.memory, 0x001234, &low));
    CHECK(cemu_memory_controller_flash_translate(&soc.memory, 0x801234, &high));
    CHECK(low.chip_index == 0 && high.chip_index == 0);
    CHECK(low.chip_offset == 0x001234);
    CHECK(high.chip_offset == 0x801234);
    CHECK(low.command_visible && high.command_visible);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x001234) == 0x5A);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x801234) == 0xA5);

    external_ram_state_t *ram =
        cemu_memory_controller_external_ram_state(&soc.memory, 0);
    CHECK(ram != NULL && ram->addrsel_index == 1);
    CHECK(ram != NULL && ram->chip_size == 0x200000);
    bus_write16(&soc.bus, ADDRSEL1_ADDR, 0x2009);
    bus_write16(&soc.bus, BUSCON1_ADDR, BUSCON_BUSACT);
    bus_write8(&soc.bus, 0x234567, 0x6C);
    bus_write8(&soc.bus, 0x334567, 0xA5);
    CHECK(ram != NULL && ram->bytes[0x34567] == 0x6C);
    CHECK(ram != NULL && ram->bytes[0x134567] == 0xA5);

    cemu_soc_free(&soc);
    free(image);
}

static void test_cf62_linear_flash_and_addrsel1_ram(void) {
    const size_t length = 16u * 1024u * 1024u;
    uint8_t *image = malloc(length);
    CHECK(image != NULL);
    if (!image) return;
    memset(image, 0xFF, length);
    memcpy(image, (uint8_t[]){0xFA,0x00,0x00,0x08}, 4);
    image[0x001234] = 0x5A;
    image[0x801234] = 0xA5;

    const device_config_t *cfg = cemu_device_by_name("cf62");
    cemu_soc_init(&soc, image, length, cfg, synth_defaults(), 0);

    memory_flash_target_t low = {0};
    memory_flash_target_t high = {0};
    CHECK(cemu_memory_controller_flash_translate(&soc.memory, 0x001234, &low));
    CHECK(cemu_memory_controller_flash_translate(&soc.memory, 0x801234, &high));
    CHECK(low.chip_index == 0 && low.chip_offset == 0x001234);
    CHECK(high.chip_index == 0 && high.chip_offset == 0x801234);
    CHECK(low.command_visible && high.command_visible);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x001234) == 0x5A);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x801234) == 0xA5);

    external_ram_state_t *ram =
        cemu_memory_controller_external_ram_state(&soc.memory, 0);
    CHECK(ram != NULL && ram->addrsel_index == 1);
    CHECK(ram != NULL && ram->chip_size == 0x200000);
    bus_write16(&soc.bus, ADDRSEL1_ADDR, 0x2009);
    bus_write16(&soc.bus, BUSCON1_ADDR, 0x05AE);
    bus_write8(&soc.bus, 0x200123, 0x6C);
    bus_write8(&soc.bus, 0x300123, 0xA5);
    CHECK(ram != NULL && ram->bytes[0x000123] == 0x6C);
    CHECK(ram != NULL && ram->bytes[0x100123] == 0xA5);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x200123) == 0x6C);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x300123) == 0xA5);

    cemu_soc_free(&soc);
    free(image);
}

typedef struct {
    int sfr_writes;
    int byte_writes;
} endpoint_state_t;

static uint16_t endpoint_read_sfr(
    peripheral_t *self, soc_t *owner, uint32_t addr, uint16_t stored) {
    (void)self;
    (void)owner;
    (void)addr;
    return (uint16_t)(stored ^ 0x00FFu);
}

static void endpoint_write_sfr(
    peripheral_t *self, soc_t *owner, uint32_t addr, uint16_t stored) {
    (void)owner;
    (void)addr;
    (void)stored;
    ((endpoint_state_t *)self->state)->sfr_writes++;
}

static int endpoint_read8(
    peripheral_t *self, soc_t *owner, uint32_t addr) {
    (void)self;
    (void)owner;
    (void)addr;
    return 0xA5;
}

static int endpoint_peek8(
    peripheral_t *self, soc_t *owner, uint32_t addr) {
    (void)self;
    (void)owner;
    (void)addr;
    return 0x5A;
}

static int endpoint_write8(
    peripheral_t *self, soc_t *owner, uint32_t addr, uint8_t value) {
    (void)owner;
    (void)addr;
    (void)value;
    ((endpoint_state_t *)self->state)->byte_writes++;
    return 1;
}

static void test_private_endpoint_registration_and_dispatch(void) {
    soc_t owner;
    device_config_t cfg;
    endpoint_state_t state = {0};
    const uint32_t sfr_words[] = {0xF130};
    const addr_range_t byte_ranges[] = {{0xE100, 0xE100}};
    peripheral_t endpoint = {
        .id = "test-memory-endpoint",
        .state = &state,
        .sfr_words = sfr_words,
        .n_sfr_words = 1,
        .byte_ranges = byte_ranges,
        .n_byte_ranges = 1,
        .read8 = endpoint_read8,
        .peek8 = endpoint_peek8,
        .write8 = endpoint_write8,
        .read_sfr_word = endpoint_read_sfr,
        .on_sfr_write = endpoint_write_sfr,
    };

    memset(&owner, 0, sizeof owner);
    memset(&cfg, 0, sizeof cfg);
    cemu_event_hub_init(&owner.instrumentation);
    cemu_memory_controller_init(&owner.memory, &owner, NULL, 0, &cfg);
    cemu_interrupt_subsystem_init(&owner.interrupts, &owner);
    cemu_memory_controller_register_endpoint(&owner.memory, &endpoint);

    cemu_memory_controller_sfr_put(&owner.memory, 0xF130, 0x1200);
    bus_transaction_t txn = {
        .kind = BUS_ACCESS_READ, .addr = 0xF130, .size = 2,
    };
    cemu_memory_controller_access(&owner.memory, &txn);
    CHECK(txn.value == 0x12FF);
    txn.kind = BUS_ACCESS_WRITE;
    txn.value = 0xABCD;
    cemu_memory_controller_access(&owner.memory, &txn);
    CHECK(state.sfr_writes == 1);

    txn.kind = BUS_ACCESS_READ;
    txn.addr = 0xE100;
    txn.size = 1;
    cemu_memory_controller_access(&owner.memory, &txn);
    CHECK(txn.value == 0xA5);
    CHECK(cemu_memory_controller_peek8(&owner.memory, 0xE100) == 0x5A);
    txn.kind = BUS_ACCESS_WRITE;
    txn.value = 0x77;
    cemu_memory_controller_access(&owner.memory, &txn);
    CHECK(state.byte_writes == 1);
    CHECK(memory_controller_ram_get(&owner.memory, 0xE100) == 0x77);

    cemu_memory_controller_free(&owner.memory);
}

static void test_addrsel_external_ram_and_bus_priority(void) {
    setup();
    external_ram_state_t *ram =
        cemu_memory_controller_external_ram_state(&soc.memory, 0);
    CHECK(ram != NULL && ram->addrsel_index == 2);
    bus_write16(&soc.bus, ADDRSEL1_ADDR + 2, 0x2009);
    bus_write16(&soc.bus, BUSCON1_ADDR + 2, BUSCON_BUSACT);
    bus_write8(&soc.bus, 0x234567, 0x6C);
    CHECK(ram->bytes[0x34567] == 0x6C);
    CHECK(cemu_memory_controller_peek8(&soc.memory, 0x234567) == 0x6C);

    bus_write16(&soc.bus, ADDRSEL1_ADDR, 0x2009);
    bus_write16(&soc.bus, BUSCON1_ADDR, BUSCON_BUSACT);
    memory_bus_window_info_t route;
    CHECK(cemu_memory_controller_bus_route_decode(
        &soc.memory, 0x234567, &route));
    CHECK(route.kind == MEMORY_BUS_WINDOW_ADDRSEL && route.index == 2);

    bus_write16(&soc.bus, XADRS1_ADDR + 8, 0x2009);
    bus_write16(&soc.bus, XBCON1_ADDR + 8, BUSCON_BUSACT);
    CHECK(cemu_memory_controller_bus_route_decode(
        &soc.memory, 0x234567, &route));
    CHECK(route.kind == MEMORY_BUS_WINDOW_XBUS && route.index == 5);
    cemu_soc_free(&soc);
}

static void test_bus_window_decode_gates_and_reserved_ranges(void) {
    setup();
    memory_bus_window_info_t window;
    cemu_memory_controller_sfr_put(&soc.memory, XADRS1_ADDR, 0x0EF0);
    cemu_memory_controller_sfr_put(&soc.memory, XBCON1_ADDR, BUSCON_BUSACT);
    CHECK(cemu_memory_controller_bus_window_decode(
        &soc.memory, MEMORY_BUS_WINDOW_XBUS, 1, &window));
    CHECK(window.configured && !window.active);
    CHECK(!strcmp(window.reason, "XPEN not set"));

    cemu_memory_controller_sfr_put(
        &soc.memory, SYSCON_ADDR, SYSCON_XPEN);
    cemu_memory_controller_sfr_put(&soc.memory, XPERCON_ADDR, 1);
    CHECK(cemu_memory_controller_bus_window_decode(
        &soc.memory, MEMORY_BUS_WINDOW_XBUS, 1, &window));
    CHECK(window.active && window.start == 0xEF00 &&
          window.end == 0xEFFF);

    cemu_memory_controller_sfr_put(&soc.memory, ADDRSEL1_ADDR, 0x000C);
    cemu_memory_controller_sfr_put(
        &soc.memory, BUSCON1_ADDR, BUSCON_BUSACT);
    CHECK(cemu_memory_controller_bus_window_decode(
        &soc.memory, MEMORY_BUS_WINDOW_ADDRSEL, 1, &window));
    CHECK(window.reserved && !window.active);
    CHECK(!strcmp(window.reason, "reserved"));
    cemu_soc_free(&soc);
}

typedef struct {
    const char *name;
    void (*fn)(void);
} test_entry_t;

static const test_entry_t TESTS[] = {
    {"owns_storage_and_cpu_bus_endpoint",
     test_owns_storage_and_cpu_bus_endpoint},
    {"sfr_mutation_and_debug_poke_resynchronize_interrupts",
     test_sfr_mutation_and_debug_poke_resynchronize_interrupts},
    {"unmapped_write_creates_ram_and_peeks_are_event_free",
     test_unmapped_write_creates_ram_and_peeks_are_event_free},
    {"local_memory_aliases_follow_syscon",
     test_local_memory_aliases_follow_syscon},
    {"flash_translation_mirrors_and_command_visibility",
     test_flash_translation_mirrors_and_command_visibility},
    {"dual_flash_windows_keep_chip_state_isolated",
     test_dual_flash_windows_keep_chip_state_isolated},
    {"a60_a62_flash_aliases_and_addrsel1_ram",
     test_a60_a62_flash_aliases_and_addrsel1_ram},
    {"a65_linear_flash_and_addrsel1_ram",
     test_a65_linear_flash_and_addrsel1_ram},
    {"cf62_linear_flash_and_addrsel1_ram",
     test_cf62_linear_flash_and_addrsel1_ram},
    {"private_endpoint_registration_and_dispatch",
     test_private_endpoint_registration_and_dispatch},
    {"addrsel_external_ram_and_bus_priority",
     test_addrsel_external_ram_and_bus_priority},
    {"bus_window_decode_gates_and_reserved_ranges",
     test_bus_window_decode_gates_and_reserved_ranges},
};

int main(void) {
    int count = (int)(sizeof TESTS / sizeof TESTS[0]);
    for (int i = 0; i < count; i++) {
        g_fail = 0;
        TESTS[i].fn();
        g_total_run++;
        if (g_fail) {
            g_total_fail++;
            printf("[FAIL] %s (%d)\n", TESTS[i].name, g_fail);
        }
    }
    free(flash);
    printf("\n%d tests, %d failed\n", g_total_run, g_total_fail);
    return g_total_fail ? 1 : 0;
}
