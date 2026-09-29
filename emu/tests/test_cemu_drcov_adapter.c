#include <stdio.h>

#include "cemu_core.h"
#include "test_support.h"
#include "emu_cemu_drcov_adapter.h"

static int reject_subscription;
static cemu_event_consumer_fn subscribed_consumer;
static void *subscribed_opaque;

cemu_status_t cemu_core_subscribe_events(
        cemu_core_t *core, cemu_event_mask_t mask,
        cemu_event_consumer_fn consumer, cemu_event_filter_fn filter,
        void *opaque, unsigned *subscription) {
    (void)core; (void)filter;
    if (reject_subscription || mask != CEMU_EVENT_INSTRUCTION || !consumer)
        return (cemu_status_t){.code = CEMU_STATUS_UNSUPPORTED};
    subscribed_consumer = consumer;
    subscribed_opaque = opaque;
    *subscription = 1;
    return (cemu_status_t){.code = CEMU_STATUS_OK};
}

cemu_status_t cemu_core_unsubscribe_events(cemu_core_t *core, unsigned subscription) {
    (void)core; (void)subscription;
    subscribed_consumer = NULL;
    subscribed_opaque = NULL;
    return (cemu_status_t){.code = CEMU_STATUS_OK};
}

static void emit_instruction(uint32_t pc, int size) {
    cemu_event_t event = {.type = CEMU_EVENT_INSTRUCTION};
    event.as.instruction.pc_before = pc;
    event.as.instruction.size = size;
    if (subscribed_consumer) subscribed_consumer(subscribed_opaque, &event);
}

static void test_translation_and_cleanup(void) {
    emu_drcov_t *collector = NULL;
    EMU_CHECK(emu_cemu_drcov_create(&collector) == EMU_DRCOV_OK);
    emu_cemu_drcov_adapter_t *adapter = NULL;
    EMU_CHECK(emu_cemu_drcov_attach_core(
        collector, (cemu_core_t *)1, &adapter) == EMU_DRCOV_OK);
    emit_instruction(0xab802000u, 2);
    emit_instruction(0xcd802000u, 2);
    emit_instruction(0x00802002u, 0);
    emu_drcov_statistics_t statistics;
    emu_drcov_get_statistics(collector, &statistics);
    EMU_CHECK(statistics.execution_events == 2);
    EMU_CHECK(statistics.unique_blocks == 1);
    EMU_CHECK(statistics.covered_modules == 1);
    emu_cemu_drcov_detach(&adapter);
    EMU_CHECK(adapter == NULL && subscribed_consumer == NULL);
    emu_drcov_destroy(&collector);
}

static void test_attach_and_latched_failures(void) {
    emu_drcov_t *collector = NULL;
    EMU_CHECK(emu_cemu_drcov_create(&collector) == EMU_DRCOV_OK);
    emu_cemu_drcov_adapter_t *adapter = NULL;
    reject_subscription = 1;
    EMU_CHECK(emu_cemu_drcov_attach_core(
        collector, (cemu_core_t *)1, &adapter) == EMU_DRCOV_ERR_VALIDATION);
    reject_subscription = 0;
    EMU_CHECK(emu_cemu_drcov_attach_core(NULL, (cemu_core_t *)1, &adapter) ==
              EMU_DRCOV_ERR_ARGUMENT);
    EMU_CHECK(emu_cemu_drcov_attach_core(
        collector, (cemu_core_t *)1, &adapter) == EMU_DRCOV_OK);
    emit_instruction(0x0001ffu, 2);
    EMU_CHECK(emu_cemu_drcov_result(adapter) == EMU_DRCOV_ERR_VALIDATION);
    emit_instruction(0x802000u, 2);
    emu_drcov_statistics_t statistics;
    emu_drcov_get_statistics(collector, &statistics);
    EMU_CHECK(statistics.execution_events == 0);
    emu_cemu_drcov_detach(&adapter);
    emu_drcov_destroy(&collector);
}

int main(void) {
    test_translation_and_cleanup();
    test_attach_and_latched_failures();
    puts("CEMU drcov event adapter: PASS");
    return 0;
}
