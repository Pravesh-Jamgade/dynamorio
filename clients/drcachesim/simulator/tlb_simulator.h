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

/* tlb_simulator.h: Controls multi-level TLB, 4-level radix page table,
 * hardware page table walker (PTW), Page Walk Cache (PWC), huge-page
 * (4KB/2MB/1GB), and coupled PTW data-cache simulation.
 */

#ifndef _TLB_SIMULATOR_H_ // Guard against multiple inclusions.
#define _TLB_SIMULATOR_H_ 1 // Define inclusion guard macro.

#include <memory>        // Provide std::unique_ptr for RAII ownership.
#include <string>        // Provide std::string.
#include <unordered_map> // Provide std::unordered_map.
#include <vector>        // Provide std::vector.

#include "cache.h"                   // Provide cache_t for coupled PTW data caches.
#include "memref.h"                  // Provide memref_t, addr_t, and memref_pid_t.
#include "mmu/mmu_stats.h"           // Provide mmu_stats_t.
#include "mmu/mmu_types.h"           // Provide page_size_kind_t and ptw_result_t.
#include "mmu/page_table.h"          // Provide mmu_address_space_manager_t.
#include "mmu/page_table_walker.h"   // Provide page_table_walker_t.
#include "simulator.h"               // Provide base simulator_t class.
#include "tlb.h"                     // Provide tlb_t, tlb_lru_t, and tlb_fifo_t.
#include "tlb_simulator_create.h"    // Provide tlb_simulator_knobs_t.
#include "tlb_stats.h"               // Provide tlb_stats_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Multi-core TLB and MMU simulator supporting L1I/L1D/L2 TLBs, 4-level
 *        radix page tables, hardware Page Table Walkers (PTW), Page Walk Caches
 *        (PWC), 2MB/1GB huge pages, TLB shootdowns, and coupled PTW data caches.
 */
class tlb_simulator_t : public simulator_t {
public:
    /**
     * @brief Constructs and initializes the multi-core TLB and MMU simulator.
     */
    tlb_simulator_t(const tlb_simulator_knobs_t &knobs);

    /**
     * @brief Destroys the simulator and frees all per-core TLBs, walkers, and caches.
     */
    virtual ~tlb_simulator_t();

    /**
     * @brief Processes a single trace record through the TLB and MMU hierarchy.
     */
    bool
    process_memref(const memref_t &memref) override;

    /**
     * @brief Prints per-core L1I, L1D, LL TLB stats and (when enabled) MMU/PTW stats.
     */
    bool
    print_results() override;

    /**
     * @brief Parses a v2p.textproto stream and populates both simulator_t::virt2phys_
     *        and the MMU address space manager's v2p & huge-page tables.
     */
    std::string
    create_v2p_from_file(std::istream &v2p_file) override;

    /**
     * @brief Performs a system-wide TLB shootdown for `(pid, vaddr)`: unmaps the page
     *        in `pid`'s radix page table and invalidates matching entries across all
     *        cores' L1I, L1D, L2 TLBs and Page Walk Caches.
     */
    void
    perform_tlb_shootdown(memref_pid_t pid, addr_t vaddr);

    /**
     * @brief Attaches an external CPU data cache (e.g., from cache_simulator_t) to
     *        receive 8-byte physical PTE reads during page table walks on `core`.
     */
    void
    set_core_ptw_data_cache(unsigned int core, caching_device_t *dcache);

    /**
     * @brief Returns the coupled PTW data cache for `core` (or nullptr if none).
     */
    caching_device_t *
    get_core_ptw_data_cache(unsigned int core) const;

    /**
     * @brief Returns the shared multi-process MMU address space manager.
     */
    mmu_address_space_manager_t *
    get_address_space_manager()
    {
        return address_space_mgr_.get(); // Return address space manager pointer.
    }

    /**
     * @brief Returns the hardware page table walker for `core` (or nullptr).
     */
    page_table_walker_t *
    get_page_table_walker(unsigned int core = 0) const;

    /**
     * @brief Returns the MMU / PTW statistics collector for `core` (or nullptr).
     */
    const mmu_stats_t *
    get_mmu_stats(unsigned int core = 0) const;

    /**
     * @brief Returns the `tlb_stats_t` for a specific core and TLB level
     *        (level 1: `is_instr ? itlbs_ : dtlbs_`, level 2: `lltlbs_`).
     */
    const tlb_stats_t *
    get_tlb_stats(unsigned int core, unsigned int level, bool is_instr = false) const;

    /**
     * @brief Queries the current TLB hierarchy on `core` for the translated physical
     *        page number (`ppn_out`) and granularity (`kind_out`) of `(pid, vaddr)`.
     */
    bool
    lookup_tlb_entry(unsigned int core, memref_pid_t pid, addr_t vaddr, bool is_instr,
                     addr_t *ppn_out = nullptr,
                     page_size_kind_t *kind_out = nullptr) const;

protected:
    // Factory method that creates a tlb_t subclass for the given replacement policy.
    virtual tlb_t *
    create_tlb(std::string policy);

    // Checks whether all L2 TLBs have reached `knobs_.warmup_fraction`.
    bool
    check_warmed_up();

    // Resets all TLB, MMU, and coupled PTW cache stats across all cores after warmup.
    void
    reset_all_stats();

    // Configuration knobs for this TLB simulator instance.
    tlb_simulator_knobs_t knobs_;

    // Each CPU core contains a private L1 ITLB, L1 DTLB, and L2 unified TLB.
    tlb_t **itlbs_;
    tlb_t **dtlbs_;
    tlb_t **lltlbs_;

    // Shared multi-process 4-level radix page table, frame allocator, and huge-page detector.
    std::unique_ptr<mmu_address_space_manager_t> address_space_mgr_;
    // Per-core hardware Page Table Walkers (including per-core Page Walk Caches).
    std::vector<std::unique_ptr<page_table_walker_t>> walkers_;
    // Optional internal per-core L1D data caches for coupled PTW memory reads.
    std::vector<std::unique_ptr<cache_t>> internal_ptw_dcaches_;
    // Tracks whether warmup has completed when warmup_refs or warmup_fraction is used.
    bool is_warmed_up_;
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _TLB_SIMULATOR_H_ */
