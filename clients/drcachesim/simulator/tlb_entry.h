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

#include "caching_device_block.h" // Provide base caching_device_block_t class.
#include "mmu/mmu_types.h"        // Provide page_size_kind_t and addr_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Represents a hardware TLB entry extending caching_device_block_t.
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
    {
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
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _TLB_ENTRY_H_ */
