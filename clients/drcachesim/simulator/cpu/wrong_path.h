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

/* wrong_path.h: Speculative Wrong-Path execution engine modeling instruction
 * fetching, decoding, register allocation, cache pollution, and LSQ occupancy
 * along mispredicted branch paths before pipeline squash.
 */

#ifndef _DRMEMTRACE_WRONG_PATH_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_WRONG_PATH_H_ 1 // Define inclusion guard macro.

// Provide standard integer types.
#include <stdint.h>
// Provide dynamic array container.
#include <vector>
// Provide standard string class.
#include <string>
// Provide standard output streams.
#include <iostream>

// Provide CPU types and dynamic instruction representations.
#include "cpu/cpu_types.h"
// Provide 3-level cache hierarchy interface.
#include "cpu/three_level_cache.h"
// Provide TLB interface.
#include "tlb.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Statistics collected for speculative wrong-path execution.
 */
struct wrong_path_stats_t {
    int64_t mispredictions_handled = 0;       // Total branch mispredictions modeled.
    int64_t instructions_fetched = 0;         // Speculative instructions fetched on wrong path.
    int64_t instructions_squashed = 0;        // Speculative instructions squashed from ROB.
    int64_t loads_executed = 0;               // Speculative loads executed.
    int64_t stores_buffered = 0;              // Speculative stores buffered in SQ (never committed).
    int64_t cache_accesses = 0;               // Speculative memory accesses issued to cache.
    int64_t cache_hits = 0;                   // Speculative accesses that hit in cache.
    int64_t cache_misses = 0;                 // Speculative accesses that missed and polluted cache.
};

/**
 * @brief Configuration parameters for wrong-path simulation.
 */
struct wrong_path_knobs_t {
    wrong_path_knobs_t()
        : enable_wrong_path(true)             // Enable wrong-path execution by default.
        , max_speculative_depth(32)           // Maximum instructions fetched on wrong path.
        , load_probability(0.25)              // Probability an instruction is a load (25%).
        , store_probability(0.15)             // Probability an instruction is a store (15%).
        , branch_probability(0.20)            // Probability an instruction is an inner branch (20%).
    {
    }

    bool enable_wrong_path;                   // Whether to execute along mispredicted paths.
    size_t max_speculative_depth;             // Max instructions dispatched before squash.
    double load_probability;                  // Fraction of speculative instructions that are loads.
    double store_probability;                 // Fraction of speculative instructions that are stores.
    double branch_probability;                // Fraction of speculative instructions that are branches.
};

/**
 * @brief Engine that generates and executes instructions along mispredicted branch paths.
 */
class speculative_wrong_path_engine_t {
public:
    /**
     * @brief Constructs the wrong-path engine.
     *
     * @param knobs Configuration parameters.
     */
    explicit speculative_wrong_path_engine_t(
        const wrong_path_knobs_t &knobs = wrong_path_knobs_t());

    /**
     * @brief Generates a sequence of speculative wrong-path instructions starting from
     *        the mispredicted target or fallthrough PC.
     *
     * @param branch_seq_id   Sequence ID of the mispredicted branch instruction.
     * @param branch_pc       PC of the mispredicted branch.
     * @param pred_info       Outcome of branch prediction.
     * @param fetch_width     Number of instructions fetched per cycle.
     * @param penalty_cycles  Cycles until branch resolution and pipeline squash.
     * @param pid             Process ID.
     * @param tid             Thread ID.
     * @param next_seq_id     In-out sequence ID counter for assigning instruction IDs.
     * @return Ordered vector of speculative wrong-path dyn_instruction_t records.
     */
    std::vector<dyn_instruction_t>
    generate_wrong_path_stream(uint64_t branch_seq_id,
                               addr_t branch_pc,
                               const branch_prediction_t &pred_info,
                               size_t fetch_width,
                               uint64_t penalty_cycles,
                               memref_pid_t pid,
                               memref_tid_t tid,
                               uint64_t &next_seq_id);

    /**
     * @brief Simulates speculative memory hierarchy side effects (cache and TLB pollution)
     *        for a wrong-path load instruction.
     *
     * @param core_id         CPU core ID.
     * @param load_instr      Speculative load instruction.
     * @param cache_hierarchy Pointer to 3-level data cache hierarchy.
     * @param dtlb            Pointer to L1 Data TLB.
     */
    void
    simulate_speculative_load(unsigned int core_id,
                              const dyn_instruction_t &load_instr,
                              three_level_cache_t *cache_hierarchy,
                              tlb_t *dtlb);

    /**
     * @brief Records that speculative wrong-path instructions were squashed upon resolution.
     *
     * @param count Number of instructions squashed from the ROB.
     */
    void
    record_squash(size_t count);

    /**
     * @brief Prints wrong-path statistics to an output stream.
     *
     * @param os Output stream.
     * @param prefix Line indentation prefix.
     */
    void
    print_stats(std::ostream &os, const std::string &prefix = "") const;

    /**
     * @brief Resets all statistics counters.
     */
    void
    reset();

    // Query methods.
    const wrong_path_stats_t &get_stats() const { return stats_; }
    bool is_enabled() const { return knobs_.enable_wrong_path; }

private:
    wrong_path_knobs_t knobs_;      // Configuration knobs.
    wrong_path_stats_t stats_;      // Accumulated metrics.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_WRONG_PATH_H_ */
