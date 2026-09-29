#include "cemu_event.h"

#include <string.h>

void cemu_event_hub_init(cemu_event_hub_t *hub) {
    if (!hub) return;
    memset(hub, 0, sizeof *hub);
#if CEMU_INSTRUMENTED
    hub->next_id = 1;
#endif
}

unsigned cemu_event_subscribe_filtered(
        cemu_event_hub_t *hub, cemu_event_mask_t mask,
        cemu_event_consumer_fn consumer, cemu_event_filter_fn filter,
        void *opaque) {
#if CEMU_INSTRUMENTED
    if (!hub || !consumer || !mask ||
        hub->consumer_count >= CEMU_EVENT_MAX_CONSUMERS)
        return 0;
    cemu_event_subscription_t *subscription =
        &hub->consumers[hub->consumer_count++];
    *subscription = (cemu_event_subscription_t){
        .mask = mask,
        .consumer = consumer,
        .filter = filter,
        .opaque = opaque,
        .id = hub->next_id++,
    };
    if (!subscription->id) subscription->id = hub->next_id++;
    hub->active_mask = (cemu_event_mask_t)(hub->active_mask | mask);
    return subscription->id;
#else
    (void)hub;
    (void)mask;
    (void)consumer;
    (void)filter;
    (void)opaque;
    return 0;
#endif
}

unsigned cemu_event_subscribe(
        cemu_event_hub_t *hub, cemu_event_mask_t mask,
        cemu_event_consumer_fn consumer, void *opaque) {
    return cemu_event_subscribe_filtered(
        hub, mask, consumer, NULL, opaque);
}

void cemu_event_unsubscribe(cemu_event_hub_t *hub, unsigned id) {
#if CEMU_INSTRUMENTED
    if (!hub || !id) return;
    for (int i = 0; i < hub->consumer_count; i++) {
        if (hub->consumers[i].id != id) continue;
        if (i + 1 < hub->consumer_count) {
            memmove(&hub->consumers[i], &hub->consumers[i + 1],
                    (size_t)(hub->consumer_count - i - 1) *
                        sizeof hub->consumers[0]);
        }
        hub->consumer_count--;
        hub->active_mask = 0;
        for (int j = 0; j < hub->consumer_count; j++)
            hub->active_mask = (cemu_event_mask_t)(
                hub->active_mask | hub->consumers[j].mask);
        return;
    }
#else
    (void)hub;
    (void)id;
#endif
}

void cemu_event_emit(cemu_event_hub_t *hub, const cemu_event_t *event) {
#if CEMU_INSTRUMENTED
    if (!hub || !event || !(hub->active_mask & event->type)) return;
    cemu_event_subscription_t selected[CEMU_EVENT_MAX_CONSUMERS];
    int count = 0;
    for (int i = 0; i < hub->consumer_count; i++) {
        if (hub->consumers[i].mask & event->type)
            selected[count++] = hub->consumers[i];
    }
    for (int i = 0; i < count; i++)
        selected[i].consumer(selected[i].opaque, event);
#else
    (void)hub;
    (void)event;
#endif
}

#if CEMU_INSTRUMENTED
int cemu_event_native_trace_active(
        const cemu_event_hub_t *hub, const char *kind) {
    if (!hub || !kind) return 0;
    for (int i = 0; i < hub->consumer_count; i++) {
        const cemu_event_subscription_t *subscription = &hub->consumers[i];
        if (!(subscription->mask & (CEMU_EVENT_BUS | CEMU_EVENT_PERIPHERAL)))
            continue;
        if (!subscription->filter ||
            subscription->filter(subscription->opaque, kind))
            return 1;
    }
    return 0;
}

void cemu_event_emit_native_trace(
        cemu_event_hub_t *hub, const cemu_native_trace_event_t *trace,
        cemu_event_mask_t type) {
    if (!hub || !trace || !cemu_event_native_trace_active(hub, trace->kind))
        return;
    cemu_event_t event = { .type = type };
    if (type == CEMU_EVENT_BUS)
        event.as.bus.trace = trace;
    else
        event.as.peripheral.trace = trace;
    cemu_event_emit(hub, &event);
}

void cemu_event_set_statistics(cemu_event_hub_t *hub, int enabled) {
    if (hub) hub->statistics_enabled = enabled != 0;
}

int cemu_event_statistics_enabled(const cemu_event_hub_t *hub) {
    return hub && hub->statistics_enabled;
}
#endif
