/* CEMU diagnostic aliases over the native event hub. */
#ifndef CEMU_INSTRUMENTATION_H
#define CEMU_INSTRUMENTATION_H

#include "cemu_event.h"

typedef cemu_event_mask_t instrumentation_event_mask_t;
#define INSTR_EVENT_INSTRUCTION CEMU_EVENT_INSTRUCTION
#define INSTR_EVENT_BUS CEMU_EVENT_BUS
#define INSTR_EVENT_CONTROL CEMU_EVENT_CONTROL
#define INSTR_EVENT_PROTECTED CEMU_EVENT_PROTECTED
#define INSTR_EVENT_TRAP CEMU_EVENT_TRAP
#define INSTR_EVENT_DEBUG_IRQ CEMU_EVENT_DEBUG_IRQ
#define INSTR_EVENT_PERIPHERAL CEMU_EVENT_PERIPHERAL
#define INSTR_EVENT_FLASH_MUTATION CEMU_EVENT_FLASH_MUTATION

typedef cemu_instruction_event_t instrumentation_instruction_t;
typedef cemu_control_event_t instrumentation_control_t;
typedef cemu_debug_irq_event_t instrumentation_debug_irq_t;
typedef cemu_event_t instrumentation_event_t;
typedef cemu_event_consumer_fn instrumentation_consumer_fn;
typedef cemu_event_subscription_t instrumentation_subscription_t;
typedef cemu_event_hub_t instrumentation_hub_t;

#define INSTRUMENTATION_MAX_CONSUMERS CEMU_EVENT_MAX_CONSUMERS
#define instrumentation_hub_init cemu_event_hub_init
#define instrumentation_subscribe cemu_event_subscribe
#define instrumentation_unsubscribe cemu_event_unsubscribe
#define instrumentation_emit cemu_event_emit
#define instrumentation_active cemu_event_active
#define instrumentation_set_stats cemu_event_set_statistics
#define instrumentation_stats_enabled cemu_event_statistics_enabled

#if CEMU_INSTRUMENTED
#define INSTRUMENTATION_IF(hub, mask) if (instrumentation_active((hub), (mask)))
#else
#define INSTRUMENTATION_IF(hub, mask) if (0)
#endif

#endif
