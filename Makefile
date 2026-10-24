# DynamoRIO Out-of-Order CPU, MMU & 3-Level Cache Simulator Makefile
# Provides simple, fast parallel compilation and testing for run_cpu_sim and unit_tests.

# Compiler selection (default: g++).
CXX ?= g++

# Compiler optimization and diagnostic flags.
CXXFLAGS ?= -std=c++14 -O3 -Wall -Wextra -Werror -Wno-unused-parameter -DX86_64 -DLINUX -DARC_64

# Header search directories.
INCLUDES := -Iclients/drcachesim \
            -Iclients/drcachesim/common \
            -Iclients/drcachesim/reader \
            -Iclients/drcachesim/simulator \
            -Iclients/drcachesim/tests \
            -Iext/droption

# Output binary directory.
BIN_DIR := bin

# Intermediate object files directory.
OBJ_DIR := build_sim

# Core simulator source files.
SIM_SRCS := \
    clients/drcachesim/common/trace_entry.cpp \
    clients/drcachesim/simulator/tlb.cpp \
    clients/drcachesim/simulator/tlb_simulator.cpp \
    clients/drcachesim/simulator/simulator.cpp \
    clients/drcachesim/simulator/caching_device.cpp \
    clients/drcachesim/simulator/caching_device_stats.cpp \
    clients/drcachesim/simulator/cache.cpp \
    clients/drcachesim/simulator/cache_lru.cpp \
    clients/drcachesim/simulator/cache_fifo.cpp \
    clients/drcachesim/simulator/cache_stats.cpp \
    clients/drcachesim/simulator/prefetcher.cpp \
    clients/drcachesim/simulator/snoop_filter.cpp \
    clients/drcachesim/simulator/mmu/physical_frame_allocator.cpp \
    clients/drcachesim/simulator/mmu/huge_page_detector.cpp \
    clients/drcachesim/simulator/mmu/page_table.cpp \
    clients/drcachesim/simulator/mmu/page_walk_cache.cpp \
    clients/drcachesim/simulator/mmu/mmu_stats.cpp \
    clients/drcachesim/simulator/mmu/page_table_walker.cpp \
    clients/drcachesim/simulator/mmu/performance_model.cpp \
    clients/drcachesim/simulator/cpu/three_level_cache.cpp \
    clients/drcachesim/simulator/cpu/btb.cpp \
    clients/drcachesim/simulator/cpu/reg_deps.cpp \
    clients/drcachesim/simulator/cpu/rob.cpp \
    clients/drcachesim/simulator/cpu/lsq.cpp \
    clients/drcachesim/simulator/cpu/wrong_path.cpp \
    clients/drcachesim/simulator/cpu/cpu_core.cpp \
    clients/drcachesim/simulator/cpu/cpu_simulator.cpp \
    clients/drcachesim/reader/v2p_reader.cpp \
    clients/drcachesim/reader/champsim_reader.cpp

# Map each source file to an object file in OBJ_DIR.
SIM_OBJS := $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(SIM_SRCS))

# CLI runner target binary.
RUN_CPU_SIM_BIN := $(BIN_DIR)/run_cpu_sim
# CLI runner main source file.
RUN_CPU_SIM_SRC := clients/drcachesim/simulator/cpu/run_cpu_sim.cpp
# CLI runner object file.
RUN_CPU_SIM_OBJ := $(OBJ_DIR)/clients/drcachesim/simulator/cpu/run_cpu_sim.o

# Unit test target binary.
TEST_BIN := $(BIN_DIR)/unit_tests
# Unit test suite implementation sources.
TEST_SRCS := clients/drcachesim/tests/tlb_simulator_unit_test.cpp \
             clients/drcachesim/tests/run_tests_main.cpp
# Unit test object files.
TEST_OBJS := $(patsubst %.cpp, $(OBJ_DIR)/%.o, $(TEST_SRCS))

# Phony targets declaration.
.PHONY: all sim test clean help

# Default target building both the simulator CLI and unit tests.
all: $(RUN_CPU_SIM_BIN) $(TEST_BIN)
	@echo "Build complete! Simulator: $(RUN_CPU_SIM_BIN), Tests: $(TEST_BIN)"

# Target building only the standalone simulator CLI binary.
sim: $(RUN_CPU_SIM_BIN)

# Target building and executing the comprehensive test suite.
test: $(TEST_BIN)
	@$(TEST_BIN) clients/drcachesim/tests

# Rule to link the run_cpu_sim executable.
$(RUN_CPU_SIM_BIN): $(RUN_CPU_SIM_OBJ) $(SIM_OBJS)
	@mkdir -p $(BIN_DIR)
	@echo "[Linking] $@"
	@$(CXX) $(CXXFLAGS) $^ -o $@

# Rule to link the unit_tests executable.
$(TEST_BIN): $(TEST_OBJS) $(SIM_OBJS)
	@mkdir -p $(BIN_DIR)
	@echo "[Linking] $@"
	@$(CXX) $(CXXFLAGS) $^ -o $@

# Pattern rule compiling C++ sources into object files with dependency directory creation.
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "[Compiling] $<"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Target to remove all generated object files and binaries.
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(OBJ_DIR) $(BIN_DIR)
	@echo "Clean complete."

# Target displaying available Makefile commands.
help:
	@echo "Available make targets:"
	@echo "  make              - Build both run_cpu_sim and unit_tests"
	@echo "  make sim          - Build standalone simulator (bin/run_cpu_sim)"
	@echo "  make test         - Build and run unit test suite"
	@echo "  make clean        - Remove all compiled objects and binaries"
	@echo "  make -j           - Compile in parallel using multiple cores"
