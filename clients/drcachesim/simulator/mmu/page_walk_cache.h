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

/* page_walk_cache.h: Models a per-core hardware Page Walk Cache (PWC /
 * Translation Cache) that caches intermediate radix directory physical
 * addresses (PML4E at L4, PDPTE at L3, PDE at L2) to shorten page table walks.
 */

#ifndef _DRMEMTRACE_PAGE_WALK_CACHE_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_PAGE_WALK_CACHE_H_ 1 // Define inclusion guard macro.

#include <stddef.h> // Provide size_t definition.
#include <stdint.h> // Provide fixed-width integer types.
#include <vector>   // Provide std::vector for sets and ways.

#include "mmu/mmu_types.h" // Provide ptw_level_t, addr_t, and memref_pid_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Represents a single entry inside the hardware Page Walk Cache (PWC).
 */
struct pwc_entry_t {
    bool valid = false;                                  // True if this slot holds a valid entry.
    memref_pid_t pid = 0;                                // Process ID owning this translation.
    ptw_level_t level = ptw_level_t::PTW_LEVEL_INVALID;  // Cached radix level (L4, L3, or L2).
    addr_t vaddr_tag = 0;                                // Virtual address prefix shifted to `level`.
    addr_t next_table_paddr = 0;                         // Physical base address of child table.
    uint64_t lru_tick = 0;                               // Monotonic timestamp for LRU eviction.
};

/**
 * @brief Set-associative hardware Page Walk Cache (PWC) with LRU replacement.
 *
 * Real x86-64 / AArch64 MMUs maintain small translation caches (e.g., 24-32 entries)
 * for intermediate page table levels (L4 PML4E, L3 PDPTE, L2 PDE). When an L2 TLB
 * miss triggers a hardware page table walk, the walker probes the PWC for the deepest
 * matching intermediate table so it can skip reading upper page table levels from memory.
 */
class page_walk_cache_t {
public:
    /**
     * @brief Constructs a Page Walk Cache with the given capacity and associativity.
     *
     * @param num_entries Total number of entries across all sets (0 disables the PWC).
     * @param assoc       Set associativity (must divide num_entries evenly when > 0).
     */
    page_walk_cache_t(unsigned int num_entries = 32, unsigned int assoc = 4);

    /**
     * @brief Virtual destructor.
     */
    virtual ~page_walk_cache_t() = default;

    // Returns whether the Page Walk Cache is enabled (num_entries_ > 0).
    bool
    is_enabled() const
    {
        return num_entries_ > 0; // Enabled when capacity is non-zero.
    }

    /**
     * @brief Looks up an intermediate page table translation at `level` for `(pid, vaddr)`.
     *
     * @param pid                  Process ID of the access.
     * @param level                Intermediate radix level to check (L4, L3, or L2).
     * @param vaddr                Virtual address being translated.
     * @param next_table_paddr_out Output receiving the child table physical base on hit.
     * @return True on a PWC hit; false on a miss.
     */
    bool
    lookup(memref_pid_t pid, ptw_level_t level, addr_t vaddr,
           addr_t *next_table_paddr_out);

    /**
     * @brief Inserts or updates an intermediate page table translation in the PWC.
     *
     * @param pid              Process ID of the access.
     * @param level            Intermediate radix level (L4, L3, or L2).
     * @param vaddr            Virtual address being translated.
     * @param next_table_paddr Physical base address of the child table resolved at `level`.
     */
    void
    insert(memref_pid_t pid, ptw_level_t level, addr_t vaddr, addr_t next_table_paddr);

    /**
     * @brief Invalidates all PWC entries belonging to `pid` (e.g., on CR3 switch or flush).
     */
    void
    invalidate_pid(memref_pid_t pid);

    /**
     * @brief Invalidates all PWC entries for `pid` whose virtual prefix covers `vaddr`
     *        (e.g., on munmap / TLB shootdown).
     */
    void
    invalidate_vaddr(memref_pid_t pid, addr_t vaddr);

    /**
     * @brief Flushes the entire Page Walk Cache across all PIDs.
     */
    void
    flush_all();

    /**
     * @brief Computes the virtual address tag prefix for a given radix level.
     *
     * Level 4: vaddr >> 39
     * Level 3: vaddr >> 30
     * Level 2: vaddr >> 21
     */
    static inline addr_t
    compute_vaddr_tag(addr_t vaddr, ptw_level_t level)
    {
        // Convert level enum to integer [1..4].
        int lvl = static_cast<int>(level);
        // Compute bit shift: 12 + (lvl - 1) * 9.
        int shift = mmu_constants_t::PAGE_SHIFT_4KB +
            (lvl - 1) * mmu_constants_t::PT_INDEX_BITS;
        // Shift the virtual address right to form the level-specific tag.
        return (vaddr >> shift);
    }

private:
    // Computes the set index for a given (vaddr_tag, level) pair.
    size_t
    compute_set_index(addr_t vaddr_tag, ptw_level_t level) const;

    unsigned int num_entries_;                  // Total number of entries in the PWC.
    unsigned int assoc_;                        // Number of ways per set.
    unsigned int num_sets_;                     // Number of sets (num_entries_ / assoc_).
    uint64_t access_clock_;                     // Monotonic counter for LRU ordering.
    std::vector<std::vector<pwc_entry_t>> sets_; // 2D array [num_sets_][assoc_] of entries.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_PAGE_WALK_CACHE_H_ */
