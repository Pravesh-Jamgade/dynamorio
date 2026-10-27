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

/* page_table_walker.h: Models a per-core hardware Page Table Walker (PTW)
 * that resolves L2 TLB misses by probing the Page Walk Cache (PWC), walking
 * the multi-level radix page table (L4 -> L3 -> L2 [-> L1]), and optionally
 * issuing 8-byte physical PTE memory reads into a coupled CPU data cache.
 */

#ifndef _DRMEMTRACE_PAGE_TABLE_WALKER_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_PAGE_TABLE_WALKER_H_ 1 // Define inclusion guard macro.

#include "caching_device.h"         // Provide caching_device_t for coupled cache reads.
#include "mmu/mmu_stats.h"          // Provide mmu_stats_t.
#include "mmu/mmu_types.h"          // Provide ptw_result_t and ptw_level_t.
#include "mmu/page_table.h"         // Provide mmu_address_space_manager_t.
#include "mmu/page_walk_cache.h"    // Provide page_walk_cache_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Object-oriented per-core Hardware Page Table Walker (PTW).
 *
 * When a core's last-level (L2) TLB misses on a memory access, `page_table_walker_t`
 * performs an architectural radix page table walk:
 *   1. Probes the per-core `page_walk_cache_t` from the deepest intermediate level
 *      upward (L2 PDE -> L3 PDPTE -> L4 PML4E) to see if upper levels of the walk
 *      can be bypassed.
 *   2. For each uncached radix level down to the terminal leaf (L3 for 1 GB, L2 for
 *      2 MB, or L1 for 4 KB):
 *      - Issues an 8-byte physical `TRACE_TYPE_READ` for `entry_paddr` into the
 *        attached CPU data cache (`ptw_data_cache_`) if coupled cache simulation is
 *        enabled.
 *      - Inserts newly resolved intermediate directory pointers into the PWC.
 *   3. Records comprehensive PTW, PWC, huge-page, and cache hit/miss metrics in
 *      `mmu_stats_t`.
 */
class page_table_walker_t {
public:
    /**
     * @brief Constructs a hardware page table walker for a specific CPU core.
     *
     * @param core_id           Zero-based index of the simulated CPU core.
     * @param address_space_mgr Shared multi-process radix page table manager.
     * @param pwc_entries       Capacity of the core's Page Walk Cache (0 disables PWC).
     * @param pwc_assoc         Associativity of the core's Page Walk Cache.
     * @param ptw_data_cache    Optional pointer to the core's data cache for PTE reads.
     */
    page_table_walker_t(int core_id, mmu_address_space_manager_t *address_space_mgr,
                        unsigned int pwc_entries = 32, unsigned int pwc_assoc = 4,
                        caching_device_t *ptw_data_cache = nullptr);

    /**
     * @brief Virtual destructor.
     */
    virtual ~page_table_walker_t() = default;

    /**
     * @brief Attaches or updates the CPU data cache device that receives 8-byte
     *        physical PTE read requests during page table walks.
     */
    void
    set_ptw_data_cache(caching_device_t *ptw_data_cache)
    {
        ptw_data_cache_ = ptw_data_cache; // Store pointer to coupled data cache.
    }

    /**
     * @brief Returns the currently attached PTW data cache (or nullptr if uncoupled).
     */
    caching_device_t *
    get_ptw_data_cache() const
    {
        return ptw_data_cache_; // Return coupled data cache pointer.
    }

    /**
     * @brief Executes a hardware page table walk for the given memory reference.
     *
     * @param memref     The trace memory reference (instruction or data access).
     * @param vaddr      The specific virtual address to translate.
     * @param result_out Optional output structure receiving detailed walk results.
     * @return True if the page table walk succeeded.
     */
    bool
    walk(const memref_t &memref, addr_t vaddr, ptw_result_t *result_out = nullptr);

    /**
     * @brief Handles a TLB shootdown / invalidation on this core's Page Walk Cache
     *        and records the shootdown in `stats_`.
     *
     * @param pid   Process ID whose translation is being invalidated.
     * @param vaddr Virtual address being invalidated.
     */
    void
    handle_shootdown(memref_pid_t pid, addr_t vaddr);

    /**
     * @brief Flushes all PWC entries for `pid` on this core.
     */
    void
    flush_pid(memref_pid_t pid);

    // Returns a mutable reference to this walker's Page Walk Cache.
    page_walk_cache_t &
    get_pwc()
    {
        return pwc_; // Return mutable PWC reference.
    }

    // Returns a const reference to this walker's Page Walk Cache.
    const page_walk_cache_t &
    get_pwc() const
    {
        return pwc_; // Return const PWC reference.
    }

    // Returns a mutable reference to this walker's MMU statistics collector.
    mmu_stats_t &
    get_stats()
    {
        return stats_; // Return mutable MMU stats reference.
    }

    // Returns a const reference to this walker's MMU statistics collector.
    const mmu_stats_t &
    get_stats() const
    {
        return stats_; // Return const MMU stats reference.
    }

    // Returns the core ID owning this page table walker.
    int
    get_core_id() const
    {
        return core_id_; // Return core index.
    }

    /**
     * @brief Sets the sector size for this hardware page table walker.
     */
    void
    set_sector_size(size_t sector_size)
    {
        sector_size_ = (sector_size > 0) ? sector_size : 1; // Update configured sector size.
    }

    /**
     * @brief Returns the configured sector size.
     */
    size_t
    get_sector_size() const
    {
        return sector_size_; // Return sector size.
    }

    /**
     * @brief Configures memory access latencies used during radix page table walks.
     */
    void
    set_latencies(uint64_t pwc_lat, uint64_t l1d_lat, uint64_t l2_lat, uint64_t l3_lat,
                  uint64_t dram_lat)
    {
        pwc_hit_latency_ = (pwc_lat > 0) ? pwc_lat : 1;       // PWC hit latency.
        l1d_hit_latency_ = (l1d_lat > 0) ? l1d_lat : 1;       // L1D hit latency for PTE read.
        l2_hit_latency_ = (l2_lat > 0) ? l2_lat : 1;         // L2 hit latency for PTE read.
        l3_hit_latency_ = (l3_lat > 0) ? l3_lat : 1;         // L3 hit latency for PTE read.
        dram_latency_ = (dram_lat > 0) ? dram_lat : 1;       // DRAM access latency for PTE read.
    }

    /**
     * @brief Returns the total latency in cycles incurred by the most recent page table walk.
     */
    uint64_t
    get_last_walk_latency() const
    {
        return last_walk_latency_; // Return last walk duration in cycles.
    }

private:
    // Issues a physical memory read for a PTE (or full sector of PTEs) into `ptw_data_cache_` if attached
    // and returns the hierarchy level where the read was satisfied (L1D, L2, L3, DRAM).
    ptw_cache_hit_level_t
    issue_pte_cache_read(const memref_t &origin_memref, addr_t pte_phys_addr,
                         ptw_level_t ptw_level);

    int core_id_;                                   // Simulated CPU core index.
    mmu_address_space_manager_t *address_space_mgr_; // Shared radix page table manager.
    page_walk_cache_t pwc_;                         // Per-core Page Walk Cache.
    mmu_stats_t stats_;                             // Per-core MMU & PTW statistics.
    caching_device_t *ptw_data_cache_;              // Optional coupled data cache.
    size_t sector_size_ = 1;                        // Configured sector size (1, 2, 4, or 8 PTEs).
    uint64_t pwc_hit_latency_ = 4;                  // Latency for Page Walk Cache hit.
    uint64_t l1d_hit_latency_ = 4;                  // Latency for PTE read hitting in L1D.
    uint64_t l2_hit_latency_ = 14;                  // Latency for PTE read hitting in L2 (L1D + L2).
    uint64_t l3_hit_latency_ = 34;                  // Latency for PTE read hitting in L3 (L1D + L2 + L3).
    uint64_t dram_latency_ = 184;                   // Latency for PTE read hitting in DRAM (caches + DRAM).
    uint64_t last_walk_latency_ = 0;                // Latency in cycles of the last executed walk.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_PAGE_TABLE_WALKER_H_ */
