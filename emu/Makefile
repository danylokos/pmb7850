.DEFAULT_GOAL := all
CC ?= cc
CMAKE ?= $(abspath ../.venv/bin/cmake)
PYTHON ?= $(abspath ../.venv/bin/python)
BUILD := build
OBJ := $(BUILD)/obj
BIN := $(BUILD)/tests
PUBLIC_BIN := bin
CEMU := ../cemu
OPENMP_FLAG := $(shell $(CC) -fopenmp -E -x c /dev/null >/dev/null 2>&1 && echo -fopenmp)
GIO_CFLAGS := $(shell pkg-config --cflags gio-2.0 gio-unix-2.0 pixman-1)
GIO_LIBS := $(shell pkg-config --libs gio-2.0 gio-unix-2.0)

WARN := -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
DEP := -MMD -MP
HOST_INCLUDES := -Iinclude
GC_LDFLAGS := -Wl,--gc-sections
TEST_FEATURES :=
ifeq ($(shell uname -s),Darwin)
GC_LDFLAGS := -Wl,-dead_strip
TEST_FEATURES := -D_DARWIN_C_SOURCE
endif
CEMU_INCLUDES := $(HOST_INCLUDES) -I$(CEMU)/src/cpu -I$(CEMU)/src/soc \
	-I$(CEMU)/src/core \
	-I$(CEMU)/src/soc/peripherals -I$(CEMU)/src/soc/peripherals/flash \
	-I$(CEMU)/src/soc/peripherals/audio \
	-I$(CEMU)/src/soc/peripherals/lcd -I$(CEMU)/src/soc/peripherals/timers \
	-I$(CEMU)/src/diagnostics

HOST_SRC := src/host.c src/sha256.c src/manifest.c src/product.c src/qemu_snapshot.c \
	src/eeprom.c src/eeprom_trace.c src/eeprom_debugger.c src/identity.c src/patch.c src/prepare.c \
	src/trace.c src/trace_schema.c \
	src/input.c src/runtime.c src/runtime_state.c src/runtime_cli.c src/ui_socket.c src/serial.c src/artifact.c \
	src/drcov.c src/dispatch.c src/cemu_runtime.c src/gdb.c
ADAPTER_SRC := src/adapters/cemu/adapter.c src/adapters/cemu/storage.c \
	src/registry.c
TRACE_ADAPTER_SRC := src/adapters/cemu/trace.c
DRCOV_ADAPTER_SRC := src/adapters/cemu/drcov.c
QEMU_ADAPTER_SRC := src/adapters/qemu/adapter.c src/adapters/qemu/console.c \
	src/adapters/qemu/display.c src/adapters/qemu/audio.c src/adapters/qemu/image.c \
	src/adapters/qemu/qmp.c src/adapters/qemu/trace.c \
	src/adapters/qemu/eeprom_trace.c
HOST_OBJ := $(patsubst %.c,$(OBJ)/%.o,$(HOST_SRC))
ADAPTER_OBJ := $(patsubst %.c,$(OBJ)/%.o,$(ADAPTER_SRC))
ADAPTER_INST_OBJ := $(patsubst %.c,$(OBJ)/inst/%.o,$(ADAPTER_SRC))
TRACE_ADAPTER_OBJ := $(patsubst %.c,$(OBJ)/%.o,$(TRACE_ADAPTER_SRC))
DRCOV_ADAPTER_OBJ := $(patsubst %.c,$(OBJ)/%.o,$(DRCOV_ADAPTER_SRC))
QEMU_ADAPTER_OBJ := $(patsubst %.c,$(OBJ)/%.o,$(QEMU_ADAPTER_SRC))
TRACE_PARQUET_OBJ := $(OBJ)/src/trace_parquet.o
CEMU_CORE_LIB := $(CEMU)/build/lib/libcemu.a
CEMU_CORE_INST_LIB := $(CEMU)/build/lib/libcemu_inst.a
CEMU_CORE_INPUTS := $(CEMU)/Makefile \
	$(shell find $(CEMU)/src/core $(CEMU)/src/cpu $(CEMU)/src/soc \
	  -type f \( -name '*.c' -o -name '*.h' \) | sort)
CEMU_ADAPTER_SUPPORT_INPUTS := $(CEMU)/Makefile \
	$(CEMU)/src/core/cemu_core.h $(CEMU)/src/core/cemu_core_private.h \
	$(CEMU)/src/diagnostics/cemu_core_diagnostics.c \
	$(CEMU)/src/diagnostics/cemu_core_diagnostics.h \
	$(CEMU)/src/cpu/disasm.c $(CEMU)/src/cpu/disasm.h \
	$(CEMU)/src/diagnostics/gsm_l1_probe.c \
	$(CEMU)/src/diagnostics/gsm_l1_probe.h \
	$(CEMU)/src/diagnostics/debugger.c $(CEMU)/src/diagnostics/debugger.h \
	$(CEMU)/src/diagnostics/instrumentation.h \
	$(CEMU)/src/diagnostics/monitor.c $(CEMU)/src/diagnostics/monitor.h \
	$(CEMU)/src/diagnostics/summary.c $(CEMU)/src/diagnostics/summary.h \
	$(CEMU)/src/diagnostics/snapshot.c $(CEMU)/src/diagnostics/snapshot.h \
	$(CEMU)/src/diagnostics/state_digest.c $(CEMU)/src/diagnostics/state_digest.h \
	$(CEMU)/src/diagnostics/synth.c $(CEMU)/src/diagnostics/synth.h
CEMU_ADAPTER_LINK_OBJS := $(ADAPTER_OBJ) $(CEMU_CORE_LIB) $(HOST_OBJ)
CEMU_ADAPTER_INST_LINK_OBJS := $(ADAPTER_INST_OBJ) $(TRACE_ADAPTER_OBJ) \
	$(DRCOV_ADAPTER_OBJ) $(CEMU_CORE_INST_LIB) $(HOST_OBJ)
CEMU_RUNTIME_INST_LINK_OBJS := $(CEMU_ADAPTER_INST_LINK_OBJS) \
	$(TRACE_PARQUET_OBJ)

TEST_NAMES := test_host test_product test_eeprom test_eeprom_trace test_eeprom_debugger test_identity test_patch test_prepare \
	test_contract test_cemu_adapter test_trace test_runtime test_input test_ui test_serial \
	test_artifact test_drcov test_cemu_drcov_adapter test_cemu_core test_dispatch
TEST_NAMES += test_qemu_image test_qmp test_qemu_snapshot
TEST_NAMES += test_qemu_display test_qemu_audio
TEST_NAMES += test_qemu_adapter
TEST_NAMES += test_qemu_trace
TEST_NAMES += test_qemu_eeprom_trace
TEST_BINS := $(addprefix $(BIN)/,$(TEST_NAMES))
TEST_OBJ := $(patsubst %.c,$(OBJ)/%.o,$(wildcard tests/*.c))
FAKE_OBJ := $(OBJ)/tests/fake_engine.o
FIXTURE := $(BUILD)/fixtures/generated-c55.bin
CANONICAL_FLASH = $(shell $(PYTHON) tools/bundled_firmware.py --model c55)
S55_FLASH = $(shell $(PYTHON) tools/bundled_firmware.py --model s55)
QEMU_ROOT := $(abspath ../qemu)
QEMU_PROFILE := $(QEMU_ROOT)/scripts/pmb7850/build_profile.py
QEMU_DEBUG_BIN := $(QEMU_ROOT)/build/debug/qemu-system-c166
QEMU_RELEASE_BIN := $(QEMU_ROOT)/build/release/qemu-system-c166
QEMU_PERF_CPU ?= 3
QEMU_PERF_ROOT := shots/qemu-phase5-performance/benchmark
QEMU_PERF_FLASH ?= $(CANONICAL_FLASH)
QEMU_MATRIX_DEVICES ?= all
QEMU_MATRIX_GATES ?= no-sim
QEMU_MATRIX_ROOT ?= shots/qemu-phase6-device-crosscheck
QEMU_MATRIX_RUNNER := tools.qualification.shared.run_x55_boot
GDB_BUILD_DIR ?= $(abspath ../gdb/build/c166)
GDB_JOBS ?= 4
GDB_BINARY ?= $(GDB_BUILD_DIR)/gdb/gdb
QEMU_BINARY ?= $(QEMU_RELEASE_BIN)
GDB_DISASSEMBLY_OUTPUT ?= shots/gdb-c166-phase4/disassembly
GDB_REGISTER_OUTPUT ?= shots/gdb-c166-phase3/registers
GDB_MEMORY_OUTPUT ?= shots/gdb-c166-phase6/memory
GDB_EXECUTION_OUTPUT ?= shots/gdb-c166-phase5/execution
GDB_SCRIPTING_OUTPUT ?= shots/gdb-c166-phase7/scripting
EMU_BINARY ?= $(abspath $(PUBLIC_BIN)/emu)
GDB_HANDSET_OUTPUT ?= shots/gdb-c166-phase8/handsets
TOOL_TEST_SUITES := tools/tests tools/qualification/tests tools/siemens_tools/tests tools/obex/tests \
	tools/web_ui/tests tools/cemu_trace/tests

.PHONY: all test test-native test-tools deps qemu-deps clean-deps check-carquet clean \
	qemu-debug qemu-release qemu-layout qemu-perf qemu-matrix gdb test-gdb test-gdb-build test-gdb-registers test-gdb-disassembly test-gdb-execution test-gdb-memory
all: $(CEMU_CORE_LIB) $(CEMU_CORE_INST_LIB) $(PUBLIC_BIN)/emu \
	$(PUBLIC_BIN)/cemu $(PUBLIC_BIN)/cemu_inst

$(PUBLIC_BIN)/recover_fsn: tools/recover_fsn.c
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -O3 $(OPENMP_FLAG) $< -o $@

CARQUET_SRC := vendor/carquet
CARQUET_BUILD := $(BUILD)/carquet
CARQUET_LIB_DIR := $(abspath $(CARQUET_BUILD)/lib)
CARQUET_STAMP := $(CARQUET_BUILD)/.built
CARQUET_LIBS := $(CARQUET_LIB_DIR)/libcarquet.a \
	$(CARQUET_LIB_DIR)/libzstd.a $(CARQUET_LIB_DIR)/liblz4.a \
	$(CARQUET_LIB_DIR)/libz.a
CARQUET_INPUTS := tools/trace/CMakeLists.txt \
	$(shell test ! -d $(CARQUET_SRC) || find $(CARQUET_SRC) -type f \
	  \( -name '*.c' -o -name '*.h' -o -name '*.cmake' \
	     -o -name 'CMakeLists.txt' \))
CARQUET_INCLUDE := -I$(CARQUET_SRC)/include

$(CARQUET_STAMP): $(CARQUET_INPUTS)
	@test -f "$(CARQUET_SRC)/CMakeLists.txt" || \
		{ echo "Carquet submodule missing; run: git submodule update --init emu/vendor/carquet"; exit 1; }
	@test -x "$(CMAKE)" || \
		{ echo "CMake missing; install the repository requirements"; exit 1; }
	@if test -f "$(CARQUET_BUILD)/CMakeCache.txt" && \
	    ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$(abspath tools/trace)" \
	      "$(CARQUET_BUILD)/CMakeCache.txt"; then \
		$(CMAKE) -E rm -f "$(CARQUET_BUILD)/CMakeCache.txt"; \
		$(CMAKE) -E rm -rf "$(CARQUET_BUILD)/CMakeFiles"; \
	fi
	$(CMAKE) -S tools/trace -B $(CARQUET_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=$(CARQUET_LIB_DIR)
	+$(CMAKE) --build $(CARQUET_BUILD) --target carquet
	@touch $@

deps: $(CARQUET_STAMP)

check-carquet:
	@test -f "$(CARQUET_STAMP)" && \
		for lib in $(CARQUET_LIBS); do test -f "$$lib" || exit 1; done || \
		{ echo "Carquet dependencies are missing; run: make -C emu deps"; exit 1; }
	@test -z "$$(find tools/trace/CMakeLists.txt $(CARQUET_SRC) -type f \
		\( -name '*.c' -o -name '*.h' -o -name '*.cmake' -o -name 'CMakeLists.txt' \) \
		-newer "$(CARQUET_STAMP)" -print -quit)" || \
		{ echo "Carquet dependencies are stale; run: make -C emu deps"; exit 1; }

clean-deps:
	rm -rf $(CARQUET_BUILD)

.PHONY: FORCE_GDB_CONFIG
$(BUILD)/gdb_binary.h: FORCE_GDB_CONFIG
	@mkdir -p $(@D)
	@$(PYTHON) -c 'import json, pathlib, sys; p=pathlib.Path(sys.argv[1]); s="#define EMU_GDB_BINARY " + json.dumps(sys.argv[2]) + "\n"; p.write_text(s) if not p.exists() or p.read_text()!=s else None' "$@" "$(GDB_BINARY)"

$(OBJ)/src/gdb.o: $(BUILD)/gdb_binary.h
$(OBJ)/src/gdb.o: src/gdb.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(HOST_INCLUDES) -I$(BUILD) -O2 -c $< -o $@

$(OBJ)/src/sha256.o: src/sha256.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(HOST_INCLUDES) -O3 -funroll-loops -c $< -o $@

$(OBJ)/src/adapters/cemu/%.o: src/adapters/cemu/%.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -DCEMU_INSTRUMENTED=0 -DCEMU_TRACE_PARQUET=0 -O2 -c $< -o $@

$(OBJ)/inst/src/adapters/cemu/%.o: src/adapters/cemu/%.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -DCEMU_INSTRUMENTED=1 -DCEMU_TRACE_PARQUET=1 -O2 -c $< -o $@

$(OBJ)/inst/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -DCEMU_INSTRUMENTED=1 -DCEMU_TRACE_PARQUET=1 -O2 -c $< -o $@

$(DRCOV_ADAPTER_OBJ): $(DRCOV_ADAPTER_SRC)
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -DCEMU_INSTRUMENTED=1 -DCEMU_TRACE_PARQUET=0 -O2 -c $< -o $@

$(OBJ)/tests/test_cemu_drcov_adapter.o: tests/test_cemu_drcov_adapter.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -Itests -DCEMU_INSTRUMENTED=1 -DCEMU_TRACE_PARQUET=0 -O2 -c $< -o $@

$(OBJ)/tests/test_cemu_diagnostics.o: tests/test_cemu_diagnostics.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -Itests -DCEMU_INSTRUMENTED=1 -DCEMU_TRACE_PARQUET=1 -O2 -c $< -o $@

$(OBJ)/tests/test_cemu_core.o: tests/test_cemu_core.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -Itests -DCEMU_INSTRUMENTED=1 -DCEMU_TRACE_PARQUET=1 -O2 -c $< -o $@

$(TRACE_PARQUET_OBJ): src/trace_parquet.c | check-carquet
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(HOST_INCLUDES) $(CARQUET_INCLUDE) -O2 -c $< -o $@

$(OBJ)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(HOST_INCLUDES) -Itests $(TEST_FEATURES) -O2 -c $< -o $@

$(OBJ)/src/adapters/qemu/%.o: src/adapters/qemu/%.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(HOST_INCLUDES) $(GIO_CFLAGS) $(TEST_FEATURES) -O2 -c $< -o $@

$(OBJ)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(HOST_INCLUDES) -O2 -c $< -o $@

$(OBJ)/tests/test_qemu_display.o $(OBJ)/tests/test_qemu_audio.o \
$(OBJ)/tests/test_qemu_adapter.o: $(OBJ)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(HOST_INCLUDES) $(GIO_CFLAGS) -Itests $(TEST_FEATURES) -O2 -c $< -o $@

$(OBJ)/tests/trace_fixture.o: tests/trace_fixture.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(HOST_INCLUDES) -Itests \
		$(TEST_FEATURES) -DCEMU_TRACE_PARQUET=1 -O2 -c $< -o $@

$(OBJ)/tests/test_cemu_profile_plain.o: tests/test_cemu_profile.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -Itests \
		-DCEMU_INSTRUMENTED=0 -DCEMU_TRACE_PARQUET=0 -O2 -c $< -o $@

$(OBJ)/tests/test_cemu_profile_inst.o: tests/test_cemu_profile.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -Itests \
		-DCEMU_INSTRUMENTED=1 -DCEMU_TRACE_PARQUET=1 -O2 -c $< -o $@

$(CEMU_CORE_LIB): $(CEMU_CORE_INPUTS) $(CEMU_ADAPTER_SUPPORT_INPUTS)
	+$(MAKE) -C $(CEMU) core-libs

$(CEMU_CORE_INST_LIB): $(CEMU_CORE_LIB)
	@test -f $@ || $(MAKE) -C $(CEMU) core-libs

$(BIN)/test_host: $(OBJ)/tests/test_host.o $(FAKE_OBJ) $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_contract: $(OBJ)/tests/test_contract.o $(FAKE_OBJ) $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_product: $(OBJ)/tests/test_product.o $(CEMU_ADAPTER_LINK_OBJS)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_eeprom: $(OBJ)/tests/test_eeprom.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_eeprom_trace: $(OBJ)/tests/test_eeprom_trace.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_eeprom_debugger: $(OBJ)/tests/test_eeprom_debugger.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_identity: $(OBJ)/tests/test_identity.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_patch: $(OBJ)/tests/test_patch.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_qemu_snapshot: $(OBJ)/tests/test_qemu_snapshot.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -lm -lpthread -o $@

$(BIN)/test_prepare: $(OBJ)/tests/test_prepare.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_qemu_image: $(OBJ)/tests/test_qemu_image.o \
	$(OBJ)/src/adapters/qemu/image.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_qmp: $(OBJ)/tests/test_qmp.o \
	$(OBJ)/src/adapters/qemu/qmp.o $(OBJ)/src/host.o
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_qemu_display: $(OBJ)/tests/test_qemu_display.o \
	$(OBJ)/src/adapters/qemu/display.o $(OBJ)/src/adapters/qemu/qmp.o \
	$(OBJ)/src/host.o \
	$(OBJ)/src/sha256.o
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(GIO_LIBS) -o $@
$(BIN)/test_qemu_adapter: $(OBJ)/tests/test_qemu_adapter.o \
	$(QEMU_ADAPTER_OBJ) $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(GIO_LIBS) -lz -o $@
$(BIN)/test_qemu_audio: $(OBJ)/tests/test_qemu_audio.o \
	$(OBJ)/src/adapters/qemu/audio.o $(OBJ)/src/adapters/qemu/display.o \
	$(OBJ)/src/adapters/qemu/qmp.o $(OBJ)/src/host.o $(OBJ)/src/sha256.o
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(GIO_LIBS) -o $@
$(BIN)/test_qemu_trace: $(OBJ)/tests/test_qemu_trace.o \
	$(OBJ)/src/adapters/qemu/trace.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_qemu_eeprom_trace: $(OBJ)/tests/test_qemu_eeprom_trace.o \
	$(OBJ)/src/adapters/qemu/eeprom_trace.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@

$(BIN)/test_cemu_adapter: $(OBJ)/tests/test_cemu_adapter.o $(CEMU_ADAPTER_LINK_OBJS)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_cemu_profile_plain: $(OBJ)/tests/test_cemu_profile_plain.o $(CEMU_ADAPTER_LINK_OBJS)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_cemu_profile_inst: $(OBJ)/tests/test_cemu_profile_inst.o $(CEMU_RUNTIME_INST_LINK_OBJS) | check-carquet
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(CARQUET_LIBS) -lm -lpthread -o $@
$(BIN)/test_cemu_diagnostics: $(OBJ)/tests/test_cemu_diagnostics.o $(CEMU_RUNTIME_INST_LINK_OBJS) | check-carquet
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(CARQUET_LIBS) -lm -lpthread -o $@
$(BIN)/test_cemu_core: $(OBJ)/tests/test_cemu_core.o $(CEMU_RUNTIME_INST_LINK_OBJS) | check-carquet
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(CARQUET_LIBS) -lm -lpthread -o $@
$(BIN)/test_trace: $(OBJ)/tests/test_trace.o $(TRACE_PARQUET_OBJ) $(HOST_OBJ) | check-carquet
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(CARQUET_LIBS) -lm -lpthread -o $@
$(BIN)/test_input: $(OBJ)/tests/test_input.o $(OBJ)/src/input.o
	$(CC) $(LDFLAGS) $^ -o $@

$(BIN)/test_runtime: $(OBJ)/tests/test_runtime.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_dispatch: $(OBJ)/tests/test_dispatch.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_ui: $(OBJ)/tests/test_ui.o $(FAKE_OBJ) $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_serial: $(OBJ)/tests/test_serial.o $(FAKE_OBJ) $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_artifact: $(OBJ)/tests/test_artifact.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -lz -o $@
$(BIN)/test_drcov: $(OBJ)/tests/test_drcov.o $(HOST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(BIN)/test_cemu_drcov_adapter: $(OBJ)/tests/test_cemu_drcov_adapter.o $(DRCOV_ADAPTER_OBJ) $(HOST_OBJ) $(CEMU_CORE_INST_LIB)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ -o $@
$(FIXTURE): tests/generate_fixture.py
	@mkdir -p $(@D)
	$(PYTHON) $< $@

test: test-native
	+@$(MAKE) test-tools

test-native: all $(TEST_BINS) $(BIN)/test_cemu_profile_plain \
	$(BIN)/test_cemu_profile_inst $(BIN)/test_cemu_diagnostics \
	$(FIXTURE)
	@$(BIN)/test_host
	@$(BIN)/test_product
	@$(BIN)/test_eeprom
	@$(BIN)/test_eeprom_trace
	@$(BIN)/test_eeprom_debugger
	@$(BIN)/test_identity
	@$(BIN)/test_patch
	@$(BIN)/test_prepare
	@$(BIN)/test_qemu_snapshot
	@$(BIN)/test_qemu_image
	@$(BIN)/test_qmp
	@$(BIN)/test_qemu_display
	@$(BIN)/test_qemu_audio
	@$(BIN)/test_qemu_adapter "$(QEMU_RELEASE_BIN)"
	@$(BIN)/test_qemu_trace
	@$(BIN)/test_qemu_eeprom_trace
	@$(BIN)/test_contract
	@$(BIN)/test_cemu_adapter $(FIXTURE) "$(CANONICAL_FLASH)" "$(S55_FLASH)"
	@$(BIN)/test_cemu_profile_plain
	@$(BIN)/test_cemu_profile_inst
	@$(BIN)/test_cemu_diagnostics $(FIXTURE)
	@$(BIN)/test_cemu_core
	@$(BIN)/test_trace
	@$(BIN)/test_input
	@$(BIN)/test_runtime
	@$(BIN)/test_dispatch
	@$(BIN)/test_ui
	@$(BIN)/test_serial
	@$(BIN)/test_artifact
	@$(BIN)/test_drcov
	@$(BIN)/test_cemu_drcov_adapter

test-tools: $(BIN)/trace_fixture
	@set -e; for suite in $(TOOL_TEST_SUITES); do \
		echo "== $$suite =="; \
		PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m unittest discover \
			-s $$suite -t .; \
	done

$(BIN)/trace_fixture: $(OBJ)/tests/trace_fixture.o $(TRACE_PARQUET_OBJ) \
	$(HOST_OBJ) | check-carquet
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(CARQUET_LIBS) -lm -lpthread -o $@

$(OBJ)/src/launcher.o: src/launcher.c Makefile
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(HOST_INCLUDES) -O2 \
		-DEMU_PLAIN_RUNNER='"$(abspath $(PUBLIC_BIN)/cemu)"' \
		-DEMU_INSTRUMENTED_RUNNER='"$(abspath $(PUBLIC_BIN)/cemu_inst)"' \
		-DEMU_QEMU_BINARY='"$(QEMU_RELEASE_BIN)"' \
		-c $< -o $@

gdb:
	@$(PYTHON) tools/gdb_build.py build --build-dir "$(GDB_BUILD_DIR)" --jobs "$(GDB_JOBS)"

test-gdb:
	@$(PYTHON) -m tools.qualification.gdb.gdb_scripting --gdb "$(GDB_BINARY)" --qemu "$(QEMU_BINARY)" --output "$(GDB_SCRIPTING_OUTPUT)"

.PHONY: test-gdb-handsets
test-gdb-handsets:
	@$(PYTHON) -m tools.qualification.gdb.gdb_handsets --emu "$(EMU_BINARY)" --gdb "$(GDB_BINARY)" --qemu "$(QEMU_BINARY)" --output "$(GDB_HANDSET_OUTPUT)"

test-gdb-memory:
	@$(PYTHON) -m tools.qualification.gdb.gdb_memory --gdb "$(GDB_BINARY)" --qemu "$(QEMU_BINARY)" --output "$(GDB_MEMORY_OUTPUT)"

test-gdb-execution:
	@$(PYTHON) -m tools.qualification.gdb.gdb_execution --gdb "$(GDB_BINARY)" --qemu "$(QEMU_BINARY)" --output "$(GDB_EXECUTION_OUTPUT)"

test-gdb-disassembly:
	@$(PYTHON) -m tools.qualification.gdb.gdb_disassembly --gdb "$(GDB_BINARY)" --qemu "$(QEMU_BINARY)" --output "$(GDB_DISASSEMBLY_OUTPUT)"

test-gdb-registers:
	@$(PYTHON) -m tools.qualification.gdb.gdb_registers --gdb "$(GDB_BINARY)" --qemu "$(QEMU_BINARY)" --output "$(GDB_REGISTER_OUTPUT)"

test-gdb-build:
	@$(PYTHON) tools/gdb_build.py smoke --binary "$(GDB_BINARY)"

qemu-deps:
	$(PYTHON) -m pip install --no-index $(QEMU_ROOT)/python/wheels/meson-*.whl
	cd $(QEMU_ROOT) && $(PYTHON) -m mesonbuild.mesonmain subprojects download \
		keycodemapdb berkeley-softfloat-3 berkeley-testfloat-3

qemu-layout:
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) $(QEMU_PROFILE) check

qemu-debug:
	@mkdir -p $(QEMU_ROOT)/build/debug $(QEMU_ROOT)/build/release
	@if ! PYTHONDONTWRITEBYTECODE=1 $(PYTHON) $(QEMU_PROFILE) check debug; then \
		QEMU_PROFILE_PYTHON=$(PYTHON) PYTHONDONTWRITEBYTECODE=1 \
			$(PYTHON) $(QEMU_PROFILE) configure debug; \
	fi
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) $(QEMU_PROFILE) build debug

qemu-release:
	@mkdir -p $(QEMU_ROOT)/build/debug $(QEMU_ROOT)/build/release
	@if ! PYTHONDONTWRITEBYTECODE=1 $(PYTHON) $(QEMU_PROFILE) check release; then \
		QEMU_PROFILE_PYTHON=$(PYTHON) PYTHONDONTWRITEBYTECODE=1 \
			$(PYTHON) $(QEMU_PROFILE) configure release; \
	fi
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) $(QEMU_PROFILE) build release

qemu-perf: qemu-release all verify-firmware
	@mkdir -p $(QEMU_PERF_ROOT)
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/benchmark.py \
		--qemu $(QEMU_RELEASE_BIN) --flash "$(QEMU_PERF_FLASH)" \
		--scenario qemu-no-sim-throughput \
		--scenario qemu-sim-throughput --warmup 1 --runs 7 \
		--cpu $(QEMU_PERF_CPU) \
		--output $(QEMU_PERF_ROOT)/report.json

qemu-matrix: qemu-release all
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m $(QEMU_MATRIX_RUNNER) \
		--qemu $(QEMU_RELEASE_BIN) --cemu $(abspath $(PUBLIC_BIN)/cemu_inst) \
		--emu $(abspath $(PUBLIC_BIN)/emu) \
		--device "$(QEMU_MATRIX_DEVICES)" --gate "$(QEMU_MATRIX_GATES)" \
		--artifacts $(abspath $(QEMU_MATRIX_ROOT)) --include-alternates \
		--comparison-mode probe

$(PUBLIC_BIN)/emu: $(OBJ)/src/launcher.o \
	$(QEMU_ADAPTER_OBJ) $(HOST_OBJ) \
	$(TRACE_PARQUET_OBJ) | check-carquet
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(GIO_LIBS) $(CARQUET_LIBS) \
		-lm -lpthread -o $@

$(OBJ)/runtime/plain/runner.o: src/runner.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -DCEMU_INSTRUMENTED=0 \
		-DCEMU_TRACE_PARQUET=0 -O2 -c $< -o $@

$(OBJ)/runtime/instrumented/runner.o: src/runner.c
	@mkdir -p $(@D)
	$(CC) $(WARN) $(DEP) $(CEMU_INCLUDES) -DCEMU_INSTRUMENTED=1 \
		-DCEMU_TRACE_PARQUET=1 -O2 -c $< -o $@

$(PUBLIC_BIN)/cemu: $(OBJ)/runtime/plain/runner.o \
	$(CEMU_ADAPTER_LINK_OBJS) $(TRACE_PARQUET_OBJ)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(CARQUET_LIBS) -lm -lpthread -o $@

$(PUBLIC_BIN)/cemu_inst: $(OBJ)/runtime/instrumented/runner.o \
	$(CEMU_RUNTIME_INST_LINK_OBJS)
	@mkdir -p $(@D)
	$(CC) $(GC_LDFLAGS) $^ $(CARQUET_LIBS) -lm -lpthread -o $@

clean:
	rm -rf $(OBJ) $(BIN) $(BUILD)/lib $(BUILD)/bin $(BUILD)/fixtures \
		$(PUBLIC_BIN)/emu $(PUBLIC_BIN)/cemu $(PUBLIC_BIN)/cemu_inst \
		$(PUBLIC_BIN)/recover_fsn

-include $(HOST_OBJ:.o=.d) $(ADAPTER_OBJ:.o=.d) $(TRACE_ADAPTER_OBJ:.o=.d) \
	$(ADAPTER_INST_OBJ:.o=.d) $(DRCOV_ADAPTER_OBJ:.o=.d) \
	$(QEMU_ADAPTER_OBJ:.o=.d) \
	$(TRACE_PARQUET_OBJ:.o=.d) $(OBJ)/src/launcher.d \
	$(OBJ)/runtime/plain/runner.d $(OBJ)/runtime/instrumented/runner.d \
	$(OBJ)/tests/test_cemu_profile_plain.d $(OBJ)/tests/test_cemu_profile_inst.d \
	$(TEST_OBJ:.o=.d)

# Native and Python suites select the same required bundled defaults.
$(BUILD)/fixtures/bundled_firmware.h: ../firmware/manifest.json tools/bundled_firmware.py
	$(PYTHON) tools/bundled_firmware.py --header $@

$(TEST_OBJ): $(BUILD)/fixtures/bundled_firmware.h
HOST_INCLUDES += -I$(BUILD)/fixtures

.PHONY: test-standalone
test-standalone:
	$(PYTHON) tools/bundled_firmware.py
	+$(MAKE) -C $(CEMU) test
	+$(MAKE) test
	$(PYTHON) -m tools.qualification.shared.run_x55_boot --startup-smoke --include-alternates \
		--qemu "$(QEMU_BINARY)" --artifacts shots/standalone-startup

.PHONY: verify-firmware
verify-firmware:
	$(PYTHON) tools/bundled_firmware.py

test-native test-tools: verify-firmware
