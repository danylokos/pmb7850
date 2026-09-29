# CEMU PMB7850 / C166S engine and native diagnostics.
.DEFAULT_GOAL := all

CC ?= cc
AR ?= ar
PYTHON ?= $(abspath ../.venv/bin/python)
SRC := src
BUILD := build
WARN := -std=c11 -Wall -Wextra -Wno-unused-function
DEP := -MMD -MP
TEST_FEATURES :=
ifeq ($(shell uname -s),Darwin)
TEST_FEATURES := -D_DARWIN_C_SOURCE
endif
INCLUDES := -I$(SRC)/core -I$(SRC)/cpu -I$(SRC)/soc \
	-I$(SRC)/soc/peripherals -I$(SRC)/soc/peripherals/flash \
	-I$(SRC)/soc/peripherals/audio \
	-I$(SRC)/soc/peripherals/lcd -I$(SRC)/soc/peripherals/timers \
	-I$(SRC)/diagnostics

CORE_SRC := $(SRC)/core/cemu_core.c $(SRC)/core/cemu_core_storage.c \
	$(SRC)/cpu/cpu.c $(SRC)/cpu/names.c \
	$(SRC)/soc/cemu_event.c $(SRC)/soc/cemu_status.c $(SRC)/soc/soc.c \
	$(SRC)/soc/memory_controller.c $(SRC)/soc/interrupts.c $(SRC)/soc/devices.c \
	$(wildcard $(SRC)/soc/devices/*.c) \
	$(wildcard $(SRC)/soc/peripherals/*.c) \
	$(wildcard $(SRC)/soc/peripherals/audio/*.c) \
	$(wildcard $(SRC)/soc/peripherals/flash/*.c) \
	$(wildcard $(SRC)/soc/peripherals/lcd/*.c) \
	$(wildcard $(SRC)/soc/peripherals/timers/*.c)
PLAIN_DIAG_SRC := $(SRC)/diagnostics/cemu_core_diagnostics.c \
	$(SRC)/diagnostics/state_digest.c $(SRC)/diagnostics/synth.c \
	$(SRC)/diagnostics/snapshot.c
INST_DIAG_SRC := $(PLAIN_DIAG_SRC) $(SRC)/cpu/disasm.c \
	$(SRC)/diagnostics/monitor.c $(SRC)/diagnostics/summary.c \
	$(SRC)/diagnostics/debugger.c $(SRC)/diagnostics/gsm_l1_probe.c

PLAIN_OBJ := $(patsubst %.c,$(BUILD)/plain/%.o,$(CORE_SRC) $(PLAIN_DIAG_SRC))
INST_OBJ := $(patsubst %.c,$(BUILD)/inst/%.o,$(CORE_SRC) $(INST_DIAG_SRC))
PLAIN_LIB := $(BUILD)/lib/libcemu.a
INST_LIB := $(BUILD)/lib/libcemu_inst.a

ALL_TEST_SRC := $(shell find tests -type f -name 'test_*.c' | sort)
TEST_SRC := $(ALL_TEST_SRC)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/tests/%,$(TEST_SRC))

.PHONY: all core-libs check-libs test clean
all: core-libs
core-libs: $(PLAIN_LIB) $(INST_LIB) check-libs

check-libs: $(PLAIN_LIB) $(INST_LIB)
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tests/check_libraries.py

$(BUILD)/plain/src/cpu/cpu.o: $(SRC)/cpu/cpu.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(INCLUDES) -O2 -DCEMU_INSTRUMENTED=0 -DCEMU_TRACE_PARQUET=0 -c $< -o $@

$(BUILD)/inst/src/cpu/cpu.o: $(SRC)/cpu/cpu.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(INCLUDES) -O2 -DCEMU_INSTRUMENTED=1 -DCEMU_TRACE_PARQUET=0 -c $< -o $@

$(BUILD)/plain/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(INCLUDES) -O3 -DCEMU_INSTRUMENTED=0 -DCEMU_TRACE_PARQUET=0 -c $< -o $@

$(BUILD)/inst/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(INCLUDES) -O3 -DCEMU_INSTRUMENTED=1 -DCEMU_TRACE_PARQUET=0 -c $< -o $@

$(PLAIN_LIB): $(PLAIN_OBJ)
	@mkdir -p $(@D)
	$(RM) $@
	$(AR) rcs $@ $^

$(INST_LIB): $(INST_OBJ)
	@mkdir -p $(@D)
	$(RM) $@
	$(AR) rcs $@ $^

$(BUILD)/tests/%: tests/%.c $(INST_LIB)
	@mkdir -p $(@D)
	$(CC) $(WARN) $(INCLUDES) -Itests/cpu -Itests/harness \
		$(TEST_FEATURES) -DCEMU_INSTRUMENTED=1 -DCEMU_TRACE_PARQUET=0 \
		$< $(INST_LIB) -lz -o $@

test: core-libs $(TEST_BIN)
	@set -e; for test in $(TEST_BIN); do $$test; done

clean:
	$(RM) -r $(BUILD)

-include $(PLAIN_OBJ:.o=.d) $(INST_OBJ:.o=.d)
