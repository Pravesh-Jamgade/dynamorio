/* **********************************************************
 * Copyright (c) 2026 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* cpu_core.h: Complete Out-of-Order (OoO) superscalar CPU core simulator coupling
 * Front-End (Fetch, Decode, BTB/Branch Predictor), Reorder Buffer (ROB), Register
 * Scoreboard (RAW dependencies), Load-Store Queue (LSQ with store forwarding),
 * TLBs, hardware Page Table Walker, and 3-Level Data Cache hierarchy.
 */

#ifndef _DRMEMTRACE_CPU_CORE_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_CPU_CORE_H_ 1 // Define inclusion guard macro.

// Provide standard integer types.
#include <stdint.h>
// Provide dynamic array container.
#include <vector>
// Provide standard string class.
#include <string>
// Provide standard output streams.
#include <iostream>

// Provide CPU types and pipeline definitions.
#include "cpu/cpu_types.h"
// Provide Branch Target Buffer and branch predictor.
#include "cpu/btb.h"
// Provide Register Dependency Scoreboard.
#include "cpu/reg_deps.h"
// Provide Reorder Buffer.
#include "cpu/rob.h"
// Provide Load-Store Queue.
#include "cpu/lsq.h"
// Provide Speculative Wrong-Path execution engine.
#include "cpu/wrong_path.h"
// Provide 3-Level Cache hierarchy.
#include "cpu/three_level_cache.h"
// Provide TLB interfaces.
#include "tlb.h"
// Provide hardware Page Table Walker.
#include "mmu/page_table_walker.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Configuration parameters for an Out-of-Order CPU core.
 */
struct cpu_core_knobs_t {
    // Default constructor configuring a modern 4-wide out-of-order pipeline.
    cpu_core_knobs_t()
        : core_id(0)                           // Hardware core index.
        , fetch_width(4)                       // Max instructions fetched per cycle.
        , decode_width(4)                      // Max instructions decoded per cycle.
        , dispatch_width(4)                    // Max instructions dispatched to ROB per cycle.
        , commit_width(4)                      // Max instructions retired from ROB per cycle.
        , rob_capacity(256)                    // Capacity of the Reorder Buffer.
        , lq_capacity(72)                      // Capacity of the Load Queue.
        , sq_capacity(56)                      // Capacity of the Store Queue.
        , btb_entries(1024)                    // Branch Target Buffer slots.
        , branch_mispredict_penalty(14)        // Cycles lost on pipeline flush after misprediction.
        , alu_latency(1)                       // ALU execution latency in cycles.
        , enable_wrong_path(true)              // Enable speculative wrong-path execution.
        , max_speculative_depth(32)            // Max instructions fetched along wrong path.
    {
    }

    unsigned int core_id;                      // Core identifier.
    size_t fetch_width;                        // Peak instruction fetch rate.
    size_t decode_width;                       // Peak instruction decode rate.
    size_t dispatch_width;                     // Peak instruction dispatch rate.
    size_t commit_width;                       // Peak in-order retirement rate.
    size_t rob_capacity;                       // Reorder Buffer entry limit.
    size_t lq_capacity;                        // Load Queue entry limit.
    size_t sq_capacity;                        // Store Queue entry limit.
    size_t btb_entries;                        // Number of BTB entries.
    uint64_t branch_mispredict_penalty;        // Pipeline flush latency in cycles.
    uint64_t alu_latency;                      // Fixed execution latency for general integer ops.
    bool enable_wrong_path;                    // Whether to simulate speculative wrong-path execution.
    size_t max_speculative_depth;              // Maximum speculative instructions fetched on wrong path.
};

/**
 * @brief Out-of-Order CPU Core pipeline simulator.
 */
class cpu_core_t {
public:
    /**
     * @brief Constructs an Out-of-Order CPU Core simulator.
     *
     * @param knobs Configuration parameters.
     * @param cache_hierarchy Pointer to the unified 3-level cache hierarchy (or nullptr).
     * @param itlb Pointer to core's L1 Instruction TLB (or nullptr).
     * @param dtlb Pointer to core's L1 Data TLB (or nullptr).
     * @param l2_tlb Pointer to core's unified L2 TLB (or nullptr).
     * @param ptw Pointer to core's hardware Page Table Walker (or nullptr).
     */
    cpu_core_t(const cpu_core_knobs_t &knobs,
               three_level_cache_t *cache_hierarchy = nullptr,
               tlb_t *itlb = nullptr,
               tlb_t *dtlb = nullptr,
               tlb_t *l2_tlb = nullptr,
               page_table_walker_t *ptw = nullptr);

    /**
     * @brief Destructor.
     */
    virtual ~cpu_core_t() = default;

    /**
     * @brief Ingests and processes a single trace memory reference through the OoO pipeline.
     *
     * @param memref Trace memory reference record.
     * @return True on success; false on failure.
     */
    bool
    process_memref(const memref_t &memref);

    /**
     * @brief Advances simulation time to drain all remaining in-flight instructions from
     *        the ROB, LSQ, and cache hierarchy upon end of trace.
     */
    void
    flush_and_drain_pipeline();

    /**
     * @brief Prints comprehensive CPU core, front-end, ROB, LSQ, and performance statistics.
     *
     * @param os Output stream (e.g. std::cerr).
     * @param prefix Line indentation prefix string.
     */
    void
    print_stats(std::ostream &os, const std::string &prefix = "") const;

    /**
     * @brief Resets all pipeline structures, queues, tables, and statistics.
     */
    void
    reset();

    // Query methods.
    uint64_t get_current_cycle() const { return current_cycle_; }
    int64_t total_instructions_retired() const { return total_instructions_retired_; }
    int64_t icache_miss_stall_cycles() const { return icache_miss_stall_cycles_; }
    double get_ipc() const;
    double get_cpi() const;

    // Structure accessors.
    branch_predictor_t &get_branch_predictor() { return branch_predictor_; }
    rob_t &get_rob() { return rob_; }
    lsq_t &get_lsq() { return lsq_; }
    register_scoreboard_t &get_scoreboard() { return scoreboard_; }
    speculative_wrong_path_engine_t &get_wrong_path_engine() { return wrong_path_engine_; }

private:
    // Dispatches an instruction into the ROB, scoreboard, and LSQ, advancing cycles if stalled.
    void
    dispatch_instruction(dyn_instruction_t &instr);

    // Executes dynamic instruction: evaluates dependencies, memory hierarchy, and writeback.
    void
    execute_instruction(dyn_instruction_t &instr);

    // Advances the simulation clock by `num_cycles`, retiring ready instructions from the ROB.
    void
    step_cycles(uint64_t num_cycles = 1);

    cpu_core_knobs_t knobs_;                   // Core configuration parameters.
    three_level_cache_t *cache_hierarchy_;     // 3-level data cache hierarchy.
    tlb_t *itlb_;                              // L1 Instruction TLB.
    tlb_t *dtlb_;                              // L1 Data TLB.
    tlb_t *l2_tlb_;                            // Unified L2 TLB.
    page_table_walker_t *ptw_;                 // Hardware Page Table Walker.

    branch_predictor_t branch_predictor_;      // Front-End BTB & Branch Predictor.
    register_scoreboard_t scoreboard_;         // Register Dependency Scoreboard.
    rob_t rob_;                                // Reorder Buffer.
    lsq_t lsq_;                                // Load-Store Queue.
    speculative_wrong_path_engine_t wrong_path_engine_; // Speculative wrong-path engine.

    uint64_t current_cycle_ = 1;               // Current processor clock cycle.
    uint64_t next_seq_id_ = 1;                 // Sequence ID counter for dynamic instructions.
    uint64_t branch_recovery_until_cycle_ = 0; // Front-End stall horizon during branch flush.
    addr_t last_pc_ = 0;                       // Previous instruction PC for branch ground truth.

    // Statistics counters.
    int64_t total_instructions_fetched_ = 0;   // Instructions fetched into pipeline.
    int64_t total_instructions_retired_ = 0;   // Instructions committed from the ROB.
    int64_t total_loads_ = 0;                  // Total load instructions executed.
    int64_t total_stores_ = 0;                 // Total store instructions executed.
    int64_t branch_squash_cycles_ = 0;         // Cycles lost to branch misprediction flushes.
    int64_t icache_miss_stall_cycles_ = 0;     // Front-End stall cycles due to L1I cache misses.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_CPU_CORE_H_ */
