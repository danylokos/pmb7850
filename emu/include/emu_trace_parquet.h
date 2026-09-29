#ifndef EMU_TRACE_PARQUET_H
#define EMU_TRACE_PARQUET_H

#include "emu_trace.h"

/* Deterministic worker-failure injection used by host-only tests. */
void emu_trace_test_fail_worker_after(int successful_batches);

#endif
