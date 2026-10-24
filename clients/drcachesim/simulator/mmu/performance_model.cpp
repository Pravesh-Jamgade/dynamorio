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

// Implementation header.
#include "mmu/performance_model.h"

// Provide std::round and floating-point math routines.
#include <cmath>
// Provide std::fixed, std::setprecision, and std::setw formatters.
#include <iomanip>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

performance_model_t::performance_model_t(const performance_params_t &params)
    // Initialize active latency parameters.
    : params_(params)
{
    // Constructor body.
}

performance_metrics_t
performance_model_t::evaluate(int64_t total_instructions,
                              const tlb_stats_t *l1i_tlb_stats,
                              const tlb_stats_t *l1d_tlb_stats,
                              const tlb_stats_t *l2_tlb_stats,
                              const mmu_stats_t *mmu_stats,
                              const caching_device_stats_t *dcache_stats) const
{
    // Initialize output metrics structure.
    performance_metrics_t m;

    // If explicit instruction count is zero or negative, attempt to infer from L1I TLB.
    if (total_instructions <= 0 && l1i_tlb_stats != nullptr) {
        // Total instruction fetches equals L1I hits plus misses.
        total_instructions = l1i_tlb_stats->get_metric(metric_name_t::HITS) +
            l1i_tlb_stats->get_metric(metric_name_t::MISSES);
    }
    // Store resolved total instruction count.
    m.total_instructions = total_instructions;

    // 1. Calculate Cache Stall Cycles:
    if (dcache_stats != nullptr) {
        // Misses in the coupled/attached data cache incur main memory DRAM latency.
        int64_t dcache_misses = dcache_stats->get_metric(metric_name_t::MISSES);
        // Accumulate data cache stall cycles.
        m.cache_stall_cycles = dcache_misses * static_cast<int64_t>(params_.dcache_miss_latency);
    }

    // 2. Calculate TLB Stall Cycles:
    int64_t l1_tlb_misses = 0; // Total misses across L1I and L1D TLBs.
    if (l1i_tlb_stats != nullptr) {
        // Accumulate L1 instruction TLB misses.
        l1_tlb_misses += l1i_tlb_stats->get_metric(metric_name_t::MISSES);
    }
    if (l1d_tlb_stats != nullptr) {
        // Accumulate L1 data TLB misses.
        l1_tlb_misses += l1d_tlb_stats->get_metric(metric_name_t::MISSES);
    }
    // Each L1 TLB miss accesses the unified L2 TLB.
    int64_t l1_miss_stall_cycles = l1_tlb_misses * static_cast<int64_t>(params_.l2_tlb_hit_latency);

    // Stalls caused by hardware page table walks on L2 TLB misses.
    int64_t ptw_stall_cycles = 0;
    if (mmu_stats != nullptr) {
        // Query Page Walk Cache hits.
        int64_t pwc_hits = mmu_stats->get_pwc_hits();
        // Query coupled data-cache hits for PTE reads.
        int64_t ptw_cache_hits = mmu_stats->get_ptw_cache_hits();
        // Query coupled data-cache misses for PTE reads.
        int64_t ptw_cache_misses = mmu_stats->get_ptw_cache_misses();
        // Query total 8-byte PTE memory reads issued during walks.
        int64_t ptw_mem_reads = mmu_stats->get_total_ptw_memory_reads();

        // PWC hits incur low-latency directory lookups.
        ptw_stall_cycles += pwc_hits * static_cast<int64_t>(params_.pwc_hit_latency);

        // If coupled data-cache simulation is active:
        if (ptw_cache_hits > 0 || ptw_cache_misses > 0) {
            // PTE reads that hit in the coupled L1 data cache incur L1 hit latency.
            ptw_stall_cycles += ptw_cache_hits * static_cast<int64_t>(params_.dcache_hit_latency);
            // PTE reads that miss in the coupled data cache incur DRAM latency.
            ptw_stall_cycles += ptw_cache_misses * static_cast<int64_t>(params_.dram_latency);
        } else {
            // Uncached PTW: each physical PTE memory read incurs DRAM access latency.
            ptw_stall_cycles += ptw_mem_reads * static_cast<int64_t>(params_.dram_latency);
        }
    } else if (l2_tlb_stats != nullptr) {
        // Fallback when MMU walker is not enabled: estimate 4-step walk to DRAM per L2 miss.
        int64_t l2_misses = l2_tlb_stats->get_metric(metric_name_t::MISSES);
        // Estimate 4 DRAM accesses per compulsory/capacity L2 TLB miss.
        ptw_stall_cycles = l2_misses * 4 * static_cast<int64_t>(params_.dram_latency);
    }

    // Total TLB stall cycles is L1 lookup penalty plus hardware PTW walk penalty.
    m.tlb_stall_cycles = l1_miss_stall_cycles + ptw_stall_cycles;

    // 3. Calculate Base Execution Cycles:
    if (m.total_instructions > 0) {
        // Ideal processor cycles spent retiring instructions without stalls.
        m.base_cycles = static_cast<int64_t>(
            std::round(static_cast<double>(m.total_instructions) * params_.cpi_base));
    }

    // 4. Calculate Total Processor Cycles:
    m.total_cycles = m.base_cycles + m.cache_stall_cycles + m.tlb_stall_cycles;

    // 5. Calculate CPI and IPC Ratios:
    if (m.total_instructions > 0) {
        // Record ideal base CPI component.
        m.cpi_base = params_.cpi_base;
        // Compute cache stall CPI.
        m.cpi_cache_stalls = static_cast<double>(m.cache_stall_cycles) /
            static_cast<double>(m.total_instructions);
        // Compute TLB stall CPI.
        m.cpi_tlb_stalls = static_cast<double>(m.tlb_stall_cycles) /
            static_cast<double>(m.total_instructions);
        // Compute total CPI.
        m.cpi_total = m.cpi_base + m.cpi_cache_stalls + m.cpi_tlb_stalls;
        // Compute Instructions Per Cycle (IPC = 1 / CPI).
        if (m.total_cycles > 0) {
            m.ipc = static_cast<double>(m.total_instructions) /
                static_cast<double>(m.total_cycles);
        }
    }

    // Return the completed performance breakdown.
    return m;
}

void
performance_model_t::print_metrics(std::ostream &os, const std::string &prefix,
                                   const performance_metrics_t &m) const
{
    // Set width and formatting.
    os << prefix << "Performance & Timing model:\n";
    os << prefix << "  Total instructions:            " << std::setw(12)
       << m.total_instructions << "\n";
    os << prefix << "  Total cycles:                  " << std::setw(12)
       << m.total_cycles << "\n";
    os << prefix << "  Base execution cycles:         " << std::setw(12)
       << m.base_cycles << "\n";
    os << prefix << "  Cache stall cycles:            " << std::setw(12)
       << m.cache_stall_cycles << "\n";
    os << prefix << "  TLB stall cycles:              " << std::setw(12)
       << m.tlb_stall_cycles << "\n";

    // Set floating-point formatting.
    os << std::fixed << std::setprecision(4);
    os << prefix << "  CPI base:                      " << std::setw(12)
       << m.cpi_base << "\n";
    os << prefix << "  CPI cache stalls:              " << std::setw(12)
       << m.cpi_cache_stalls << "\n";
    os << prefix << "  CPI TLB stalls:                " << std::setw(12)
       << m.cpi_tlb_stalls << "\n";
    os << prefix << "  Total CPI:                     " << std::setw(12)
       << m.cpi_total << "\n";
    os << prefix << "  IPC (Instructions Per Cycle):  " << std::setw(12)
       << m.ipc << "\n";
}

} // namespace drmemtrace
} // namespace dynamorio
