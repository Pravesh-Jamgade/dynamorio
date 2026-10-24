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

/* mmu_types.h: Fundamental types, constants, and structures for realistic
 * MMU, multi-level radix page table, huge-page, and hardware page table walk
 * simulation.
 */

#ifndef _DRMEMTRACE_MMU_TYPES_H_ // Guard against multiple inclusions of this header.
#define _DRMEMTRACE_MMU_TYPES_H_ 1 // Define inclusion guard macro.

#include <stddef.h> // Provide size_t definition.
#include <stdint.h> // Provide fixed-width integer types such as uint64_t.
#include <string>   // Provide std::string for human-readable formatting.
#include <vector>   // Provide std::vector for storing page table walk steps.

#include "memref.h"      // Provide addr_t and memref_pid_t definitions.
#include "trace_entry.h" // Provide trace entry types and constants.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace analysis and simulation namespace.

/**
 * @brief Architectural constants for 4-level radix page tables (x86-64 / AArch64).
 */
struct mmu_constants_t {
    // Size of a standard base page in bytes (4 KB = 4096 bytes).
    static constexpr uint64_t PAGE_SIZE_4KB_BYTES = 4ULL * 1024ULL;
    // Bit shift for a 4 KB base page (2^12 = 4096).
    static constexpr int PAGE_SHIFT_4KB = 12;
    // Size of a L2 huge page (PMD / PDE huge page) in bytes (2 MB = 2,097,152 bytes).
    static constexpr uint64_t PAGE_SIZE_2MB_BYTES = 2ULL * 1024ULL * 1024ULL;
    // Bit shift for a 2 MB huge page (2^21 = 2,097,152).
    static constexpr int PAGE_SHIFT_2MB = 21;
    // Size of a L3 giant huge page (PUD / PDPTE huge page) in bytes (1 GB).
    static constexpr uint64_t PAGE_SIZE_1GB_BYTES = 1024ULL * 1024ULL * 1024ULL;
    // Bit shift for a 1 GB huge page (2^30 = 1,073,741,824).
    static constexpr int PAGE_SHIFT_1GB = 30;
    // Number of 4 KB base pages inside one 2 MB huge page (512 pages).
    static constexpr size_t PAGES_PER_2MB_HUGE_PAGE = 512;
    // Number of 4 KB base pages inside one 1 GB huge page (262,144 pages).
    static constexpr size_t PAGES_PER_1GB_HUGE_PAGE = 512 * 512;
    // Number of entries in each radix page table node (512 entries per 4 KB page).
    static constexpr size_t PT_ENTRIES_PER_TABLE = 512;
    // Number of index bits resolved at each radix page table level (9 bits).
    static constexpr int PT_INDEX_BITS = 9;
    // Bitmask for extracting a 9-bit page table index (0x1ff).
    static constexpr uint64_t PT_INDEX_MASK = 0x1ffULL;
    // Size in bytes of a single hardware page table entry (8 bytes / 64 bits).
    static constexpr size_t PTE_SIZE_BYTES = 8;
    // Total number of levels in a standard 48-bit 4-level radix page table.
    static constexpr int NUM_PT_LEVELS = 4;
    // Default starting physical address for synthetically allocated page table pages.
    static constexpr addr_t DEFAULT_PT_PHYS_BASE = 0x0000001000000000ULL;
    // Default starting physical address for synthetically allocated data/code frames.
    static constexpr addr_t DEFAULT_DATA_PHYS_BASE = 0x0000002000000000ULL;
};

/**
 * @brief Enumerates the supported hardware page size granularities.
 */
enum class page_size_kind_t {
    PAGE_SIZE_4KB = 0, // Standard 4 KB base page mapped at Level 1 (PTE).
    PAGE_SIZE_2MB = 1, // 2 MB huge page mapped at Level 2 (PDE / PMD with PS=1).
    PAGE_SIZE_1GB = 2, // 1 GB giant page mapped at Level 3 (PDPTE / PUD with PS=1).
};

/**
 * @brief Enumerates the levels of a 4-level radix page table hierarchy.
 *
 * Level 4 is the root table (PML4 on x86-64, PGD on Linux) pointed to by CR3/TTBR.
 * Level 1 is the leaf Page Table (PT) for 4 KB pages.
 */
enum class ptw_level_t : int {
    PTW_LEVEL_INVALID = 0, // Sentinel indicating no level / invalid level.
    PTW_LEVEL_1_PTE = 1,   // Level 1: Page Table Entry (maps a 4 KB page).
    PTW_LEVEL_2_PDE = 2,   // Level 2: Page Directory Entry (maps PT or 2 MB huge page).
    PTW_LEVEL_3_PDPTE = 3, // Level 3: Page Dir Ptr Entry (maps PD or 1 GB huge page).
    PTW_LEVEL_4_PML4E = 4, // Level 4: Page Map Level 4 Entry (maps PDPT).
};

/**
 * @brief Enumerates the cache/memory hierarchy level where a PTE read was satisfied.
 */
enum class ptw_cache_hit_level_t : int {
    HIT_L1D = 0,  // Satisfied by Level 1 Data Cache.
    HIT_L2 = 1,   // Satisfied by Level 2 Cache.
    HIT_L3 = 2,   // Satisfied by Level 3 / LLC Cache.
    HIT_DRAM = 3, // Satisfied by Main Memory (DRAM).
};

/**
 * @brief Helper utility providing string representations for ptw_cache_hit_level_t.
 */
class ptw_cache_hit_level_helper_t {
public:
    /**
     * @brief Returns a short human-readable string for the cache hit level.
     */
    static inline const char *
    to_string(ptw_cache_hit_level_t hit_level)
    {
        // Dispatch based on the cache hit level.
        switch (hit_level) {
        case ptw_cache_hit_level_t::HIT_L1D: // L1 Data Cache.
            return "L1D";
        case ptw_cache_hit_level_t::HIT_L2:  // L2 Unified Cache.
            return "L2";
        case ptw_cache_hit_level_t::HIT_L3:  // L3 Shared Cache.
            return "L3";
        case ptw_cache_hit_level_t::HIT_DRAM: // Main Memory.
            return "DRAM";
        }
        // Fallback for unexpected values.
        return "UNKNOWN";
    }
};

/**
 * @brief Utility class providing conversion methods for page sizes and radix levels.
 */
class page_size_helper_t {
public:
    /**
     * @brief Returns the size in bytes for a given page_size_kind_t.
     */
    static inline uint64_t
    get_page_size_bytes(page_size_kind_t kind)
    {
        // Dispatch based on the page size enumeration value.
        switch (kind) {
        case page_size_kind_t::PAGE_SIZE_4KB: // Handle 4 KB page size.
            return mmu_constants_t::PAGE_SIZE_4KB_BYTES; // Return 4096 bytes.
        case page_size_kind_t::PAGE_SIZE_2MB: // Handle 2 MB huge page size.
            return mmu_constants_t::PAGE_SIZE_2MB_BYTES; // Return 2,097,152 bytes.
        case page_size_kind_t::PAGE_SIZE_1GB: // Handle 1 GB giant page size.
            return mmu_constants_t::PAGE_SIZE_1GB_BYTES; // Return 1,073,741,824 bytes.
        }
        // Fallback to 4 KB if an unknown enum value is passed.
        return mmu_constants_t::PAGE_SIZE_4KB_BYTES;
    }

    /**
     * @brief Returns the bit shift (log2 of byte size) for a given page_size_kind_t.
     */
    static inline int
    get_page_shift(page_size_kind_t kind)
    {
        // Dispatch based on the page size enumeration value.
        switch (kind) {
        case page_size_kind_t::PAGE_SIZE_4KB: // Handle 4 KB page shift.
            return mmu_constants_t::PAGE_SHIFT_4KB; // Return 12 bits.
        case page_size_kind_t::PAGE_SIZE_2MB: // Handle 2 MB page shift.
            return mmu_constants_t::PAGE_SHIFT_2MB; // Return 21 bits.
        case page_size_kind_t::PAGE_SIZE_1GB: // Handle 1 GB page shift.
            return mmu_constants_t::PAGE_SHIFT_1GB; // Return 30 bits.
        }
        // Fallback to 12 bits for 4 KB.
        return mmu_constants_t::PAGE_SHIFT_4KB;
    }

    /**
     * @brief Converts a byte size to its corresponding page_size_kind_t.
     */
    static inline page_size_kind_t
    kind_from_bytes(uint64_t size_bytes)
    {
        // Check if the size matches a 1 GB giant page.
        if (size_bytes >= mmu_constants_t::PAGE_SIZE_1GB_BYTES) {
            return page_size_kind_t::PAGE_SIZE_1GB; // Return 1 GB kind.
        }
        // Check if the size matches a 2 MB huge page.
        if (size_bytes >= mmu_constants_t::PAGE_SIZE_2MB_BYTES) {
            return page_size_kind_t::PAGE_SIZE_2MB; // Return 2 MB kind.
        }
        // Otherwise treat as a standard 4 KB page.
        return page_size_kind_t::PAGE_SIZE_4KB;
    }

    /**
     * @brief Returns the terminal radix tree level for a given page size kind.
     *
     * A 4 KB page terminates at Level 1 (PTE), a 2 MB huge page terminates at
     * Level 2 (PDE), and a 1 GB huge page terminates at Level 3 (PDPTE).
     */
    static inline ptw_level_t
    get_terminal_level(page_size_kind_t kind)
    {
        // Dispatch based on page size kind.
        switch (kind) {
        case page_size_kind_t::PAGE_SIZE_4KB: // 4 KB page terminates at L1 PTE.
            return ptw_level_t::PTW_LEVEL_1_PTE;
        case page_size_kind_t::PAGE_SIZE_2MB: // 2 MB huge page terminates at L2 PDE.
            return ptw_level_t::PTW_LEVEL_2_PDE;
        case page_size_kind_t::PAGE_SIZE_1GB: // 1 GB huge page terminates at L3 PDPTE.
            return ptw_level_t::PTW_LEVEL_3_PDPTE;
        }
        // Default to Level 1 PTE.
        return ptw_level_t::PTW_LEVEL_1_PTE;
    }

    /**
     * @brief Aligns an address down to the start boundary of the specified page kind.
     */
    static inline addr_t
    align_to_page(addr_t addr, page_size_kind_t kind)
    {
        // Retrieve the byte size of the page kind.
        uint64_t page_bytes = get_page_size_bytes(kind);
        // Mask off the low page offset bits and return the aligned page base address.
        return static_cast<addr_t>(addr & ~(page_bytes - 1ULL));
    }

    /**
     * @brief Extracts the page offset within a page of the specified kind.
     */
    static inline addr_t
    extract_page_offset(addr_t addr, page_size_kind_t kind)
    {
        // Retrieve the byte size of the page kind.
        uint64_t page_bytes = get_page_size_bytes(kind);
        // Mask the low bits to obtain the offset within the page.
        return static_cast<addr_t>(addr & (page_bytes - 1ULL));
    }

    /**
     * @brief Extracts the 9-bit radix table index for a given virtual address and level.
     *
     * Level 1: bits [20:12]
     * Level 2: bits [29:21]
     * Level 3: bits [38:30]
     * Level 4: bits [47:39]
     */
    static inline size_t
    extract_level_index(addr_t vaddr, ptw_level_t level)
    {
        // Convert the level enum to an integer in [1, 4].
        int lvl = static_cast<int>(level);
        // Compute the bit shift: 12 + (lvl - 1) * 9.
        int shift = mmu_constants_t::PAGE_SHIFT_4KB +
            (lvl - 1) * mmu_constants_t::PT_INDEX_BITS;
        // Shift the virtual address right and apply the 9-bit mask.
        return static_cast<size_t>((vaddr >> shift) & mmu_constants_t::PT_INDEX_MASK);
    }

    /**
     * @brief Returns a human-readable string label for the page size kind.
     */
    static inline std::string
    to_string(page_size_kind_t kind)
    {
        // Dispatch based on page size kind.
        switch (kind) {
        case page_size_kind_t::PAGE_SIZE_4KB: // 4 KB label.
            return "4KB";
        case page_size_kind_t::PAGE_SIZE_2MB: // 2 MB label.
            return "2MB";
        case page_size_kind_t::PAGE_SIZE_1GB: // 1 GB label.
            return "1GB";
        }
        // Fallback label.
        return "4KB";
    }
};

/**
 * @brief Represents a single architectural entry inside a radix page table node.
 *
 * Models both intermediate directory entries (pointing to the physical base address
 * of the next-level page table) and leaf entries (pointing to a 4 KB, 2 MB, or 1 GB
 * physical data/instruction frame).
 */
class page_table_entry_t {
public:
    /**
     * @brief Constructs an invalid (non-present) page table entry.
     */
    page_table_entry_t()
        : present_(false)         // Entry is initially not present in memory.
        , writable_(false)        // Entry is initially read-only.
        , user_accessible_(true)  // User-mode accessible by default for app traces.
        , accessed_(false)        // Hardware Accessed (A) bit initially false.
        , dirty_(false)           // Hardware Dirty (D) bit initially false.
        , page_size_bit_(false)   // Hardware Page Size (PS) huge-page bit initially false.
        , no_execute_(false)      // Hardware No-Execute (NX) bit initially false.
        , phys_addr_(0)           // Physical base address of next table or leaf frame.
    {
    }

    // Returns whether this entry is valid/present (P bit = 1).
    bool
    is_present() const
    {
        return present_; // Return present status.
    }

    // Sets the present (P) bit of this entry.
    void
    set_present(bool present)
    {
        present_ = present; // Update present status.
    }

    // Returns whether this entry permits write accesses (R/W bit = 1).
    bool
    is_writable() const
    {
        return writable_; // Return writable status.
    }

    // Sets the writable (R/W) bit of this entry.
    void
    set_writable(bool writable)
    {
        writable_ = writable; // Update writable status.
    }

    // Returns whether this entry is accessible from user mode (U/S bit = 1).
    bool
    is_user_accessible() const
    {
        return user_accessible_; // Return user accessibility flag.
    }

    // Sets the user-accessible (U/S) bit of this entry.
    void
    set_user_accessible(bool user_accessible)
    {
        user_accessible_ = user_accessible; // Update user accessibility flag.
    }

    // Returns whether the hardware Accessed (A) bit has been set.
    bool
    is_accessed() const
    {
        return accessed_; // Return accessed status.
    }

    // Sets the hardware Accessed (A) bit.
    void
    set_accessed(bool accessed)
    {
        accessed_ = accessed; // Update accessed status.
    }

    // Returns whether the hardware Dirty (D) bit has been set by a write.
    bool
    is_dirty() const
    {
        return dirty_; // Return dirty status.
    }

    // Sets the hardware Dirty (D) bit.
    void
    set_dirty(bool dirty)
    {
        dirty_ = dirty; // Update dirty status.
    }

    // Returns whether the Page Size (PS) bit is set (indicating a 2 MB or 1 GB leaf).
    bool
    is_huge_page() const
    {
        return page_size_bit_; // Return huge-page PS flag.
    }

    // Sets the Page Size (PS) bit for 2 MB PDE or 1 GB PDPTE huge pages.
    void
    set_huge_page(bool is_huge)
    {
        page_size_bit_ = is_huge; // Update huge-page PS flag.
    }

    // Returns whether instruction fetch is disallowed (NX / XD bit = 1).
    bool
    is_no_execute() const
    {
        return no_execute_; // Return no-execute flag.
    }

    // Sets the No-Execute (NX) bit.
    void
    set_no_execute(bool no_execute)
    {
        no_execute_ = no_execute; // Update no-execute flag.
    }

    // Returns the physical base address of the next-level page table or leaf frame.
    addr_t
    get_phys_addr() const
    {
        return phys_addr_; // Return physical base address.
    }

    // Sets the physical base address of the next-level page table or leaf frame.
    void
    set_phys_addr(addr_t phys_addr)
    {
        phys_addr_ = phys_addr; // Store physical base address.
    }

    // Clears and invalidates this page table entry (e.g., on munmap).
    void
    clear()
    {
        present_ = false;         // Mark entry not present.
        writable_ = false;        // Clear writable bit.
        user_accessible_ = true;  // Reset user bit.
        accessed_ = false;        // Clear accessed bit.
        dirty_ = false;           // Clear dirty bit.
        page_size_bit_ = false;   // Clear huge-page bit.
        no_execute_ = false;      // Clear NX bit.
        phys_addr_ = 0;           // Reset physical address to 0.
    }

private:
    bool present_;         // Hardware Present (P) bit.
    bool writable_;        // Hardware Read/Write (R/W) bit.
    bool user_accessible_; // Hardware User/Supervisor (U/S) bit.
    bool accessed_;        // Hardware Accessed (A) bit set by PTW on walk.
    bool dirty_;           // Hardware Dirty (D) bit set by PTW on write.
    bool page_size_bit_;   // Hardware Page Size (PS) bit (1 = huge leaf at L2/L3).
    bool no_execute_;      // Hardware No-Execute (NX) bit.
    addr_t phys_addr_;     // Physical base address of child table or leaf page frame.
};

/**
 * @brief Captures details of a single level lookup during a hardware page table walk.
 */
struct ptw_step_record_t {
    ptw_level_t level = ptw_level_t::PTW_LEVEL_INVALID; // Radix level (4 down to 1).
    addr_t table_base_paddr = 0; // Physical base address of this level's table page.
    size_t entry_index = 0;      // 9-bit index [0..511] within the table page.
    addr_t entry_paddr = 0;      // Exact 8-byte physical address of the PTE read.
    addr_t next_paddr = 0;       // Physical address stored inside the resolved entry.
    bool pwc_hit = false;        // True if this level was satisfied by Page Walk Cache.
    bool is_leaf = false;        // True if this level terminated the walk (PTE or PS=1).
    ptw_cache_hit_level_t cache_hit_level =
        ptw_cache_hit_level_t::HIT_DRAM; // Cache hierarchy level where PTE read hit.
};

/**
 * @brief Captures the complete outcome of a hardware page table walk for a memref.
 */
struct ptw_result_t {
    bool success = false;        // True if translation succeeded.
    memref_pid_t pid = 0;        // Process ID whose page table was walked.
    addr_t virt_addr = 0;        // Input virtual address being translated.
    addr_t phys_addr = 0;        // Output translated physical address (frame + offset).
    addr_t page_base_vaddr = 0;  // Page-aligned virtual start address.
    addr_t page_base_paddr = 0;  // Page-aligned physical frame start address.
    uint64_t page_size_bytes = mmu_constants_t::PAGE_SIZE_4KB_BYTES; // Resolved size.
    page_size_kind_t page_size_kind = page_size_kind_t::PAGE_SIZE_4KB; // Resolved kind.
    bool is_page_fault = false;  // True if walk triggered a first-touch page allocation.
    ptw_level_t pwc_hit_level = ptw_level_t::PTW_LEVEL_INVALID; // Highest level hit in PWC.
    int num_memory_reads = 0;    // Number of 8-byte PTE memory reads issued to cache/mem.
    std::vector<ptw_step_record_t> steps; // Ordered sequence of radix walk steps.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_MMU_TYPES_H_ */
