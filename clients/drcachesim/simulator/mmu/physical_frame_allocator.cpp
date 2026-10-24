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

/* physical_frame_allocator.cpp: Implementation of physical_frame_allocator_t.
 */

#include "mmu/physical_frame_allocator.h" // Include class declaration.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

physical_frame_allocator_t::physical_frame_allocator_t(addr_t pt_phys_base,
                                                       addr_t data_phys_base)
    // Align the initial page-table bump pointer to a 4 KB boundary.
    : next_pt_phys_addr_(
          page_size_helper_t::align_to_page(pt_phys_base, page_size_kind_t::PAGE_SIZE_4KB))
    // Align the initial leaf data frame bump pointer to a 4 KB boundary.
    , next_data_phys_addr_(page_size_helper_t::align_to_page(
          data_phys_base, page_size_kind_t::PAGE_SIZE_4KB))
    // Initialize the live page-table frame counter to zero.
    , allocated_pt_frames_(0)
    // Initialize the live leaf data frame counter to zero.
    , allocated_data_frames_(0)
{
}

addr_t
physical_frame_allocator_t::allocate_page_table_frame()
{
    // Check whether a recycled 4 KB page-table frame is available on the free-list.
    if (!free_pt_frames_.empty()) {
        // Retrieve the most recently freed 4 KB page-table frame address.
        addr_t reused_frame = free_pt_frames_.back();
        // Pop the reused frame from the free-list vector.
        free_pt_frames_.pop_back();
        // Increment the active page-table frame counter.
        ++allocated_pt_frames_;
        // Return the recycled 4 KB frame physical address.
        return reused_frame;
    }
    // Loop until we find a 4 KB frame not already reserved by external OS mappings.
    while (true) {
        // Read the current candidate physical frame address from the bump pointer.
        addr_t candidate = next_pt_phys_addr_;
        // Advance the bump pointer by 4 KB for the next allocation.
        next_pt_phys_addr_ +=
            static_cast<addr_t>(mmu_constants_t::PAGE_SIZE_4KB_BYTES);
        // Check if this candidate frame was externally reserved.
        if (reserved_4kb_frames_.find(candidate) == reserved_4kb_frames_.end()) {
            // Increment the active page-table frame counter.
            ++allocated_pt_frames_;
            // Return the newly allocated 4 KB physical frame address.
            return candidate;
        }
    }
}

void
physical_frame_allocator_t::free_page_table_frame(addr_t frame_paddr)
{
    // Align the address to a 4 KB boundary to ensure clean recycling.
    addr_t aligned = page_size_helper_t::align_to_page(
        frame_paddr, page_size_kind_t::PAGE_SIZE_4KB);
    // Push the aligned frame onto the page-table free-list.
    free_pt_frames_.push_back(aligned);
    // Decrement the active page-table frame count if positive.
    if (allocated_pt_frames_ > 0) {
        --allocated_pt_frames_; // Decrement live PT frame count.
    }
}

addr_t
physical_frame_allocator_t::allocate_data_frame(page_size_kind_t kind)
{
    // Select the appropriate free-list corresponding to the requested page granularity.
    std::vector<addr_t> *free_list = &free_data_frames_4kb_;
    // If a 2 MB huge page is requested, use the 2 MB free-list.
    if (kind == page_size_kind_t::PAGE_SIZE_2MB) {
        free_list = &free_data_frames_2mb_; // Point to 2 MB free-list.
    } else if (kind == page_size_kind_t::PAGE_SIZE_1GB) {
        // If a 1 GB giant page is requested, use the 1 GB free-list.
        free_list = &free_data_frames_1gb_; // Point to 1 GB free-list.
    }
    // Check if the selected free-list has a recycled frame ready to reuse.
    if (!free_list->empty()) {
        // Grab the frame from the back of the free-list.
        addr_t reused = free_list->back();
        // Remove the frame from the free-list.
        free_list->pop_back();
        // Increment the active data frame counter.
        ++allocated_data_frames_;
        // Return the recycled leaf frame address.
        return reused;
    }
    // Determine the byte size of the requested frame (4 KB, 2 MB, or 1 GB).
    uint64_t frame_bytes = page_size_helper_t::get_page_size_bytes(kind);
    // Loop until we find an aligned physical range that does not collide with reserved frames.
    while (true) {
        // Round next_data_phys_addr_ up to the natural alignment of frame_bytes.
        addr_t aligned_candidate = static_cast<addr_t>(
            (next_data_phys_addr_ + frame_bytes - 1ULL) & ~(frame_bytes - 1ULL));
        // Advance the bump pointer past this candidate frame.
        next_data_phys_addr_ = aligned_candidate + static_cast<addr_t>(frame_bytes);
        // For 4 KB pages, check whether the candidate is in reserved_4kb_frames_.
        if (kind == page_size_kind_t::PAGE_SIZE_4KB) {
            // If the 4 KB frame is already reserved, continue searching.
            if (reserved_4kb_frames_.find(aligned_candidate) !=
                reserved_4kb_frames_.end()) {
                continue; // Skip reserved frame.
            }
        }
        // Increment the active leaf data frame count.
        ++allocated_data_frames_;
        // Return the naturally aligned physical base address.
        return aligned_candidate;
    }
}

void
physical_frame_allocator_t::free_data_frame(addr_t frame_paddr, page_size_kind_t kind)
{
    // Align the address to its natural page boundary.
    addr_t aligned = page_size_helper_t::align_to_page(frame_paddr, kind);
    // Push onto the matching free-list based on granularity.
    if (kind == page_size_kind_t::PAGE_SIZE_2MB) {
        free_data_frames_2mb_.push_back(aligned); // Recycle 2 MB frame.
    } else if (kind == page_size_kind_t::PAGE_SIZE_1GB) {
        free_data_frames_1gb_.push_back(aligned); // Recycle 1 GB frame.
    } else {
        free_data_frames_4kb_.push_back(aligned); // Recycle 4 KB frame.
    }
    // Decrement the active leaf data frame counter if positive.
    if (allocated_data_frames_ > 0) {
        --allocated_data_frames_; // Decrement live data frame count.
    }
}

void
physical_frame_allocator_t::reserve_external_frame(addr_t phys_addr,
                                                   page_size_kind_t kind)
{
    // Align the external physical address to the base of the specified page kind.
    addr_t aligned_base = page_size_helper_t::align_to_page(phys_addr, kind);
    // Compute the total byte size of the page kind.
    uint64_t total_bytes = page_size_helper_t::get_page_size_bytes(kind);
    // Calculate how many 4 KB base frames are covered by this reservation.
    uint64_t num_4kb_subpages = total_bytes / mmu_constants_t::PAGE_SIZE_4KB_BYTES;
    // Cap subpage insertion in the set if a 1 GB page is passed to avoid excessive memory.
    if (num_4kb_subpages > mmu_constants_t::PAGES_PER_2MB_HUGE_PAGE) {
        // Reserve the head 4 KB frame of the 1 GB region.
        reserved_4kb_frames_.insert(aligned_base);
        return; // Return early after recording head frame.
    }
    // Insert each constituent 4 KB sub-frame into the reserved set.
    for (uint64_t i = 0; i < num_4kb_subpages; ++i) {
        // Compute the physical address of the i-th 4 KB sub-frame.
        addr_t subframe_paddr = aligned_base +
            static_cast<addr_t>(i * mmu_constants_t::PAGE_SIZE_4KB_BYTES);
        // Record the sub-frame in the reserved set.
        reserved_4kb_frames_.insert(subframe_paddr);
    }
}

} // namespace drmemtrace
} // namespace dynamorio
