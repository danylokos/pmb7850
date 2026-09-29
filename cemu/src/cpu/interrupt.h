/* CPU-facing interrupt service boundary. */
#ifndef CEMU_INTERRUPT_H
#define CEMU_INTERRUPT_H

#include <stdint.h>

typedef struct {
    uint32_t source_token;
    uint32_t ic_addr;
    int trap;
    int ilvl;
} interrupt_request_t;

typedef enum {
    INTERRUPT_SERVICE_CPU = 0,
    INTERRUPT_SERVICE_PEC,
} interrupt_service_t;

typedef struct interrupt_port {
    void *ctx;
    int (*pending)(void *ctx, interrupt_request_t *request);
    interrupt_service_t (*begin_service)(
        void *ctx, const interrupt_request_t *request);
    void (*acknowledge)(void *ctx, const interrupt_request_t *request);
} interrupt_port_t;

#endif /* CEMU_INTERRUPT_H */
