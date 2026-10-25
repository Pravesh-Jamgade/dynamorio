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

/* tlb.h: Represents a single hardware TLB supporting PID-tagged VPN->PPN entries,
 * multiple page sizes (4KB, 2MB, 1GB), hardware page table walker integration,
 * PID-aware invalidation/shootdowns, and LFU, LRU, and FIFO replacement policies.
 */

#ifndef _TLB_H_ // Guard against multiple inclusions.
#define _TLB_H_ 1 // Define inclusion guard macro.

#include <string> // Provide std::string for replacement policy names.
#include <vector> // Provide std::vector for child caching devices.

#include "caching_device.h"          // Provide base caching_device_t class.
#include "memref.h"                  // Provide memref_t, addr_t, and memref_pid_t.
#include "mmu/huge_page_detector.h"  // Provide huge_page_detector_t.
#include "mmu/mmu_types.h"           // Provide page_size_kind_t and ptw_result_t.
#include "mmu/page_table_walker.h"   // Provide page_table_walker_t.
#include "tlb_entry.h"               // Provide tlb_entry_t.
#include "tlb_stats.h"               // Provide tlb_stats_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Represents a single hardware TLB device (base class implements LFU policy).
 */
class tlb_t : public caching_device_t {
public:
    /**
     * @brief Constructs a hardware TLB device with an optional descriptive name.
     */
    explicit tlb_t(const std::string &name = "tlb")
        : caching_device_t(name)        // Initialize base caching device.
        , last_pid_(0)                  // Initialize cached last PID to 0.
        , page_table_walker_(nullptr)   // No page table walker attached initially.
        , huge_page_detector_(nullptr)  // No huge page detector attached initially.
    {
    }

    /**
     * @brief Processes a memory reference through this TLB (looking up (tag, pid),
     *        supporting huge pages, and invoking parent TLB or hardware PTW on miss).
     */
    void
    request(const memref_t &memref) override;

    /**
     * @brief Attaches a hardware page table walker to service misses when parent_ == NULL.
     */
    void
    set_page_table_walker(page_table_walker_t *walker)
    {
        page_table_walker_ = walker; // Store pointer to core's page table walker.
    }

    /**
     * @brief Returns the attached hardware page table walker (or nullptr).
     */
    page_table_walker_t *
    get_page_table_walker() const
    {
        return page_table_walker_; // Return page table walker pointer.
    }

    /**
     * @brief Attaches a huge-page detector so 2 MB and 1 GB pages use huge-page tags.
     */
    void
    set_huge_page_detector(const huge_page_detector_t *detector)
    {
        huge_page_detector_ = detector; // Store pointer to huge page detector.
    }

    /**
     * @brief Returns the attached huge-page detector (or nullptr).
     */
    const huge_page_detector_t *
    get_huge_page_detector() const
    {
        return huge_page_detector_; // Return huge page detector pointer.
    }

    /**
     * @brief Configures the sector size for this TLB (1 = baseline non-sectored, 2, 4, 8).
     */
    void
    set_sector_size(size_t sector_size)
    {
        sector_size_ = (sector_size > 0) ? sector_size : 1; // Store sector size.
    }

    /**
     * @brief Returns the configured sector size for this TLB.
     */
    size_t
    get_sector_size() const
    {
        return sector_size_; // Return sector size.
    }

    /**
     * @brief Checks whether this TLB (or any child TLB) holds an entry matching
     *        both `tag` and `pid` (resolving i#4816).
     */
    bool
    contains_tag_pid(addr_t tag, memref_pid_t pid) const;

    /**
     * @brief Looks up the translated Physical Page Number (PPN) and page granularity
     *        stored in this TLB for `(pid, vaddr)` without modifying replacement state.
     *
     * @param pid      Process ID to match.
     * @param vaddr    Virtual address to look up.
     * @param ppn_out  Optional output receiving the stored `ppn_`.
     * @param kind_out Optional output receiving the stored `page_size_kind_`.
     * @return True if a matching valid entry is present in this TLB.
     */
    bool
    lookup_entry(memref_pid_t pid, addr_t vaddr, addr_t *ppn_out = nullptr,
                 page_size_kind_t *kind_out = nullptr) const;

    /**
     * @brief Invalidates a specific `(tag, pid)` entry in this TLB and cascades
     *        the invalidation to any child TLBs (resolving i#4816).
     */
    void
    invalidate_tag_pid(addr_t tag, memref_pid_t pid,
                       invalidation_type_t invalidation_type = INVALIDATION_INCLUSIVE);

    /**
     * @brief Invalidates any TLB entry covering `(pid, vaddr)` (4KB, 2MB, or 1GB)
     *        for TLB shootdowns.
     */
    void
    invalidate_vaddr(memref_pid_t pid, addr_t vaddr);

    /**
     * @brief Flushes all TLB entries belonging to `pid` (e.g., on process exit or CR3 flush).
     */
    void
    invalidate_pid(memref_pid_t pid);

    /**
     * @brief Overrides base `invalidate(tag, type)` to invalidate all entries matching
     *        `tag` across any PID.
     */
    void
    invalidate(addr_t tag, invalidation_type_t invalidation_type) override;

protected:
    /**
     * @brief Allocates `tlb_entry_t` instances for `blocks_`.
     */
    void
    init_blocks() override;

    /**
     * @brief Computes the canonical virtual page tag, page size kind, and effective
     *        page byte size for `(pid, addr)`, accounting for 2 MB and 1 GB huge pages.
     */
    addr_t
    compute_canonical_tag(memref_pid_t pid, addr_t addr,
                          page_size_kind_t *kind_out = nullptr,
                          uint64_t *page_bytes_out = nullptr) const;

    // Optimization: remember last PID in addition to last_tag_.
    memref_pid_t last_pid_;
    // Optional hardware page table walker invoked on last-level TLB misses.
    page_table_walker_t *page_table_walker_;
    // Optional huge-page detector for 2 MB / 1 GB canonical tag alignment.
    const huge_page_detector_t *huge_page_detector_;
    // Configured sector size (1 = baseline, 2, 4, 8 PTEs per sector block).
    size_t sector_size_ = 1;
};

/**
 * @brief Hardware TLB implementing the Least Recently Used (LRU) replacement policy.
 */
class tlb_lru_t : public tlb_t {
public:
    /**
     * @brief Constructs an LRU hardware TLB.
     */
    explicit tlb_lru_t(const std::string &name = "tlb_lru")
        : tlb_t(name) // Initialize base tlb_t.
    {
    }

    /**
     * @brief Initializes the TLB and seeds per-way LRU age counters.
     */
    bool
    init(int associativity, int block_size, int num_blocks, caching_device_t *parent,
         caching_device_stats_t *stats, prefetcher_t *prefetcher = nullptr,
         cache_inclusion_policy_t inclusion_policy =
             cache_inclusion_policy_t::NON_INC_NON_EXC,
         bool coherent_cache = false, int id = -1, snoop_filter_t *snoop_filter = nullptr,
         const std::vector<caching_device_t *> &children = {}) override;

    /**
     * @brief Returns the replacement policy name ("LRU").
     */
    std::string
    get_replace_policy() const override
    {
        return "LRU"; // Return LRU policy identifier.
    }

protected:
    // Updates LRU recency counters on a hit or fill.
    void
    access_update(int block_idx, int way) override;

    // Selects the least-recently-used way for replacement.
    int
    replace_which_way(int block_idx) override;

    // Returns the index of the next way that would be replaced.
    int
    get_next_way_to_replace(const int block_idx) const override;
};

/**
 * @brief Hardware TLB implementing the First-In-First-Out (FIFO) replacement policy.
 */
class tlb_fifo_t : public tlb_t {
public:
    /**
     * @brief Constructs a FIFO hardware TLB.
     */
    explicit tlb_fifo_t(const std::string &name = "tlb_fifo")
        : tlb_t(name) // Initialize base tlb_t.
    {
    }

    /**
     * @brief Initializes the TLB and sets the round-robin victim pointer per set.
     */
    bool
    init(int associativity, int block_size, int num_blocks, caching_device_t *parent,
         caching_device_stats_t *stats, prefetcher_t *prefetcher = nullptr,
         cache_inclusion_policy_t inclusion_policy =
             cache_inclusion_policy_t::NON_INC_NON_EXC,
         bool coherent_cache = false, int id = -1, snoop_filter_t *snoop_filter = nullptr,
         const std::vector<caching_device_t *> &children = {}) override;

    /**
     * @brief Returns the replacement policy name ("FIFO").
     */
    std::string
    get_replace_policy() const override
    {
        return "FIFO"; // Return FIFO policy identifier.
    }

protected:
    // No-op on hit for FIFO replacement.
    void
    access_update(int block_idx, int way) override;

    // Advances the round-robin victim pointer and returns the replaced way.
    int
    replace_which_way(int block_idx) override;

    // Returns the current round-robin victim way without advancing the pointer.
    int
    get_next_way_to_replace(const int block_idx) const override;
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _TLB_H_ */
