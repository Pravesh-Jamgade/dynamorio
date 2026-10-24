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

/* performance_model.h: Object-oriented microarchitectural timing and stall
 * model that calculates total instructions, cache stall cycles, TLB/PTW stall
 * cycles, total execution cycles, CPI_cache_stalls, CPI_tlb_stalls, total CPI,
 * and IPC (instructions per cycle).
 */

#ifndef _DRMEMTRACE_PERFORMANCE_MODEL_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_PERFORMANCE_MODEL_H_ 1 // Define inclusion guard macro.

// Provide standard 64-bit integer types.
#include <stdint.h>
// Provide standard string representation.
#include <string>
// Provide standard I/O streaming.
#include <iostream>

// Provide MMU statistics definitions.
#include "mmu/mmu_stats.h"
// Provide caching device statistics definitions.
#include "caching_device_stats.h"
// Provide TLB statistics definitions.
#include "tlb_stats.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Microarchitectural latency configuration parameters for CPI/IPC modeling.
 */
struct performance_params_t {
    // Default constructor with standard modern processor microarchitecture latencies.
    performance_params_t()
        : cpi_base(0.5)               // Default ideal superscalar CPI without memory stalls.
        , l2_tlb_hit_latency(9)       // Latency to access the unified L2 TLB on an L1 miss.
        , pwc_hit_latency(4)          // Latency for a Page Walk Cache directory hit.
        , dram_latency(150)           // Latency for an uncached main memory DRAM read.
        , dcache_hit_latency(4)       // Latency for a coupled L1 data cache hit.
        , dcache_miss_latency(150)    // Latency for a data cache miss requiring DRAM.
    {
    }

    double cpi_base;                  // Ideal execution CPI (no memory or TLB stalls).
    uint64_t l2_tlb_hit_latency;      // Latency in cycles to hit in the unified L2 TLB.
    uint64_t pwc_hit_latency;         // Latency in cycles to hit in the Page Walk Cache.
    uint64_t dram_latency;            // Latency in cycles to read an uncached PTE from DRAM.
    uint64_t dcache_hit_latency;      // Latency in cycles to read a PTE hit from L1 D-Cache.
    uint64_t dcache_miss_latency;     // Latency in cycles when coupled D-Cache misses.
};

/**
 * @brief Computed microarchitectural performance metrics for a simulated CPU core.
 */
struct performance_metrics_t {
    int64_t total_instructions = 0;   // Total instructions executed on the core.
    int64_t total_cycles = 0;         // Total simulated processor execution cycles.
    int64_t base_cycles = 0;          // Ideal execution cycles without memory stalls.
    int64_t cache_stall_cycles = 0;   // Cycles lost to data/instruction cache stalls.
    int64_t tlb_stall_cycles = 0;     // Cycles lost to L1 TLB misses & page table walks.
    double cpi_base = 0.0;            // Ideal base CPI component.
    double cpi_cache_stalls = 0.0;    // Cache memory stall cycles per instruction.
    double cpi_tlb_stalls = 0.0;      // TLB and Page Table Walk stall cycles per instruction.
    double cpi_total = 0.0;           // Total CPI = cpi_base + cpi_cache_stalls + cpi_tlb_stalls.
    double ipc = 0.0;                 // Instructions Per Cycle = total_instructions / total_cycles.
};

/**
 * @brief Performance model calculator that synthesizes cache, TLB, and MMU metrics.
 */
class performance_model_t {
public:
    /**
     * @brief Constructs a performance model with default or user-specified latencies.
     *
     * @param params Latency and base CPI configuration parameters.
     */
    explicit performance_model_t(const performance_params_t &params = performance_params_t());

    /**
     * @brief Virtual destructor.
     */
    virtual ~performance_model_t() = default;

    /**
     * @brief Computes CPI, IPC, and stall cycle breakdowns for a core.
     *
     * @param total_instructions Total instruction count executed by the core.
     * @param l1i_tlb_stats      Pointer to core's L1 Instruction TLB stats (or nullptr).
     * @param l1d_tlb_stats      Pointer to core's L1 Data TLB stats (or nullptr).
     * @param l2_tlb_stats       Pointer to core's unified L2 TLB stats (or nullptr).
     * @param mmu_stats          Pointer to core's hardware MMU/PTW stats (or nullptr).
     * @param dcache_stats       Pointer to core's coupled data cache stats (or nullptr).
     * @return Fully populated performance_metrics_t struct.
     */
    performance_metrics_t
    evaluate(int64_t total_instructions,
             const tlb_stats_t *l1i_tlb_stats,
             const tlb_stats_t *l1d_tlb_stats,
             const tlb_stats_t *l2_tlb_stats,
             const mmu_stats_t *mmu_stats,
             const caching_device_stats_t *dcache_stats) const;

    /**
     * @brief Formats and prints performance and stall metrics to an output stream.
     *
     * @param os     Target output stream (e.g. std::cerr).
     * @param prefix Line indentation prefix (e.g. "    ").
     * @param m      The computed performance metrics to print.
     */
    void
    print_metrics(std::ostream &os, const std::string &prefix,
                  const performance_metrics_t &m) const;

    /**
     * @brief Returns the current configuration parameters.
     */
    const performance_params_t &
    get_params() const
    {
        return params_; // Return active latency configuration.
    }

    /**
     * @brief Updates the latency configuration parameters.
     */
    void
    set_params(const performance_params_t &params)
    {
        params_ = params; // Overwrite active latency configuration.
    }

private:
    performance_params_t params_; // Active microarchitectural latency parameters.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_PERFORMANCE_MODEL_H_ */
