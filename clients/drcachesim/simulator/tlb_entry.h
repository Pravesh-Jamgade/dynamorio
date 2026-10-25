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

/* tlb_entry.h: Represents a single hardware TLB entry storing process ID (ASID/PID),
 * virtual page tag (VPN), translated physical page number (PPN), page granularity
 * (4KB, 2MB, or 1GB), and access/dirty status bits.
 */

#ifndef _TLB_ENTRY_H_ // Guard against multiple inclusions.
#define _TLB_ENTRY_H_ 1 // Define inclusion guard macro.

#include <array>                  // Provide std::array for sector sub-entries.
#include <cstddef>                // Provide size_t definition.
#include "caching_device_block.h" // Provide base caching_device_block_t class.
#include "mmu/mmu_types.h"        // Provide page_size_kind_t and addr_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Maximum supported sector size (1, 2, 4, or 8 PTEs per sector block).
 */
static constexpr size_t MAX_TLB_SECTOR_SIZE = 8; // Support up to 8 PTEs (64 bytes).

/**
 * @brief Represents an individual page translation sub-entry within a sectored TLB block.
 */
struct tlb_sub_entry_t {
    bool valid = false;                                                // Sub-entry valid bit.
    addr_t ppn = 0;                                                    // Physical page number (PPN).
    page_size_kind_t page_size_kind = page_size_kind_t::PAGE_SIZE_4KB; // Page size granularity.
    bool dirty = false;                                                // Hardware Dirty (D) status bit.
};

/**
 * @brief Represents a hardware TLB entry extending caching_device_block_t.
 * Supports both baseline single-entry operation and sectored (2, 4, 8 PTE) operation.
 */
class tlb_entry_t : public caching_device_block_t {
public:
    /**
     * @brief Constructs an invalid TLB entry with default 4 KB granularity.
     */
    tlb_entry_t()
        : caching_device_block_t()                         // Initialize base tag_ and counter_.
        , pid_(0)                                          // Initialize process ID to 0.
        , ppn_(0)                                          // Initialize physical page number to 0.
        , page_size_kind_(page_size_kind_t::PAGE_SIZE_4KB) // Default to 4 KB page size.
        , dirty_(false)                                    // Initialize dirty bit to false.
        , sector_size_(1)                                  // Default sector size 1 (baseline mode).
    {
        // Mark all sector sub-entries initially invalid.
        for (size_t s = 0; s < MAX_TLB_SECTOR_SIZE; ++s) { // Iterate sub-entries.
            sub_entries_[s].valid = false;                  // Mark sub-entry invalid.
            sub_entries_[s].ppn = 0;                        // Reset PPN.
            sub_entries_[s].dirty = false;                  // Reset dirty bit.
        }
    }

    // Process ID (ASID) to differentiate virtual pages that share the same VPN
    // across different processes.
    memref_pid_t pid_;

    // Translated Physical Page Number (PPN = physical_frame_base >> page_shift)
    // populated by the hardware page table walker on a TLB fill.
    addr_t ppn_;

    // Page size granularity of this translation (4 KB, 2 MB, or 1 GB).
    page_size_kind_t page_size_kind_;

    // Hardware Dirty (D) bit indicating whether a write has hit this TLB entry.
    bool dirty_;

    // Configured sector size for this TLB block (1 = baseline, 2, 4, or 8).
    size_t sector_size_;

    // Array of sector sub-entries (each holding translation for one page in the sector).
    std::array<tlb_sub_entry_t, MAX_TLB_SECTOR_SIZE> sub_entries_;

    /**
     * @brief Checks if a specific sub-entry in this sector is valid.
     */
    bool
    is_sub_entry_valid(size_t sub_idx) const
    {
        // For non-sectored TLB, valid if block tag is not TAG_INVALID.
        if (sector_size_ <= 1) {                           // Check non-sectored mode.
            return tag_ != TAG_INVALID;                    // Return validity based on tag.
        }
        // For sectored TLB, verify tag is valid and specific sub-entry bit is set.
        if (tag_ != TAG_INVALID && sub_idx < sector_size_) { // Check valid tag and index.
            return sub_entries_[sub_idx].valid;            // Return sub-entry valid bit.
        }
        return false;                                      // Out of range or invalid tag.
    }

    /**
     * @brief Returns the PPN for the specified sub-entry.
     */
    addr_t
    get_sub_entry_ppn(size_t sub_idx) const
    {
        // Return sub-entry PPN if sectored and index valid.
        if (sector_size_ > 1 && sub_idx < sector_size_ && sub_entries_[sub_idx].valid) {
            return sub_entries_[sub_idx].ppn;              // Return sub-entry PPN.
        }
        return ppn_;                                       // Fall back to baseline PPN.
    }

    /**
     * @brief Returns the page size kind for the specified sub-entry.
     */
    page_size_kind_t
    get_sub_entry_kind(size_t sub_idx) const
    {
        // Return sub-entry page size kind if sectored and index valid.
        if (sector_size_ > 1 && sub_idx < sector_size_ && sub_entries_[sub_idx].valid) {
            return sub_entries_[sub_idx].page_size_kind;   // Return sub-entry kind.
        }
        return page_size_kind_;                            // Fall back to baseline kind.
    }

    /**
     * @brief Sets the dirty status for a specific sub-entry.
     */
    void
    set_sub_entry_dirty(size_t sub_idx, bool d)
    {
        dirty_ = d;                                        // Update primary dirty flag.
        if (sub_idx < MAX_TLB_SECTOR_SIZE) {               // Check index within bounds.
            sub_entries_[sub_idx].dirty = d;               // Update sub-entry dirty flag.
        }
    }

    /**
     * @brief Installs translation data into a specific sub-entry slot.
     */
    void
    set_sub_entry(size_t sub_idx, addr_t ppn_val, page_size_kind_t kind_val, bool dirty_val)
    {
        if (sub_idx < MAX_TLB_SECTOR_SIZE) {               // Check index within bounds.
            sub_entries_[sub_idx].valid = true;            // Mark sub-entry valid.
            sub_entries_[sub_idx].ppn = ppn_val;           // Set physical page number.
            sub_entries_[sub_idx].page_size_kind = kind_val; // Set page size granularity.
            sub_entries_[sub_idx].dirty = dirty_val;       // Set dirty bit status.
        }
    }
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _TLB_ENTRY_H_ */
