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

/* page_table.h: Object-oriented 4-level radix page table (PML4 -> PDPT -> PD -> PT)
 * supporting 4 KB base pages, 2 MB PDE huge pages, and 1 GB PDPTE giant pages,
 * per-process address spaces (CR3 / TTBR0), and on-demand page fault allocation.
 */

#ifndef _DRMEMTRACE_MMU_PAGE_TABLE_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_MMU_PAGE_TABLE_H_ 1 // Define inclusion guard macro.

#include <array>         // Provide std::array for fixed 512-entry hardware tables.
#include <memory>        // Provide std::unique_ptr for automatic node ownership.
#include <stddef.h>      // Provide size_t definition.
#include <stdint.h>      // Provide fixed-width integer types.
#include <unordered_map> // Provide std::unordered_map for per-PID address spaces.

#include "mmu/huge_page_detector.h"       // Provide huge_page_detector_t.
#include "mmu/mmu_types.h"                // Provide page_table_entry_t and ptw_result_t.
#include "mmu/physical_frame_allocator.h" // Provide physical_frame_allocator_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Represents a single 4 KB hardware page table node containing 512 8-byte PTEs.
 *
 * Each node corresponds to one physical 4 KB page at Level 4 (PML4), Level 3 (PDPT),
 * Level 2 (PD), or Level 1 (PT).
 */
class radix_table_node_t {
public:
    /**
     * @brief Constructs a 512-entry radix table node backed by a 4 KB physical frame.
     *
     * @param level          The radix hierarchy level of this table (4 down to 1).
     * @param table_phys_addr The 4 KB-aligned physical base address of this table.
     */
    radix_table_node_t(ptw_level_t level, addr_t table_phys_addr);

    /**
     * @brief Default destructor (unique_ptr children clean up recursively).
     */
    ~radix_table_node_t() = default;

    // Returns the radix level (4..1) of this table node.
    ptw_level_t
    get_level() const
    {
        return level_; // Return table hierarchy level.
    }

    // Returns the 4 KB-aligned physical base address of this table page.
    addr_t
    get_table_phys_addr() const
    {
        return table_phys_addr_; // Return physical base address.
    }

    // Computes the exact physical address of the 8-byte entry at `index` [0..511].
    addr_t
    get_entry_phys_addr(size_t index) const
    {
        // Multiply index by 8 bytes (PTE_SIZE_BYTES) and add to table base address.
        return table_phys_addr_ +
            static_cast<addr_t>(index * mmu_constants_t::PTE_SIZE_BYTES);
    }

    // Returns a mutable reference to the page table entry at `index` [0..511].
    page_table_entry_t &
    get_entry(size_t index)
    {
        return entries_[index]; // Return mutable PTE reference.
    }

    // Returns a const reference to the page table entry at `index` [0..511].
    const page_table_entry_t &
    get_entry(size_t index) const
    {
        return entries_[index]; // Return const PTE reference.
    }

    // Returns a raw pointer to the child radix table node at `index` (or nullptr).
    radix_table_node_t *
    get_child(size_t index) const
    {
        return children_[index].get(); // Return child node pointer.
    }

    // Transfers ownership of a newly allocated child radix table node into `index`.
    void
    set_child(size_t index, std::unique_ptr<radix_table_node_t> child)
    {
        children_[index] = std::move(child); // Store child unique_ptr at index.
    }

private:
    ptw_level_t level_;          // Radix level of this node (L4, L3, L2, or L1).
    addr_t table_phys_addr_;     // Physical base address of this 4 KB table page.
    // Array of 512 architectural page table entries inside this 4 KB page.
    std::array<page_table_entry_t, mmu_constants_t::PT_ENTRIES_PER_TABLE> entries_;
    // Array of 512 pointers to next-level child radix table nodes (unused at L1).
    std::array<std::unique_ptr<radix_table_node_t>,
               mmu_constants_t::PT_ENTRIES_PER_TABLE>
        children_;
};

/**
 * @brief Represents the 4-level radix page table hierarchy for a single process (PID).
 */
class radix_page_table_t {
public:
    /**
     * @brief Constructs a per-process 4-level radix page table and allocates its
     *        root Level 4 (PML4 / CR3) physical table frame.
     *
     * @param pid             Process identifier owning this address space.
     * @param frame_allocator Shared physical frame allocator (must not be nullptr).
     */
    radix_page_table_t(memref_pid_t pid, physical_frame_allocator_t *frame_allocator);

    /**
     * @brief Virtual destructor.
     */
    virtual ~radix_page_table_t() = default;

    // Returns the process ID associated with this radix page table.
    memref_pid_t
    get_pid() const
    {
        return pid_; // Return process ID.
    }

    // Returns the physical base address of the root Level 4 (PML4) table (CR3 value).
    addr_t
    get_cr3_phys_addr() const
    {
        return root_node_->get_table_phys_addr(); // Return root PML4 physical address.
    }

    /**
     * @brief Explicitly maps a virtual page of granularity `kind` (4 KB, 2 MB, or 1 GB)
     *        to the specified physical frame base address.
     *
     * Allocates any missing intermediate radix table nodes (PML4E -> PDPTE -> PDE)
     * via `frame_allocator_`. For 2 MB or 1 GB huge pages, sets the hardware PS bit
     * on the L2 PDE or L3 PDPTE entry respectively.
     *
     * @param vaddr    Virtual address to map (will be aligned to `kind`).
     * @param paddr    Physical frame address to map to (will be aligned to `kind`).
     * @param kind     Page size granularity (4 KB, 2 MB, or 1 GB).
     * @param writable Whether the mapping permits write accesses.
     * @return True if the mapping was installed successfully.
     */
    bool
    map_page(addr_t vaddr, addr_t paddr,
             page_size_kind_t kind = page_size_kind_t::PAGE_SIZE_4KB,
             bool writable = true);

    /**
     * @brief Unmaps the virtual page containing `vaddr` (simulating munmap) and frees
     *        its leaf entry.
     *
     * @param vaddr             Virtual address inside the page to unmap.
     * @param unmapped_kind_out Optional output receiving the page kind that was unmapped.
     * @return True if a present mapping was found and cleared; false if not present.
     */
    bool
    unmap_page(addr_t vaddr, page_size_kind_t *unmapped_kind_out = nullptr);

    /**
     * @brief Performs a full radix tree lookup for `vaddr`, allocating intermediate
     *        tables and demand-faulting the leaf page if not yet mapped.
     *
     * Populates `result_out->steps` with the exact physical addresses of every
     * radix level entry (L4 -> L3 -> L2 [-> L1]) visited during the walk.
     *
     * @param vaddr          Input virtual address to translate.
     * @param is_write       True if the memory access is a write (sets Dirty bit).
     * @param external_v2p   Optional map of known 4 KB virtual->physical mappings.
     * @param huge_detector  Optional huge-page detector to determine page granularity.
     * @param result_out     Output structure receiving the walk steps and translation.
     * @return True on successful resolution.
     */
    bool
    walk_and_populate(addr_t vaddr, bool is_write,
                      const std::unordered_map<addr_t, addr_t> *external_v2p,
                      const huge_page_detector_t *huge_detector,
                      ptw_result_t *result_out);

    /**
     * @brief Queries an existing mapping without allocating on a miss (non-faulting).
     *
     * @param vaddr          Virtual address to query.
     * @param phys_addr_out  Output translated physical address if present.
     * @param kind_out       Optional output receiving the page size kind if present.
     * @return True if `vaddr` is currently mapped as present in the radix tree.
     */
    bool
    lookup_non_faulting(addr_t vaddr, addr_t *phys_addr_out,
                        page_size_kind_t *kind_out = nullptr) const;

    // Returns the count of first-touch demand page faults serviced in this address space.
    uint64_t
    get_page_fault_count() const
    {
        return page_fault_count_; // Return total page faults for this PID.
    }

private:
    // Helper that ensures a child node exists at `index` of `parent_node`.
    radix_table_node_t *
    ensure_child_node(radix_table_node_t *parent_node, size_t index);

    // Helper that determines the physical leaf frame for a demand-faulted page.
    addr_t
    resolve_leaf_physical_frame(addr_t vaddr, page_size_kind_t kind,
                                const std::unordered_map<addr_t, addr_t> *external_v2p);

    memref_pid_t pid_;                              // Process ID owning this table.
    physical_frame_allocator_t *frame_allocator_;   // Non-owning pointer to allocator.
    std::unique_ptr<radix_table_node_t> root_node_; // Level 4 (PML4) root table node.
    uint64_t page_fault_count_;                     // Number of demand page faults.
};

/**
 * @brief Manages per-process radix page tables across all simulated processes.
 */
class mmu_address_space_manager_t {
public:
    /**
     * @brief Constructs the multi-process MMU address space manager.
     *
     * @param pt_phys_base   Starting physical address for 4 KB page-table frames.
     * @param data_phys_base Starting physical address for synthetic leaf data frames.
     */
    explicit mmu_address_space_manager_t(
        addr_t pt_phys_base = mmu_constants_t::DEFAULT_PT_PHYS_BASE,
        addr_t data_phys_base = mmu_constants_t::DEFAULT_DATA_PHYS_BASE);

    /**
     * @brief Virtual destructor.
     */
    virtual ~mmu_address_space_manager_t() = default;

    /**
     * @brief Retrieves or lazily creates the radix_page_table_t for `pid`.
     */
    radix_page_table_t *
    get_or_create_page_table(memref_pid_t pid);

    /**
     * @brief Returns a mutable reference to the shared physical frame allocator.
     */
    physical_frame_allocator_t &
    get_frame_allocator()
    {
        return frame_allocator_; // Return mutable allocator reference.
    }

    /**
     * @brief Returns a const reference to the shared physical frame allocator.
     */
    const physical_frame_allocator_t &
    get_frame_allocator() const
    {
        return frame_allocator_; // Return const allocator reference.
    }

    /**
     * @brief Returns a mutable reference to the shared huge page detector.
     */
    huge_page_detector_t &
    get_huge_page_detector()
    {
        return huge_page_detector_; // Return mutable huge page detector reference.
    }

    /**
     * @brief Returns a const reference to the shared huge page detector.
     */
    const huge_page_detector_t &
    get_huge_page_detector() const
    {
        return huge_page_detector_; // Return const huge page detector reference.
    }

    /**
     * @brief Registers an external virtual-to-physical mapping (e.g., from v2p_file or
     *        trace markers), reserves its physical frame, and feeds it to the huge-page
     *        detector.
     *
     * @param pid   Process ID (or 0 for global trace mapping).
     * @param vaddr Virtual page address.
     * @param paddr Physical frame address.
     */
    void
    register_external_v2p_mapping(memref_pid_t pid, addr_t vaddr, addr_t paddr);

    /**
     * @brief Bulk-loads an external v2p map (such as from simulator_t::virt2phys_).
     */
    void
    load_external_v2p_map(memref_pid_t pid,
                          const std::unordered_map<addr_t, addr_t> &v2p_map);

    /**
     * @brief Returns a pointer to the external v2p map for `pid` (falling back to PID 0).
     */
    const std::unordered_map<addr_t, addr_t> *
    get_external_v2p_map(memref_pid_t pid) const;

private:
    // Shared physical frame allocator for all page table nodes and synthetic frames.
    physical_frame_allocator_t frame_allocator_;
    // Shared huge page detector across processes.
    huge_page_detector_t huge_page_detector_;
    // Map from PID to its 4-level radix page table instance.
    std::unordered_map<memref_pid_t, std::unique_ptr<radix_page_table_t>> page_tables_;
    // Map from PID (or 0 for global) to external 4 KB v2p mappings.
    std::unordered_map<memref_pid_t, std::unordered_map<addr_t, addr_t>> external_v2p_;
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_MMU_PAGE_TABLE_H_ */
