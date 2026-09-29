#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cemu_core.h"
#include "cemu_core_diagnostics.h"
#include "cemu_core_private.h"
#include "battery.h"
#include "bus.h"
#include "gsm_stub.h"
#include "keypad.h"
#include "lcd.h"
#include "sim.h"
#include "xbus_unknown1.h"
#include "emu_cemu_drcov_adapter.h"
#include "emu_cemu_trace_adapter.h"
#include "emu_trace_schema.h"

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while (0)

static uint8_t *make_flash(size_t size) {
    uint8_t *flash = malloc(size);
    if (!flash) return NULL;
    memset(flash, 0xFF, size);
    flash[0] = 0xFA; flash[1] = 0x80;
    flash[2] = 0x04; flash[3] = 0x00; /* jmps 0x80,0x0004 */
    flash[4] = 0xCC; flash[5] = 0x00; /* nop */
    flash[6] = 0x0D; flash[7] = 0xFE; /* unconditional self-loop */
    return flash;
}

static cemu_core_options_t options_for(uint8_t *flash, size_t size) {
    return (cemu_core_options_t){
        .source = flash,
        .source_size = size,
        .device = *cemu_device_by_name("c55"),
    };
}

static void test_create_run_stop_query_and_destroy(void) {
    size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    CHECK(flash != NULL);
    cemu_core_options_t options = options_for(flash, size);
    cemu_core_t *core = NULL;
    cemu_status_t status = cemu_core_create(&core, &options);
    CHECK(status.code == CEMU_STATUS_OK);
    CHECK(core != NULL);
    CHECK(cemu_core_private_soc(core)->memory.flash_data == flash);

    cemu_core_state_t state;
    status = cemu_core_query(core, &state);
    CHECK(status.code == CEMU_STATUS_OK);
    CHECK(state.pc == 0);
    CHECK(state.instruction_count == 0);

    cemu_run_result_t result;
    status = cemu_core_run_slice(core, 2, &result);
    CHECK(status.code == CEMU_STATUS_OK);
    CHECK(result.reason == CEMU_RUN_SLICE_LIMIT);
    CHECK(result.ticks == 2);
    CHECK(result.guest_instructions == 2);
    CHECK(result.instruction_count == 2);
    CHECK(result.pc == 0x800006u);

    CHECK(cemu_core_request_stop(core).code == CEMU_STATUS_OK);
    CHECK(cemu_core_query(core, &state).code == CEMU_STATUS_OK);
    CHECK(state.stop_requested);
    CHECK(cemu_core_run_slice(core, 3, &result).code == CEMU_STATUS_OK);
    CHECK(result.reason == CEMU_RUN_STOPPED);
    CHECK(result.ticks == 0);

    cemu_core_destroy(core);
    cemu_core_destroy(NULL);
    free(flash);
}

static void test_failed_create_clears_output(void) {
    size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    cemu_core_options_t options = options_for(flash, size);
    for (long fail_after = 0; fail_after <= 1; fail_after++) {
        cemu_core_t *core = (cemu_core_t *)(uintptr_t)1;
        cemu_test_fail_alloc_after(fail_after);
        cemu_status_t status = cemu_core_create(&core, &options);
        cemu_test_clear_alloc_failure();
        CHECK(status.code == CEMU_STATUS_ALLOCATION_FAILED);
        CHECK(core == NULL);
        cemu_core_destroy(core);
    }
    free(flash);
}

static void test_reset_is_transactional(void) {
    size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    cemu_core_options_t options = options_for(flash, size);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);

    cemu_run_result_t result;
    CHECK(cemu_core_run_slice(core, 2, &result).code == CEMU_STATUS_OK);
    cemu_core_state_t before, after;
    CHECK(cemu_core_query(core, &before).code == CEMU_STATUS_OK);

    cemu_test_fail_alloc_after(0);
    cemu_status_t status = cemu_core_reset(core);
    cemu_test_clear_alloc_failure();
    CHECK(status.code == CEMU_STATUS_ALLOCATION_FAILED);
    CHECK(cemu_core_query(core, &after).code == CEMU_STATUS_OK);
    CHECK(memcmp(&before, &after, sizeof before) == 0);
    CHECK(cemu_core_run_slice(core, 1, &result).code == CEMU_STATUS_OK);
    CHECK(result.instruction_count == before.instruction_count + 1);

    CHECK(cemu_core_reset(core).code == CEMU_STATUS_OK);
    CHECK(cemu_core_query(core, &after).code == CEMU_STATUS_OK);
    CHECK(after.instruction_count == 0);
    CHECK(after.total_guest_instructions == 0);
    CHECK(after.pc == 0);
    CHECK(!after.stop_requested);

    cemu_core_destroy(core);
    free(flash);
}

static void test_reset_cancels_mailbox(void) {
    size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    cemu_core_options_t options = options_for(flash, size);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);
    const unsigned modes[] = {0x1700, 0x1701, 0x1704};
    for (unsigned i = 0; i < 3; i++) {
        soc_t *soc = cemu_core_private_soc(core);
        bus_write16(&soc->bus, 0xec10, modes[i]);
        bus_write16(&soc->bus, 0xec12, 1);
        CHECK(cemu_core_reset(core).code == CEMU_STATUS_OK);
        soc = cemu_core_private_soc(core);
        xbus_unknown1_state_t *mailbox = soc->xbus_unknown1_periph->state;
        CHECK(mailbox->phase == XBUS_MAILBOX_IDLE);
        CHECK(mailbox->control == 0 && mailbox->deadline == 0);
        cemu_soc_tick(soc, 2000);
        CHECK(bus_read16(&soc->bus, 0xec12) == 0);
        CHECK(!(bus_read16(&soc->bus, 0xf140) & XIC_IR_BIT));
    }
    cemu_core_destroy(core);
    free(flash);
}

static peripheral_t *peripheral_by_id(soc_t *soc, const char *id) {
    for (int i = 0; i < soc->n_peripherals; i++) {
        peripheral_t *peripheral = soc->peripherals[i];
        if (peripheral && peripheral->id && !strcmp(peripheral->id, id))
            return peripheral;
    }
    return NULL;
}

static void test_frame_and_key_controls_match_direct_path(void) {
    size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    cemu_core_options_t options = options_for(flash, size);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);
    soc_t *soc = cemu_core_private_soc(core);
    cpu_t *cpu = cemu_core_private_cpu(core);

    uint8_t core_rgb[LCD_MAX_RGB_SIZE];
    uint8_t direct_rgb[LCD_MAX_RGB_SIZE];
    for (int raw = 0; raw <= 1; raw++) {
        cemu_frame_info_t info;
        CHECK(cemu_core_render_frame(core, raw, core_rgb,
                                     sizeof core_rgb, &info).code ==
              CEMU_STATUS_OK);
        unsigned width = 0, height = 0;
        cemu_lcd_dimensions(soc->lcd_periph, raw, &width, &height);
        size_t rgb_size = (size_t)width * height * 3u;
        CHECK(cemu_lcd_render_rgb(soc->lcd_periph, raw, 1, direct_rgb,
                             rgb_size) == 0);
        CHECK(info.width == width);
        CHECK(info.height == height);
        CHECK(info.size == rgb_size);
        CHECK(info.sequence == cemu_lcd_frame_sequence(soc->lcd_periph));
        CHECK(info.instruction_count == cpu->icount);
        CHECK(memcmp(core_rgb, direct_rgb, rgb_size) == 0);
    }
    cemu_frame_info_t ignored;
    CHECK(cemu_core_render_frame(core, 0, core_rgb, 1, &ignored).code ==
          CEMU_STATUS_INVALID_ARGUMENT);

    peripheral_t *key_peripheral = peripheral_by_id(soc, "keypad");
    keypad_state_t *keypad = key_peripheral ? key_peripheral->state : NULL;
    const keypad_button_t *button = cemu_keypad_button_by_name(keypad, "power");
    CHECK(button != NULL);
    CHECK(cemu_core_set_key(core, "power", 1).code == CEMU_STATUS_OK);
    size_t key_count = 0;
    const char *key_name = NULL;
    CHECK(cemu_core_key_count(core, &key_count).code == CEMU_STATUS_OK);
    CHECK(key_count > 0);
    CHECK(cemu_core_key_name(core, key_count - 1, &key_name).code ==
          CEMU_STATUS_OK);
    CHECK(key_name != NULL);
    CHECK(cemu_core_key_name(core, key_count, &key_name).code ==
          CEMU_STATUS_INVALID_ARGUMENT);
    size_t index = (size_t)(button - keypad->buttons);
    uint32_t bit = UINT32_C(1) << index;
    CHECK((keypad->pressed & bit) != 0);
    keypad->sampled_pressed |= bit;
    cemu_key_state_t key_state;
    CHECK(cemu_core_query_key(core, "power", &key_state).code ==
          CEMU_STATUS_OK);
    CHECK(key_state.pressed == !!(keypad->pressed & bit));
    CHECK(key_state.sampled == !!(cemu_keypad_sampled_buttons(keypad) & bit));
    CHECK(key_state.instruction_count == cpu->icount);
    CHECK(cemu_core_set_key(core, "power", 0).code == CEMU_STATUS_OK);
    CHECK(cemu_core_set_key(core, "not-a-key", 1).code ==
          CEMU_STATUS_INVALID_ARGUMENT);

    cemu_core_destroy(core);
    free(flash);
}

static void test_serial_sim_battery_and_statistics_controls(void) {
    size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    cemu_core_options_t options = options_for(flash, size);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);
    soc_t *soc = cemu_core_private_soc(core);

    const uint8_t rx[] = {0x41, 0x42, 0x43};
    CHECK(cemu_core_feed_serial(core, rx, sizeof rx).code == CEMU_STATUS_OK);
    CHECK(soc->serial_rx_len == sizeof rx);
    CHECK(memcmp(soc->serial_rx, rx, sizeof rx) == 0);
    CHECK(cemu_soc_serial_tx_push(soc, 0x55) == CEMU_STATUS_OK);
    CHECK(cemu_soc_serial_tx_push(soc, 0xAA) == CEMU_STATUS_OK);
    cemu_serial_tx_view_t view;
    CHECK(cemu_core_serial_tx_view(core, 1, &view).code == CEMU_STATUS_OK);
    CHECK(view.data == soc->serial_tx + 1);
    CHECK(view.size == 1);
    CHECK(view.data[0] == 0xAA);
    CHECK(view.next_cursor == soc->serial_tx_len);
    CHECK(cemu_core_serial_tx_view(core, 3, &view).code ==
          CEMU_STATUS_INVALID_ARGUMENT);

    cemu_serial_link_state_t link;
    CHECK(cemu_core_set_serial_link(core, 1, &link).code == CEMU_STATUS_OK);
    CHECK(link.available == soc->serial_link.available);
    CHECK(link.attached);
    CHECK(link.received_bytes == sizeof rx);
    CHECK(link.transmitted_bytes == soc->serial_tx_len);
    CHECK(soc->serial_link_attached);

    CHECK(cemu_core_set_sim_attached(core, 0).code == CEMU_STATUS_OK);
    CHECK(cemu_sim_get_mode(soc->sim_periph) == SIM_MODE_NONE);
    CHECK(cemu_core_set_sim_attached(core, 1).code == CEMU_STATUS_OK);
    CHECK(cemu_sim_get_mode(soc->sim_periph) == SIM_MODE_STUB);

    CHECK(cemu_core_set_battery(core, 37, 1).code == CEMU_STATUS_OK);
    CHECK(cemu_battery_level(soc->battery_periph) == 37);
    CHECK(cemu_battery_charging(soc->battery_periph));
    CHECK(cemu_core_set_battery(core, 101, 0).code ==
          CEMU_STATUS_INVALID_ARGUMENT);

    CHECK(cemu_core_enable_statistics(core).code == CEMU_STATUS_OK);
    CHECK(cemu_event_statistics_enabled(&soc->instrumentation));
    cemu_core_state_t state;
    CHECK(cemu_core_query(core, &state).code == CEMU_STATUS_OK);
    CHECK(state.statistics_enabled);
    CHECK(state.pc == cpu_pc(cemu_core_private_cpu(core)));
    CHECK(state.instruction_count == cemu_core_private_cpu(core)->icount);
    cemu_core_private_cpu(core)->halted = 1;
    cemu_core_private_cpu(core)->unimpl = 1;
    cemu_core_private_cpu(core)->unimpl_op = 0x1234;
    cemu_core_private_cpu(core)->unimpl_pc = 0x5678;
    CHECK(cemu_core_request_stop(core).code == CEMU_STATUS_OK);
    CHECK(cemu_core_query(core, &state).code == CEMU_STATUS_OK);
    CHECK(state.halted);
    CHECK(state.unimplemented);
    CHECK(state.unimplemented_opcode == 0x1234);
    CHECK(state.unimplemented_pc == 0x5678);
    CHECK(state.stop_requested);
    CHECK(state.error.code == CEMU_STATUS_OK);

    cemu_core_destroy(core);
    free(flash);
}

static void test_battery_result_word_routes_and_device_matrix(void) {
    static const battery_curve_point_t expected_curve[] = {
        {0, 3649}, {1, 3659}, {2, 3669}, {3, 3679}, {4, 3689},
        {5, 3699}, {10, 3754}, {15, 3774}, {20, 3789},
        {25, 3804}, {30, 3814}, {35, 3824}, {40, 3834},
        {45, 3849}, {50, 3859}, {55, 3879}, {60, 3894},
        {65, 3919}, {70, 3939}, {75, 3969}, {80, 3999},
        {85, 4029}, {90, 4059}, {95, 4104}, {100, 4200},
    };
    static const struct {
        const char *name;
        uint8_t result_word;
    } cases[] = {
        {"a52", DEVICE_BATTERY_RESULT1},
        {"a55", DEVICE_BATTERY_RESULT1},
        {"a60", DEVICE_BATTERY_RESULT0},
        {"a62", DEVICE_BATTERY_RESULT0},
        {"a65", DEVICE_BATTERY_RESULT0},
        {"c55", DEVICE_BATTERY_RESULT1},
        {"c60", DEVICE_BATTERY_RESULT0},
        {"cf62", DEVICE_BATTERY_RESULT0},
        {"m55", DEVICE_BATTERY_RESULT0},
        {"mc60", DEVICE_BATTERY_RESULT0},
        {"s55", DEVICE_BATTERY_RESULT1},
        {"sl55", DEVICE_BATTERY_RESULT0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const device_battery_config_t *battery =
            &cemu_device_by_name(cases[i].name)->battery;
        CHECK(battery->available);
        CHECK(battery->adc_result_word == cases[i].result_word);
        CHECK(battery->battery_channel == 1);
        CHECK(battery->reference_channel == 3);
        CHECK(battery->adc_raw_low == 4499);
        CHECK(battery->adc_mv_low == 3177);
        CHECK(battery->adc_raw_high == -1628);
        CHECK(battery->adc_mv_high == 4178);
        CHECK(battery->curve != NULL && battery->n_curve == 25);
        for (size_t j = 0;
             j < sizeof expected_curve / sizeof expected_curve[0]; j++) {
            CHECK(battery->curve[j].level == expected_curve[j].level);
            CHECK(battery->curve[j].millivolts
                  == expected_curve[j].millivolts);
        }
    }

    static const char *route_models[] = {"a60", "c55"};
    for (size_t i = 0; i < sizeof route_models / sizeof route_models[0]; i++) {
        size_t size = 8u * 1024u * 1024u;
        uint8_t *flash = make_flash(size);
        cemu_core_options_t options = options_for(flash, size);
        options.device = *cemu_device_by_name(route_models[i]);
        cemu_core_t *core = NULL;
        CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);
        soc_t *soc = cemu_core_private_soc(core);
        if (!strcmp(route_models[i], "c55")) {
            for (size_t j = 0;
                 j < sizeof expected_curve / sizeof expected_curve[0]; j++) {
                CHECK(cemu_core_set_battery(core, expected_curve[j].level, 0).code
                      == CEMU_STATUS_OK);
                int16_t raw = cemu_battery_adc_raw(soc->battery_periph);
                long firmware_mv = 4178L
                                 - ((long)raw + 1628L) * 1001L / 6127L;
                CHECK(cemu_battery_millivolts(soc->battery_periph)
                      == expected_curve[j].millivolts);
                CHECK(firmware_mv == expected_curve[j].millivolts);
            }
        }
        CHECK(cemu_core_set_battery(core, 0, 0).code == CEMU_STATUS_OK);
        uint16_t sample = (uint16_t)cemu_battery_adc_raw(soc->battery_periph);
        bus_write16(&soc->bus, 0xE062u, 0x0801u);
        uint16_t result0 = cemu_memory_controller_peek16(&soc->memory,
                                                         BASEBAND_RESULT_WORD0);
        uint16_t result1 = cemu_memory_controller_peek16(&soc->memory,
                                                         BASEBAND_RESULT_WORD1);
        if (!strcmp(route_models[i], "a60")) {
            CHECK(result0 == sample && result1 == 0);
        } else {
            CHECK(result0 == 0 && result1 == sample);
        }
        cemu_core_destroy(core);
        free(flash);
    }
}

static void test_runtime_allocation_failures_are_latched(void) {
    size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    cemu_core_options_t options = options_for(flash, size);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);

    const uint8_t byte = 0x42;
    cemu_test_fail_alloc_after(0);
    CHECK(cemu_core_feed_serial(core, &byte, 1).code ==
          CEMU_STATUS_ALLOCATION_FAILED);
    cemu_test_clear_alloc_failure();
    cemu_core_state_t state;
    CHECK(cemu_core_query(core, &state).code == CEMU_STATUS_OK);
    CHECK(state.error.code == CEMU_STATUS_ALLOCATION_FAILED);
    cemu_run_result_t result;
    CHECK(cemu_core_run_slice(core, 1, &result).code == CEMU_STATUS_OK);
    CHECK(result.reason == CEMU_RUN_ERROR);
    CHECK(result.ticks == 0);

    CHECK(cemu_core_reset(core).code == CEMU_STATUS_OK);
    cemu_test_fail_alloc_after(0);
    CHECK(cemu_soc_serial_tx_push(cemu_core_private_soc(core), byte) ==
          CEMU_STATUS_ALLOCATION_FAILED);
    cemu_test_clear_alloc_failure();
    CHECK(cemu_core_query(core, &state).code == CEMU_STATUS_OK);
    CHECK(state.error.code == CEMU_STATUS_ALLOCATION_FAILED);

    CHECK(cemu_core_reset(core).code == CEMU_STATUS_OK);
    cemu_test_fail_alloc_after(0);
    CHECK(cemu_core_enable_statistics(core).code ==
          CEMU_STATUS_ALLOCATION_FAILED);
    cemu_test_clear_alloc_failure();
    CHECK(cemu_core_query(core, &state).code == CEMU_STATUS_OK);
    CHECK(state.error.code == CEMU_STATUS_ALLOCATION_FAILED);
    CHECK(!state.statistics_enabled);
    CHECK(cemu_core_reset(core).code == CEMU_STATUS_OK);
    CHECK(cemu_core_enable_statistics(core).code == CEMU_STATUS_OK);

    cemu_core_destroy(core);
    free(flash);
}

static void count_instruction_event(void *opaque, const cemu_event_t *event) {
    unsigned *count = opaque;
    if (event->type == CEMU_EVENT_INSTRUCTION) (*count)++;
}

static void test_opaque_event_subscriptions_survive_reset(void) {
    size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    cemu_core_options_t options = options_for(flash, size);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);
    unsigned count = 0, subscription = 0;
    CHECK(cemu_core_subscribe_events(
              core, CEMU_EVENT_INSTRUCTION, count_instruction_event,
              NULL, &count, &subscription).code == CEMU_STATUS_OK);
    CHECK(subscription != 0);
    cemu_run_result_t result;
    CHECK(cemu_core_run_slice(core, 2, &result).code == CEMU_STATUS_OK);
    CHECK(count > 0);
    unsigned first_count = count;
    CHECK(cemu_core_reset(core).code == CEMU_STATUS_OK);
    CHECK(cemu_core_run_slice(core, 1, &result).code == CEMU_STATUS_OK);
    CHECK(count > first_count);
    CHECK(cemu_core_unsubscribe_events(core, subscription).code ==
          CEMU_STATUS_OK);
    unsigned detached_count = count;
    CHECK(cemu_core_run_slice(core, 1, &result).code == CEMU_STATUS_OK);
    CHECK(count == detached_count);
    CHECK(cemu_core_subscribe_events(
              core, CEMU_EVENT_INSTRUCTION, count_instruction_event,
              NULL, &count, NULL).code == CEMU_STATUS_INVALID_ARGUMENT);
    cemu_core_destroy(core);
    free(flash);
}

static void count_trace_event(void *opaque, const emu_trace_event_t *event,
                              uint64_t sequence) {
    unsigned *count = opaque;
    (void)sequence;
    if (!strcmp(event->kind, "exec")) (*count)++;
}

static void test_shared_consumers_attach_through_core(void) {
    size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    cemu_core_options_t options = options_for(flash, size);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);

    unsigned trace_count = 0;
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_EXEC, count_trace_event, &trace_count);
    CHECK(sink != NULL);
    emu_cemu_trace_adapter_t *trace_adapter = NULL;
    CHECK(emu_cemu_trace_attach(core, sink, &trace_adapter) == 0);

    emu_drcov_t *collector = NULL;
    CHECK(emu_cemu_drcov_create(&collector) == EMU_DRCOV_OK);
    emu_cemu_drcov_adapter_t *drcov_adapter = NULL;
    CHECK(emu_cemu_drcov_attach_core(
              collector, core, &drcov_adapter) == EMU_DRCOV_OK);
    cemu_run_result_t result;
    CHECK(cemu_core_run_slice(core, 2, &result).code == CEMU_STATUS_OK);
    CHECK(trace_count > 0);
    emu_drcov_statistics_t statistics;
    emu_drcov_get_statistics(collector, &statistics);
    CHECK(statistics.execution_events > 0);

    emu_cemu_drcov_detach(&drcov_adapter);
    emu_cemu_trace_detach(&trace_adapter);
    CHECK(drcov_adapter == NULL && trace_adapter == NULL);
    CHECK(emu_trace_close(sink) == 0);
    emu_drcov_destroy(&collector);
    cemu_core_destroy(core);
    free(flash);
}

static void put16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void seed_trace_eeprom(uint8_t *flash) {
    const size_t base = 0x7A0000;
    const size_t magic = base + 0x12;
    flash[magic - 2] = flash[magic - 1] = 0xFE;
    memcpy(flash + magic, "EELITE", 6);
    memcpy(flash + magic + 0x20000, "EEFULL", 6);
    memcpy(flash + magic + 0x40000, "EEFULL", 6);
    uint8_t *record = flash + base + 0x200;
    put16(record, 0x01FC);
    put16(record + 2, 16);
    put16(record + 4, 0x1000);
    put16(record + 6, 0x00FA);
    put16(record + 8, 77);
    put16(record + 10, 0xFC00);
}

typedef struct {
    size_t maps;
    size_t reads;
    size_t programs;
    uint64_t last_icount;
    uint32_t last_pc;
    long last_tick;
} eeprom_trace_log_t;

static const emu_trace_field_t *trace_field(
        const emu_trace_event_t *event, const char *name) {
    for (int i = 0; i < event->info.n; i++)
        if (!strcmp(event->info.kv[i].key, name)) return &event->info.kv[i];
    return NULL;
}

static void count_eeprom_trace(void *opaque, const emu_trace_event_t *event,
                               uint64_t sequence) {
    eeprom_trace_log_t *log = opaque;
    (void)sequence;
    char error[256];
    CHECK(emu_trace_schema_validate_event(event, error, sizeof error) == 0);
    if (!strcmp(event->kind, "eeprom_map")) log->maps++;
    if (!strcmp(event->kind, "eeprom_access")) {
        const emu_trace_field_t *access = trace_field(event, "access");
        if (access && access->kind == EMU_TRACE_VALUE_STRING &&
            !strcmp(access->sval, "read")) log->reads++;
        if (access && access->kind == EMU_TRACE_VALUE_STRING &&
            !strcmp(access->sval, "program")) log->programs++;
    }
    const emu_trace_field_t *tick = trace_field(event, "tick");
    log->last_tick = tick && tick->kind == EMU_TRACE_VALUE_I64
                   ? tick->ival : -1;
    log->last_icount = event->icount;
    log->last_pc = event->pc;
}

static void count_release(void *opaque, const emu_trace_event_t *event,
                          uint64_t sequence) {
    size_t *count = opaque;
    (void)sequence;
    CHECK(!strcmp(event->kind, "keypad_deferred_release"));
    CHECK(event->value == 0x010e);
    CHECK(event->icount == 91 && event->pc == 0x123456);
    CHECK(!strcmp(trace_field(event, "button")->sval, "soft-left"));
    CHECK(trace_field(event, "key_index")->ival == 12);
    (*count)++;
}

static void test_deferred_key_release_trace(void) {
    const size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    cemu_core_options_t options = options_for(flash, size);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);
    size_t count = 0;
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_KEYPAD_DEFERRED_RELEASE, count_release, &count);
    emu_cemu_trace_adapter_t *adapter = NULL;
    CHECK(emu_cemu_trace_attach_deferred(core, sink, 1, &adapter) == 0);
    cemu_core_diagnostic_deferred_key_release(core, "soft-left", 12, "queued");
    CHECK(count == 0);
    cpu_t *cpu = cemu_core_private_cpu(core);
    cpu->icount = 91;
    cpu->csp = 0x12;
    cpu->ip = 0x3456;
    CHECK(emu_cemu_trace_set_enabled(adapter, 1) == 0);
    const char *phases[] = {"queued", "sampled", "cancelled", "forced"};
    for (size_t i = 0; i < sizeof phases / sizeof phases[0]; i++)
        cemu_core_diagnostic_deferred_key_release(core, "soft-left", 12, phases[i]);
    CHECK(count == 4);
    emu_cemu_trace_detach(&adapter);
    CHECK(emu_trace_close(sink) == 0);
    cemu_core_diagnostic_deferred_key_release(core, "soft-left", 12, "forced");
    CHECK(count == 4);
    cemu_core_destroy(core);
    free(flash);
}

static void test_logical_eeprom_adapter(void) {
    const size_t size = 8u * 1024u * 1024u;
    uint8_t *flash = make_flash(size);
    CHECK(flash != NULL);
    seed_trace_eeprom(flash);
    cemu_core_options_t options = options_for(flash, size);
    cemu_core_t *core = NULL;
    CHECK(cemu_core_create(&core, &options).code == CEMU_STATUS_OK);
    eeprom_trace_log_t log = {0};
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_EEPROM, count_eeprom_trace, &log);
    CHECK(sink != NULL);
    emu_cemu_trace_adapter_t *adapter = NULL;
    CHECK(emu_cemu_trace_attach_deferred(
              core, sink, 1, &adapter) == 0);
    CHECK(log.maps == 0 && log.reads == 0 && log.programs == 0);
    CHECK(emu_cemu_trace_set_enabled(adapter, 1) == 0);
    CHECK(log.maps == 1 && log.last_tick == 0 && log.last_icount == 0 &&
          log.last_pc == 0);

    soc_t *soc = cemu_core_private_soc(core);
    cpu_t *cpu = cemu_core_private_cpu(core);
    cpu->icount = 91;
    cpu->csp = 0x12;
    cpu->ip = 0x3456;
    soc->ticks = 88;
    CHECK(bus_read8(&soc->bus, 0xFA1004) == 0xFF);
    CHECK(log.reads == 1 && log.last_tick == 88 &&
          log.last_icount == 91 && log.last_pc == 0x123456);

    (void)bus_fetch8(&soc->bus, 0xFA1004);
    CHECK(log.reads == 1);
    bus_write8(&soc->bus, 0xFA1004, 0x90);
    (void)bus_read8(&soc->bus, 0xFA1004);
    CHECK(log.reads == 1);
    bus_write8(&soc->bus, 0xFA1004, 0xFF);
    bus_write8(&soc->bus, 0xFA1004, 0x40);
    bus_write8(&soc->bus, 0xFA1004, 0xFF);
    CHECK(log.programs == 1);

    cemu_memory_controller_poke8(&soc->memory, 0xFA1005, 0x00);
    CHECK(log.programs == 1);
    emu_cemu_trace_detach(&adapter);
    CHECK(adapter == NULL);
    CHECK(emu_trace_close(sink) == 0);
    cemu_core_destroy(core);
    free(flash);
}

static void test_eeprom_trace_is_observational(void) {
    const size_t size = 8u * 1024u * 1024u;
    uint8_t *plain_flash = make_flash(size);
    uint8_t *traced_flash = make_flash(size);
    CHECK(plain_flash != NULL && traced_flash != NULL);
    seed_trace_eeprom(plain_flash);
    seed_trace_eeprom(traced_flash);
    cemu_core_options_t plain_options = options_for(plain_flash, size);
    cemu_core_options_t traced_options = options_for(traced_flash, size);
    cemu_core_t *plain = NULL, *traced = NULL;
    CHECK(cemu_core_create(&plain, &plain_options).code == CEMU_STATUS_OK);
    CHECK(cemu_core_create(&traced, &traced_options).code == CEMU_STATUS_OK);
    eeprom_trace_log_t log = {0};
    emu_trace_sink_t *sink = emu_trace_open_callback(
        EMU_TRACE_EEPROM_ACCESS, count_eeprom_trace, &log);
    emu_cemu_trace_adapter_t *adapter = NULL;
    CHECK(sink != NULL);
    CHECK(emu_cemu_trace_attach(traced, sink, &adapter) == 0);

    cemu_run_result_t plain_result, traced_result;
    CHECK(cemu_core_run_slice(plain, 100, &plain_result).code ==
          CEMU_STATUS_OK);
    CHECK(cemu_core_run_slice(traced, 100, &traced_result).code ==
          CEMU_STATUS_OK);
    CHECK(plain_result.reason == traced_result.reason &&
          plain_result.ticks == traced_result.ticks &&
          plain_result.guest_instructions == traced_result.guest_instructions &&
          plain_result.instruction_count == traced_result.instruction_count &&
          plain_result.pc == traced_result.pc);
    CHECK(cemu_core_diagnostic_state_digest(plain) ==
          cemu_core_diagnostic_state_digest(traced));

    cemu_serial_tx_view_t plain_serial, traced_serial;
    CHECK(cemu_core_serial_tx_view(plain, 0, &plain_serial).code ==
          CEMU_STATUS_OK);
    CHECK(cemu_core_serial_tx_view(traced, 0, &traced_serial).code ==
          CEMU_STATUS_OK);
    CHECK(plain_serial.size == traced_serial.size);
    CHECK(!plain_serial.size ||
          !memcmp(plain_serial.data, traced_serial.data, plain_serial.size));

    uint8_t *plain_rgb = malloc(CEMU_FRAME_MAX_RGB_SIZE);
    uint8_t *traced_rgb = malloc(CEMU_FRAME_MAX_RGB_SIZE);
    CHECK(plain_rgb != NULL && traced_rgb != NULL);
    cemu_frame_info_t plain_frame, traced_frame;
    CHECK(cemu_core_render_frame(
              plain, 0, plain_rgb, CEMU_FRAME_MAX_RGB_SIZE,
              &plain_frame).code == CEMU_STATUS_OK);
    CHECK(cemu_core_render_frame(
              traced, 0, traced_rgb, CEMU_FRAME_MAX_RGB_SIZE,
              &traced_frame).code == CEMU_STATUS_OK);
    CHECK(plain_frame.width == traced_frame.width &&
          plain_frame.height == traced_frame.height &&
          plain_frame.size == traced_frame.size &&
          !memcmp(plain_rgb, traced_rgb, plain_frame.size));
    CHECK(log.maps == 0 && log.reads == 0 && log.programs == 0);

    free(plain_rgb);
    free(traced_rgb);
    emu_cemu_trace_detach(&adapter);
    CHECK(emu_trace_close(sink) == 0);
    cemu_core_destroy(plain);
    cemu_core_destroy(traced);
    free(plain_flash);
    free(traced_flash);
}

int main(void) {
    test_create_run_stop_query_and_destroy();
    test_failed_create_clears_output();
    test_reset_is_transactional();
    test_reset_cancels_mailbox();
    test_frame_and_key_controls_match_direct_path();
    test_serial_sim_battery_and_statistics_controls();
    test_battery_result_word_routes_and_device_matrix();
    test_runtime_allocation_failures_are_latched();
    test_opaque_event_subscriptions_survive_reset();
    test_shared_consumers_attach_through_core();
    test_deferred_key_release_trace();
    test_logical_eeprom_adapter();
    test_eeprom_trace_is_observational();
    printf("core lifecycle: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
