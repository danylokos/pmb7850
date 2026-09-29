#include <stdio.h>
#include <string.h>

#include "am29lv.h"
#include "flash.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void unlock_command(const uint8_t *data, size_t len,
                           am29lv_state_t *st, uint8_t command) {
    cemu_am29lv_write8(data, len, st, 0xAAA, 0xAA, 1, NULL);
    /* The PMB firmware uses the odd byte address 0x555. Both byte addresses
     * select x16 word address 0x2AA after the external A0 lane is removed. */
    cemu_am29lv_write8(data, len, st, 0x555, 0x55, 1, NULL);
    cemu_am29lv_write8(data, len, st, 0xAAA, command, 1, NULL);
}

static void unlock_command_at(const uint8_t *data, size_t len,
                              am29lv_state_t *st, uint32_t off,
                              uint8_t command) {
    cemu_am29lv_write8(data, len, st, 0xAAA, 0xAA, 1, NULL);
    cemu_am29lv_write8(data, len, st, 0x555, 0x55, 1, NULL);
    cemu_am29lv_write8(data, len, st, off, command, 1, NULL);
}

static void sector_erase(const uint8_t *data, size_t len,
                         am29lv_state_t *st, uint32_t sector) {
    unlock_command(data, len, st, 0x80);
    cemu_am29lv_write8(data, len, st, 0xAAA, 0xAA, 1, NULL);
    cemu_am29lv_write8(data, len, st, 0x555, 0x55, 1, NULL);
    cemu_am29lv_write8(data, len, st, sector, 0x30, 1, NULL);
}

static void test_autoselect_and_reset(void) {
    uint8_t data[0x1000];
    memset(data, 0xFF, sizeof data);
    peripheral_t p;
    am29lv_state_t st;
    cemu_am29lv128mh_periph_init(&p, &st);
    CHECK(st.config == &cemu_AM29LV128MH_CONFIG);
    cemu_am29lv_configure(&st, 0, 0x40000, 1);

    unlock_command(data, sizeof data, &st, 0x90);
    CHECK(st.mode == AM29_MODE_AUTOSELECT);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x00, 1, NULL) == 0x01);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x02, 1, NULL) == 0x7E);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x03, 1, NULL) == 0x22);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x1C, 1, NULL) == 0x12);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x1E, 1, NULL) == 0x00);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x1F, 1, NULL) == 0x22);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x04, 1, NULL) == 1);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x08, 1, NULL) == 1);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x40004, 1, NULL) == 0);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x06, 1, NULL) == 0x98);

    cemu_am29lv_write8(data, sizeof data, &st, 0, 0xF0, 1, NULL);
    CHECK(st.mode == AM29_MODE_ARRAY);
    CHECK(st.phase == AM29_PHASE_IDLE);

    cemu_am29lv_configure(&st, 0x800000, 0x40000, 1);
    cemu_am29lv_write8(data, sizeof data, &st, 0x800AAA, 0xAA, 1, NULL);
    cemu_am29lv_write8(data, sizeof data, &st, 0x800555, 0x55, 1, NULL);
    cemu_am29lv_write8(data, sizeof data, &st, 0x800AAA, 0x90, 1, NULL);
    CHECK(st.mode == AM29_MODE_AUTOSELECT);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x800008, 1, NULL) == 1);
    cemu_am29lv_state_free(&st);
}

static void test_write_buffer_program(void) {
    uint8_t data[0x40000];
    memset(data, 0xFF, sizeof data);
    peripheral_t p;
    am29lv_state_t st;
    am29lv_access_t access;
    cemu_am29lv128mh_periph_init(&p, &st);

    unlock_command_at(data, sizeof data, &st, 0x1F5D4, 0x25);
    CHECK(st.phase == AM29_PHASE_WRITE_BUFFER_COUNT);
    cemu_am29lv_write16(data, sizeof data, &st, 0x1F5D4, 2, 1, &access);
    CHECK(st.phase == AM29_PHASE_WRITE_BUFFER_DATA);
    CHECK(!strcmp(access.detail, "write-buffer-count"));
    cemu_am29lv_write16(data, sizeof data, &st, 0x1F5D4, 0x02FE, 1, NULL);
    cemu_am29lv_write16(data, sizeof data, &st, 0x1F5D6, 0x0024, 1, NULL);
    cemu_am29lv_write16(data, sizeof data, &st, 0x1F5D8, 0x7D74, 1, NULL);
    CHECK(st.phase == AM29_PHASE_WRITE_BUFFER_CONFIRM);
    CHECK(st.write_buffer_len == 3);
    CHECK(cemu_am29lv_read8(
              data, sizeof data, &st, 0x1F5D4, 1, NULL) == 0xFF);

    am29lv_state_t copy;
    memset(&copy, 0, sizeof copy);
    CHECK(cemu_am29lv_state_copy(&copy, &st));
    CHECK(copy.phase == AM29_PHASE_WRITE_BUFFER_CONFIRM);
    CHECK(copy.write_buffer_len == 3);
    cemu_am29lv_write8(data, sizeof data, &copy, 0x1F5D4, 0x29, 1, &access);
    CHECK(copy.phase == AM29_PHASE_IDLE);
    CHECK(copy.write_buffer_len == 0);
    CHECK(!strcmp(access.detail, "write-buffer-confirm"));
    CHECK(cemu_am29lv_read8(
              data, sizeof data, &copy, 0x1F5D4, 1, NULL) == 0xFE);
    CHECK(cemu_am29lv_read8(
              data, sizeof data, &copy, 0x1F5D5, 1, NULL) == 0x02);
    CHECK(cemu_am29lv_read8(
              data, sizeof data, &copy, 0x1F5D8, 1, NULL) == 0x74);

    unlock_command_at(data, sizeof data, &copy, 0x1F5D4, 0x25);
    cemu_am29lv_write16(data, sizeof data, &copy, 0x1F5D4, 0, 1, NULL);
    cemu_am29lv_write16(data, sizeof data, &copy, 0x1F5D4, 0xF0F0, 1, NULL);
    cemu_am29lv_write8(data, sizeof data, &copy, 0x1F5D4, 0x29, 1, NULL);
    CHECK(cemu_am29lv_read8(
              data, sizeof data, &copy, 0x1F5D4, 1, NULL) == 0xF0);
    CHECK(cemu_am29lv_read8(
              data, sizeof data, &copy, 0x1F5D5, 1, NULL) == 0x00);

    unlock_command_at(data, sizeof data, &copy, 0x1F5FE, 0x25);
    cemu_am29lv_write16(data, sizeof data, &copy, 0x1F5FE, 0, 1, NULL);
    cemu_am29lv_write16(data, sizeof data, &copy, 0x1F600, 0, 1, NULL);
    CHECK(copy.phase == AM29_PHASE_IDLE);
    CHECK(copy.write_buffer_len == 0);
    CHECK(cemu_am29lv_read8(
              data, sizeof data, &copy, 0x1F600, 1, NULL) == 0xFF);

    unlock_command_at(data, sizeof data, &copy, 0x10000, 0x25);
    cemu_am29lv_write16(data, sizeof data, &copy, 0x10000,
                        AM29LV_WRITE_BUFFER_WORDS, 1, &access);
    CHECK(copy.phase == AM29_PHASE_IDLE);
    CHECK(!strcmp(access.detail, "write-buffer-count-invalid"));

    cemu_am29lv_state_free(&copy);
    cemu_am29lv_state_free(&st);
}

static void test_program_and_persistence(void) {
    uint8_t data[0x2000];
    memset(data, 0xFF, sizeof data);
    peripheral_t p;
    am29lv_state_t st;
    cemu_am29lv128mh_periph_init(&p, &st);

    unlock_command(data, sizeof data, &st, 0xA0);
    cemu_am29lv_write16(data, sizeof data, &st, 0x100, 0x1234, 1, NULL);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x100, 1, NULL) == 0x34);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x101, 1, NULL) == 0x12);
    unlock_command(data, sizeof data, &st, 0xA0);
    cemu_am29lv_write16(data, sizeof data, &st, 0x100, 0xF0F0, 1, NULL);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x100, 1, NULL) == 0x30);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x101, 1, NULL) == 0x10);
    cemu_am29lv_reset(&st);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0x100, 1, NULL) == 0x30);

    am29lv_state_t copy;
    memset(&copy, 0, sizeof copy);
    CHECK(cemu_am29lv_state_copy(&copy, &st));
    CHECK(cemu_am29lv_read8(data, sizeof data, &copy, 0x101, 1, NULL) == 0x10);
    cemu_am29lv_state_free(&copy);
    cemu_am29lv_state_free(&st);
}

static void test_exact_initial_seeding(void) {
    uint8_t data[0x200];
    memset(data, 0x00, sizeof data);
    peripheral_t p;
    am29lv_state_t st;
    cemu_am29lv128mh_periph_init(&p, &st);

    static const uint8_t seeded[] = {0xFF, 0x5A};
    CHECK(cemu_am29lv_seed_bytes(data, sizeof data, &st, 0x100,
                                 seeded, sizeof seeded));
    CHECK(cemu_am29lv_array_read8(data, sizeof data, &st, 0x100) == 0xFF);
    CHECK(cemu_am29lv_array_read8(data, sizeof data, &st, 0x101) == 0x5A);
    CHECK(st.store.count == 2);

    unlock_command(data, sizeof data, &st, 0xA0);
    cemu_am29lv_write8(data, sizeof data, &st, 0x100, 0x0F, 1, NULL);
    CHECK(cemu_am29lv_array_read8(data, sizeof data, &st, 0x100) == 0x0F);
    unlock_command(data, sizeof data, &st, 0xA0);
    cemu_am29lv_write8(data, sizeof data, &st, 0x100, 0xFF, 1, NULL);
    CHECK(cemu_am29lv_array_read8(data, sizeof data, &st, 0x100) == 0x0F);

    cemu_am29lv_state_free(&st);
}

static void test_sector_erase_status_geometry_and_scheduler(void) {
    uint8_t data[2 * AM29LV_SECTOR_SIZE];
    memset(data, 0x00, sizeof data);
    peripheral_t p;
    flash_state_t flash;
    CHECK(cemu_flash_periph_init(&p, &flash, "am29lv128mh"));
    am29lv_state_t *st = &flash.u.am29;
    static const uint8_t patches[] = {0x12, 0x34};
    CHECK(cemu_am29lv_seed_bytes(data, sizeof data, st,
                                 AM29LV_SECTOR_SIZE + 0x80,
                                 patches, sizeof patches));

    sector_erase(data, sizeof data, st, AM29LV_SECTOR_SIZE);
    CHECK(st->erase_active);
    CHECK(st->erase_sector == AM29LV_SECTOR_SIZE);
    CHECK(p.next_event_ticks(&p, NULL) == AM29LV_ERASE_TIMER_TICKS);

    am29lv_access_t access;
    uint8_t first = cemu_am29lv_read8(
        data, sizeof data, st, AM29LV_SECTOR_SIZE, 1, &access);
    (void)cemu_am29lv_read8(
        data, sizeof data, st, AM29LV_SECTOR_SIZE + 1u, 1, NULL);
    uint8_t second = cemu_am29lv_read8(
        data, sizeof data, st, AM29LV_SECTOR_SIZE, 1, NULL);
    CHECK(!strcmp(access.subtype, "status"));
    CHECK((first ^ second) == 0x44u);
    CHECK((first & 0xA8u) == 0);
    int toggle = st->erase_toggle;
    (void)cemu_am29lv_peek8(data, sizeof data, st,
                            AM29LV_SECTOR_SIZE, 1);
    CHECK(st->erase_toggle == toggle);

    cemu_am29lv_write8(data, sizeof data, st,
                        AM29LV_SECTOR_SIZE, 0xF0, 1, &access);
    CHECK(st->erase_active);
    CHECK(!strcmp(access.subtype, "write-absorbed"));

    p.advance_quiet(&p, NULL, AM29LV_ERASE_TIMER_TICKS - 1u);
    CHECK(p.next_event_ticks(&p, NULL) == 1);
    CHECK(cemu_am29lv_tick(st, 1) == AM29_TICK_ERASE_TIMER);
    CHECK((cemu_am29lv_read8(data, sizeof data, st,
                             AM29LV_SECTOR_SIZE, 1, NULL) & 0x08u) != 0);
    CHECK(p.next_event_ticks(&p, NULL) ==
          AM29LV_SECTOR_ERASE_TICKS - AM29LV_ERASE_TIMER_TICKS);
    p.advance_quiet(&p, NULL,
        AM29LV_SECTOR_ERASE_TICKS - AM29LV_ERASE_TIMER_TICKS - 1u);
    CHECK(cemu_am29lv_tick(st, 1) == AM29_TICK_ERASE_COMPLETE);
    CHECK(!st->erase_active);
    CHECK(cemu_am29lv_array_read8(data, sizeof data, st,
                                  AM29LV_SECTOR_SIZE + 0x80) == 0xFF);
    CHECK(cemu_am29lv_array_read8(data, sizeof data, st, 0x80) == 0x00);
    CHECK(st->store.count == 0);

    unlock_command(data, sizeof data, st, 0xA0);
    cemu_am29lv_write16(data, sizeof data, st,
                        AM29LV_SECTOR_SIZE + 0x80, 0x1234, 1, NULL);
    CHECK(cemu_am29lv_array_read8(data, sizeof data, st,
                                  AM29LV_SECTOR_SIZE + 0x80) == 0x34);

    sector_erase(data, sizeof data, st, 0);
    CHECK(st->erase_active);
    cemu_flash_reset(&flash);
    CHECK(!st->erase_active);
    CHECK(cemu_am29lv_array_read8(data, sizeof data, st, 0x80) == 0x00);
    cemu_flash_state_free(&flash);
}

static void test_sector_erase_suspend_resume(void) {
    uint8_t data[2 * AM29LV_SECTOR_SIZE];
    memset(data, 0x00, sizeof data);
    data[0x80] = 0x5A;
    peripheral_t p;
    flash_state_t flash;
    am29lv_access_t access;
    CHECK(cemu_flash_periph_init(&p, &flash, "am29lv128mh"));
    am29lv_state_t *st = &flash.u.am29;

    sector_erase(data, sizeof data, st, AM29LV_SECTOR_SIZE);
    cemu_am29lv_tick(st, 123);
    uint32_t remaining = st->erase_ticks_remaining;
    uint32_t commands = st->cmd_writes;
    cemu_am29lv_write8(data, sizeof data, st, 0, 0xB0, 1, &access);
    CHECK(st->erase_active);
    CHECK(st->erase_suspended);
    CHECK(st->erase_timer_ticks_remaining == 0);
    CHECK(st->erase_ticks_remaining == remaining);
    CHECK(st->cmd_writes == commands + 1u);
    CHECK(!strcmp(access.subtype, "cmd"));
    CHECK(!strcmp(access.detail, "erase-suspend"));
    CHECK(p.next_event_ticks(&p, NULL) == UINT64_MAX);

    /* Non-erasing sectors return array data during suspension. The selected
     * sector keeps DQ6 stable while DQ2 toggles on successive status reads. */
    CHECK(cemu_am29lv_read8(data, sizeof data, st, 0x80, 1, NULL) == 0x5A);
    uint8_t first = cemu_am29lv_read8(
        data, sizeof data, st, AM29LV_SECTOR_SIZE, 1, &access);
    uint8_t second = cemu_am29lv_read8(
        data, sizeof data, st, AM29LV_SECTOR_SIZE, 1, NULL);
    CHECK(!strcmp(access.subtype, "status"));
    CHECK((first ^ second) == 0x04u);
    CHECK(((first & second) & 0x80u) != 0);
    CHECK(((first | second) & 0x40u) == 0);
    CHECK(((first & second) & 0x08u) != 0);
    int toggle = st->erase_toggle;
    (void)cemu_am29lv_peek8(data, sizeof data, st,
                            AM29LV_SECTOR_SIZE, 1);
    CHECK(st->erase_toggle == toggle);

    unlock_command(data, sizeof data, st, 0x90);
    CHECK(st->mode == AM29_MODE_AUTOSELECT);
    CHECK(cemu_am29lv_read8(data, sizeof data, st, 0, 1, NULL) == 0x01);
    cemu_am29lv_write8(data, sizeof data, st, 0, 0xF0, 1, NULL);
    CHECK(st->mode == AM29_MODE_ARRAY);
    CHECK(st->erase_active && st->erase_suspended);

    unlock_command(data, sizeof data, st, 0x88);
    CHECK(st->mode == AM29_MODE_ARRAY);
    CHECK(st->erase_active && st->erase_suspended);

    unlock_command(data, sizeof data, st, 0xA0);
    cemu_am29lv_write8(data, sizeof data, st, 0x80, 0x0F, 1, &access);
    CHECK(!strcmp(access.subtype, "program"));
    CHECK(cemu_am29lv_array_read8(data, sizeof data, st, 0x80) == 0x0A);

    unlock_command(data, sizeof data, st, 0xA0);
    cemu_am29lv_write8(data, sizeof data, st,
                       AM29LV_SECTOR_SIZE + 0x80, 0x0F, 1, &access);
    CHECK(!strcmp(access.subtype, "write-absorbed"));
    CHECK(!strcmp(access.detail, "program-in-suspended-erase-sector"));
    CHECK(cemu_am29lv_array_read8(data, sizeof data, st,
                                  AM29LV_SECTOR_SIZE + 0x80) == 0x00);

    cemu_am29lv_tick(st, 1000);
    p.advance_quiet(&p, NULL, 1000);
    CHECK(st->erase_ticks_remaining == remaining);
    cemu_am29lv_write8(data, sizeof data, st, 0, 0x30, 1, &access);
    CHECK(st->erase_suspended);
    CHECK(!strcmp(access.subtype, "write-absorbed"));
    CHECK(!strcmp(access.detail, "erase-resume-wrong-sector"));

    am29lv_state_t copy;
    memset(&copy, 0, sizeof copy);
    CHECK(cemu_am29lv_state_copy(&copy, st));
    CHECK(copy.erase_active && copy.erase_suspended);
    CHECK(copy.erase_ticks_remaining == remaining);
    CHECK(cemu_am29lv_array_read8(data, sizeof data, &copy, 0x80) == 0x0A);
    cemu_am29lv_state_free(&copy);

    cemu_am29lv_write8(data, sizeof data, st,
                       AM29LV_SECTOR_SIZE, 0x30, 1, &access);
    CHECK(st->erase_active);
    CHECK(!st->erase_suspended);
    CHECK(st->erase_ticks_remaining == remaining);
    CHECK(!strcmp(access.subtype, "cmd"));
    CHECK(!strcmp(access.detail, "erase-resume"));
    CHECK(p.next_event_ticks(&p, NULL) == remaining);
    CHECK(cemu_am29lv_tick(st, remaining - 1u) == AM29_TICK_NO_EVENT);
    CHECK(cemu_am29lv_tick(st, 1) == AM29_TICK_ERASE_COMPLETE);
    CHECK(!st->erase_active && !st->erase_suspended);
    CHECK(cemu_am29lv_array_read8(data, sizeof data, st,
                                  AM29LV_SECTOR_SIZE) == 0xFF);
    cemu_flash_state_free(&flash);
}

static void test_sector_erase_rejects_protected_and_bad_confirm(void) {
    uint8_t data[2 * AM29LV_SECTOR_SIZE];
    memset(data, 0x00, sizeof data);
    peripheral_t p;
    am29lv_state_t st;
    am29lv_access_t access;
    cemu_am29lv128mh_periph_init(&p, &st);
    cemu_am29lv_configure(&st, AM29LV_SECTOR_SIZE,
                          AM29LV_SECTOR_SIZE, 1);

    sector_erase(data, sizeof data, &st, AM29LV_SECTOR_SIZE);
    CHECK(!st.erase_active);
    CHECK(cemu_am29lv_array_read8(data, sizeof data, &st,
                                  AM29LV_SECTOR_SIZE) == 0x00);

    unlock_command(data, sizeof data, &st, 0x80);
    cemu_am29lv_write8(data, sizeof data, &st, 0xAAA, 0xAA, 1, NULL);
    cemu_am29lv_write8(data, sizeof data, &st, 0x555, 0x55, 1, NULL);
    cemu_am29lv_write8(data, sizeof data, &st, 0, 0x10, 1, &access);
    CHECK(!st.erase_active);
    CHECK(!strcmp(access.subtype, "write-absorbed"));
    CHECK(!strcmp(access.detail, "erase-confirm-unsupported"));
    cemu_am29lv_state_free(&st);
}

static void test_secsi_factory_lock(void) {
    uint8_t data[0x1000];
    uint8_t esn[AM29LV_ESN_SIZE];
    uint8_t customer[AM29LV_CUSTOMER_SIZE];
    memset(data, 0xFF, sizeof data);
    for (size_t i = 0; i < sizeof esn; i++) esn[i] = (uint8_t)(0xA0 + i);
    peripheral_t p;
    am29lv_state_t st;
    cemu_am29lv128mh_periph_init(&p, &st);
    cemu_am29lv_configure(&st, 0, 0x40000, 1);
    CHECK(cemu_am29lv_secsi_esn_parse(
              "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF", esn) == 0);
    CHECK(cemu_am29lv_secsi_esn_parse("A0A1", esn) != 0);
    CHECK(cemu_am29lv_secsi_customer_parse(
              "5302620027599540", customer) == 0);
    CHECK(customer[0] == 0x53 && customer[7] == 0x40);
    CHECK(cemu_am29lv_secsi_customer_parse("5302", customer) != 0);
    cemu_am29lv_secsi_esn_set(&st, esn);

    unlock_command(data, sizeof data, &st, 0x88);
    CHECK(st.mode == AM29_MODE_SECSI);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0, 1, NULL) == 0xA0);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 15, 1, NULL) == 0xAF);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 16, 1, NULL) == 0xFF);
    static const uint8_t factory_data[8] =
        {0x53,0x02,0x62,0x00,0x27,0x59,0x95,0x40};
    CHECK(cemu_am29lv_secsi_factory_set(
              &st, 0x10, factory_data, sizeof factory_data));
    CHECK(cemu_am29lv_read8(
              data, sizeof data, &st, 0x800010, 1, NULL) == 0x53);
    CHECK(cemu_am29lv_read8(
              data, sizeof data, &st, 0x800017, 1, NULL) == 0x40);
    CHECK(cemu_am29lv_read8(
              data, sizeof data, &st, 0x800100, 1, NULL) == 0xFF);

    unlock_command(data, sizeof data, &st, 0xA0);
    cemu_am29lv_write8(data, sizeof data, &st, 0, 0x00, 1, NULL);
    CHECK(cemu_am29lv_read8(data, sizeof data, &st, 0, 1, NULL) == 0xA0);
    cemu_am29lv_write8(data, sizeof data, &st, 0, 0x60, 1, NULL);
    cemu_am29lv_write8(data, sizeof data, &st, 0, 0x40, 1, NULL);
    CHECK(st.secsi_locked);

    unlock_command(data, sizeof data, &st, 0x90);
    CHECK(st.phase == AM29_PHASE_SECSI_EXIT_ZERO);
    cemu_am29lv_write8(data, sizeof data, &st, 0, 0x00, 1, NULL);
    CHECK(st.mode == AM29_MODE_ARRAY);
    cemu_am29lv_state_free(&st);
}

static void test_generic_dispatch(void) {
    uint8_t data[32];
    memset(data, 0xFF, sizeof data);
    peripheral_t p;
    flash_state_t st;
    CHECK(cemu_flash_periph_init(&p, &st, AM29LV128MH_MODEL));
    CHECK(st.kind == FLASH_MODEL_AM29LV);
    CHECK(st.u.am29.config == &cemu_AM29LV128MH_CONFIG);
    CHECK(!strcmp(cemu_flash_model_str(&st), AM29LV128MH_MODEL));
    cemu_flash_write8(data, sizeof data, &st, 0xAAA, 0xAA, 1, NULL);
    CHECK(st.u.am29.phase == AM29_PHASE_EXPECT_55);
    cemu_flash_reset(&st);
    CHECK(st.u.am29.phase == AM29_PHASE_IDLE);
    cemu_flash_state_free(&st);
}

int main(void) {
    test_autoselect_and_reset();
    test_program_and_persistence();
    test_exact_initial_seeding();
    test_write_buffer_program();
    test_sector_erase_status_geometry_and_scheduler();
    test_sector_erase_suspend_resume();
    test_sector_erase_rejects_protected_and_bad_confirm();
    test_secsi_factory_lock();
    test_generic_dispatch();
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
