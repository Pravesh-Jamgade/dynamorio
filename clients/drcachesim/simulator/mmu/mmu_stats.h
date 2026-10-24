/* **********************************************************
 * Copyright (c) 2015-2026 Google, Inc.  All rights reserved.
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

/* mmu_stats.h: Collects and reports hardware page table walk (PTW), Page Walk
 * Cache (PWC), huge-page (4KB/2MB/1GB), demand page fault, coupled PTW data
 * cache, and TLB shootdown statistics.
 */

#ifndef _DRMEMTRACE_MMU_STATS_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_MMU_STATS_H_ 1 // Define inclusion guard macro.

#include <array>    // Provide std::array for per-level counters.
#include <stdint.h> // Provide int64_t and uint64_t types.
#include <string>   // Provide std::string for formatting output.

#include "mmu/mmu_types.h" // Provide ptw_level_t and page_size_kind_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Object-oriented collector for hardware MMU and Page Table Walker metrics.
 */
class mmu_stats_t {
public:
    /**
     * @brief Constructs a zero-initialized MMU statistics collector.
     */
    mmu_stats_t();

    /**
     * @brief Virtual destructor.
     */
    virtual ~mmu_stats_t() = default;

    /**
     * @brief Records the completion of a hardware page table walk.
     *
     * @param result The completed ptw_result_t describing the walk.
     */
    void
    record_walk(const ptw_result_t &result);

    /**
     * @brief Records a Page Walk Cache (PWC) lookup outcome at a given radix level.
     *
     * @param level The radix level (L4, L3, or L2) queried in the PWC.
     * @param hit   True if the PWC lookup hit; false if it missed.
     */
    void
    record_pwc_access(ptw_level_t level, bool hit);

    /**
     * @brief Records the outcome of an 8-byte PTE memory read issued into the coupled
     *        CPU data cache hierarchy during a page table walk.
     *
     * @param cache_hit True if the PTE read hit in the coupled data cache.
     */
    void
    record_ptw_cache_access(bool cache_hit);

    /**
     * @brief Records the cache hierarchy hit location for an 8-byte PTE memory read
     *        at a specific page table walk radix level.
     *
     * @param ptw_level       The radix level (L4 PML4E down to L1 PTE).
     * @param cache_hit_level The cache hierarchy level (L1D, L2, L3, or DRAM).
     */
    void
    record_ptw_cache_access(ptw_level_t ptw_level, ptw_cache_hit_level_t cache_hit_level);

    /**
     * @brief Records a TLB shootdown / invalidation event.
     */
    void
    record_shootdown()
    {
        ++tlb_shootdowns_; // Increment shootdown counter.
    }

    /**
     * @brief Resets all counters to zero (e.g., when warmup finishes).
     */
    void
    reset();

    /**
     * @brief Prints formatted MMU, PTW, PWC, and huge-page statistics to std::cerr.
     *
     * @param prefix Indentation prefix string for each output line.
     */
    void
    print_stats(const std::string &prefix) const;

    // Accessors for individual statistics counters (useful for unit tests & API).
    int64_t
    get_total_ptw_walks() const
    {
        return total_ptw_walks_; // Return total PTW invocations.
    }

    int64_t
    get_walks_4kb() const
    {
        return walks_4kb_; // Return count of 4 KB page walks.
    }

    int64_t
    get_walks_2mb() const
    {
        return walks_2mb_; // Return count of 2 MB huge-page walks.
    }

    int64_t
    get_walks_1gb() const
    {
        return walks_1gb_; // Return count of 1 GB giant-page walks.
    }

    int64_t
    get_page_faults() const
    {
        return page_faults_; // Return count of first-touch demand page faults.
    }

    int64_t
    get_total_ptw_memory_reads() const
    {
        return total_ptw_memory_reads_; // Return total 8-byte PTE memory reads.
    }

    int64_t
    get_ptw_reads_at_level(ptw_level_t level) const
    {
        int idx = static_cast<int>(level); // Convert level enum to array index.
        if (idx < 1 || idx > 4)
            return 0; // Guard bounds.
        return ptw_reads_per_level_[idx]; // Return reads at that radix level.
    }

    int64_t
    get_pwc_hits() const
    {
        return total_pwc_hits_; // Return total Page Walk Cache hits.
    }

    int64_t
    get_pwc_misses() const
    {
        return total_pwc_misses_; // Return total Page Walk Cache misses.
    }

    int64_t
    get_pwc_hits_at_level(ptw_level_t level) const
    {
        int idx = static_cast<int>(level);
        if (idx < 1 || idx > 4)
            return 0;
        return pwc_hits_per_level_[idx]; // Return PWC hits at `level`.
    }

    int64_t
    get_ptw_cache_hits() const
    {
        return ptw_cache_hits_; // Return coupled data cache hits for PTE reads.
    }

    int64_t
    get_ptw_cache_misses() const
    {
        return ptw_cache_misses_; // Return coupled data cache misses for PTE reads.
    }

    // Returns the count of PTE reads satisfied at `cache_level` for radix `level`.
    int64_t
    get_level_cache_hits(ptw_level_t level, ptw_cache_hit_level_t cache_level) const
    {
        int lvl_idx = static_cast<int>(level); // Convert radix level enum to index.
        int hit_idx = static_cast<int>(cache_level); // Convert cache hit level to index.
        if (lvl_idx < 1 || lvl_idx > 4 || hit_idx < 0 || hit_idx > 3) { // Bounds check.
            return 0; // Return zero on invalid range.
        }
        return ptw_level_cache_hits_[lvl_idx][hit_idx]; // Return hit count.
    }

    // Returns total PTE reads satisfied in L1 data cache.
    int64_t
    get_ptw_cache_hits_l1d() const
    {
        return ptw_cache_hits_l1d_; // Return L1D hits.
    }

    // Returns total PTE reads satisfied in L2 cache.
    int64_t
    get_ptw_cache_hits_l2() const
    {
        return ptw_cache_hits_l2_; // Return L2 hits.
    }

    // Returns total PTE reads satisfied in L3 cache.
    int64_t
    get_ptw_cache_hits_l3() const
    {
        return ptw_cache_hits_l3_; // Return L3 hits.
    }

    // Returns total PTE reads satisfied in DRAM.
    int64_t
    get_ptw_cache_hits_dram() const
    {
        return ptw_cache_hits_dram_; // Return DRAM accesses.
    }

    int64_t
    get_tlb_shootdowns() const
    {
        return tlb_shootdowns_; // Return count of TLB shootdown invalidations.
    }

private:
    int64_t total_ptw_walks_;                   // Total hardware page table walks executed.
    int64_t walks_4kb_;                         // Walks resolving to a 4 KB base page.
    int64_t walks_2mb_;                         // Walks resolving to a 2 MB huge page.
    int64_t walks_1gb_;                         // Walks resolving to a 1 GB giant page.
    int64_t page_faults_;                       // First-touch demand page faults serviced.
    int64_t total_ptw_memory_reads_;            // Total 8-byte PTE memory reads issued.
    std::array<int64_t, 5> ptw_reads_per_level_; // Memory reads indexed by level [1..4].
    int64_t total_pwc_hits_;                    // Total Page Walk Cache hits.
    int64_t total_pwc_misses_;                  // Total Page Walk Cache misses.
    std::array<int64_t, 5> pwc_hits_per_level_; // PWC hits indexed by level [1..4].
    std::array<int64_t, 5> pwc_misses_per_level_; // PWC misses indexed by level [1..4].
    int64_t ptw_cache_hits_;                    // Coupled data cache hits on PTE reads.
    int64_t ptw_cache_misses_;                  // Coupled data cache misses on PTE reads.
    std::array<std::array<int64_t, 4>, 5>
        ptw_level_cache_hits_;                  // Per-level cache hits [level 1..4][cache_level 0..3].
    int64_t ptw_cache_hits_l1d_;                // Total PTE reads satisfied in L1D cache.
    int64_t ptw_cache_hits_l2_;                 // Total PTE reads satisfied in L2 cache.
    int64_t ptw_cache_hits_l3_;                 // Total PTE reads satisfied in L3 cache.
    int64_t ptw_cache_hits_dram_;               // Total PTE reads satisfied in DRAM.
    int64_t tlb_shootdowns_;                    // Total TLB shootdowns processed.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_MMU_STATS_H_ */
