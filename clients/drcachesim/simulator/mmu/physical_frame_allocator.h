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

/* physical_frame_allocator.h: Manages allocation, reservation, and recycling of
 * physical memory frames for radix page table nodes and leaf data/code pages
 * (4 KB, 2 MB, and 1 GB granularities).
 */

#ifndef _DRMEMTRACE_PHYSICAL_FRAME_ALLOCATOR_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_PHYSICAL_FRAME_ALLOCATOR_H_ 1 // Define inclusion guard macro.

#include <stddef.h>      // Provide size_t definition.
#include <stdint.h>      // Provide fixed-width integer types.
#include <unordered_set> // Provide std::unordered_set for tracking reserved frames.
#include <vector>        // Provide std::vector for free-lists.

#include "mmu/mmu_types.h" // Provide page_size_kind_t and mmu_constants_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Object-oriented physical frame allocator for the MMU simulator.
 *
 * Because user-space tracing (/proc/self/pagemap or v2p.textproto) only exposes leaf
 * physical frame numbers (or none at all when running without -use_physical), the MMU
 * simulator needs to assign realistic, non-overlapping physical addresses to:
 *   1. Intermediate radix page table nodes (PML4, PDPT, PD, PT) — always 4 KB aligned.
 *   2. Demand-paged leaf data/code frames (4 KB, 2 MB, or 1 GB aligned) when no OS
 *      physical mapping is present.
 */
class physical_frame_allocator_t {
public:
    /**
     * @brief Constructs a physical frame allocator with configurable base regions.
     *
     * @param pt_phys_base   Starting physical address for 4 KB page-table frames.
     * @param data_phys_base Starting physical address for synthetic leaf data frames.
     */
    explicit physical_frame_allocator_t(
        addr_t pt_phys_base = mmu_constants_t::DEFAULT_PT_PHYS_BASE,
        addr_t data_phys_base = mmu_constants_t::DEFAULT_DATA_PHYS_BASE);

    /**
     * @brief Virtual destructor for safe subclassing.
     */
    virtual ~physical_frame_allocator_t() = default;

    /**
     * @brief Allocates a 4 KB physical frame for a radix page table node.
     *
     * @return The 4 KB-aligned physical base address of the newly allocated table frame.
     */
    addr_t
    allocate_page_table_frame();

    /**
     * @brief Frees a previously allocated 4 KB page table frame back to the free-list.
     *
     * @param frame_paddr The 4 KB-aligned physical base address to recycle.
     */
    void
    free_page_table_frame(addr_t frame_paddr);

    /**
     * @brief Allocates a naturally aligned physical leaf frame of the given page size.
     *
     * @param kind The desired page granularity (4 KB, 2 MB, or 1 GB).
     * @return The aligned physical base address of the allocated frame.
     */
    addr_t
    allocate_data_frame(page_size_kind_t kind = page_size_kind_t::PAGE_SIZE_4KB);

    /**
     * @brief Frees a previously allocated leaf data frame back to its size free-list.
     *
     * @param frame_paddr The aligned physical base address of the leaf frame.
     * @param kind        The granularity of the frame being freed.
     */
    void
    free_data_frame(addr_t frame_paddr,
                    page_size_kind_t kind = page_size_kind_t::PAGE_SIZE_4KB);

    /**
     * @brief Marks an externally provided physical frame as reserved to prevent
     *        synthetic allocations from colliding with real OS physical frames.
     *
     * @param phys_addr The physical start address observed from pagemap or v2p_file.
     * @param kind      The page granularity of the external frame.
     */
    void
    reserve_external_frame(addr_t phys_addr,
                           page_size_kind_t kind = page_size_kind_t::PAGE_SIZE_4KB);

    /**
     * @brief Returns the total number of 4 KB page-table frames currently allocated.
     */
    uint64_t
    get_allocated_pt_frames() const
    {
        return allocated_pt_frames_; // Return active page-table frame count.
    }

    /**
     * @brief Returns the total number of synthetic leaf data frames currently allocated.
     */
    uint64_t
    get_allocated_data_frames() const
    {
        return allocated_data_frames_; // Return active leaf data frame count.
    }

private:
    addr_t next_pt_phys_addr_;                   // Bump pointer for 4 KB page table frames.
    addr_t next_data_phys_addr_;                 // Bump pointer for leaf data frames.
    std::vector<addr_t> free_pt_frames_;         // Free-list of recycled 4 KB PT frames.
    std::vector<addr_t> free_data_frames_4kb_;   // Free-list of recycled 4 KB data frames.
    std::vector<addr_t> free_data_frames_2mb_;   // Free-list of recycled 2 MB data frames.
    std::vector<addr_t> free_data_frames_1gb_;   // Free-list of recycled 1 GB data frames.
    std::unordered_set<addr_t> reserved_4kb_frames_; // Reserved 4 KB frame base addresses.
    uint64_t allocated_pt_frames_;               // Count of live page-table frames.
    uint64_t allocated_data_frames_;             // Count of live synthetic data frames.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_PHYSICAL_FRAME_ALLOCATOR_H_ */
