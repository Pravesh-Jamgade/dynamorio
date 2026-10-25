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

/* tlb.cpp: Implementation of tlb_t, tlb_lru_t, and tlb_fifo_t.
 */

#include "tlb.h" // Include class declarations.

#include <assert.h> // Provide assert() macro.
#include <stddef.h> // Provide NULL and size_t definitions.
#include <vector>   // Provide std::vector.

#include "caching_device.h"       // Provide base caching_device_t methods.
#include "caching_device_block.h" // Provide caching_device_block_t and TAG_INVALID.
#include "memref.h"               // Provide memref_t.
#include "tlb_entry.h"            // Provide tlb_entry_t.
#include "tlb_stats.h"            // Provide tlb_stats_t.
#include "trace_entry.h"          // Provide trace_type_t constants.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

void
tlb_t::init_blocks()
{
    // Allocate a tlb_entry_t object for every slot across all sets and ways.
    for (int i = 0; i < num_blocks_; i++) {
        tlb_entry_t *entry = new tlb_entry_t; // Instantiate extended TLB entry block.
        entry->sector_size_ = sector_size_;   // Propagate configured sector size to block.
        blocks_[i] = entry;                   // Assign entry to block slot.
    }
}

addr_t
tlb_t::compute_canonical_tag(memref_pid_t pid, addr_t addr, page_size_kind_t *kind_out,
                             uint64_t *page_bytes_out) const
{
    // Default to standard 4 KB granularity and this TLB's configured block_size_.
    page_size_kind_t kind = page_size_kind_t::PAGE_SIZE_4KB;
    uint64_t eff_bytes = static_cast<uint64_t>(block_size_);
    addr_t aligned_addr = addr;
    // If a huge-page detector is attached, query whether (pid, addr) is 2 MB or 1 GB.
    if (huge_page_detector_ != nullptr) {
        kind = huge_page_detector_->get_page_size_kind(pid, addr);
        // If backed by a 2 MB or 1 GB huge page, align `addr` to the huge page base.
        if (kind != page_size_kind_t::PAGE_SIZE_4KB) {
            eff_bytes = page_size_helper_t::get_page_size_bytes(kind);
            aligned_addr = page_size_helper_t::align_to_page(addr, kind);
        }
    }
    // Populate optional output parameters if requested by caller.
    if (kind_out != nullptr) {
        *kind_out = kind;
    }
    if (page_bytes_out != nullptr) {
        *page_bytes_out = eff_bytes;
    }
    // Shift the aligned virtual address by block_size_bits_ to produce the VPN tag.
    return compute_tag(aligned_addr);
}

void
tlb_t::request(const memref_t &memref_in)
{
    // Extract the process ID from the incoming memory reference.
    memref_pid_t pid = memref_in.data.pid;
    // Compute the inclusive final byte address touched by this memory reference.
    addr_t start_addr = memref_in.data.addr;
    size_t access_size = (memref_in.data.size > 0) ? memref_in.data.size : 1;
    addr_t final_addr = start_addr + access_size - 1 /*avoid overflow*/;
    // Compute the canonical tag, page kind, and effective page size for start and end.
    page_size_kind_t start_kind = page_size_kind_t::PAGE_SIZE_4KB;
    uint64_t start_page_bytes = static_cast<uint64_t>(block_size_);
    addr_t tag = compute_canonical_tag(pid, start_addr, &start_kind, &start_page_bytes);
    addr_t final_tag = compute_canonical_tag(pid, final_addr, nullptr, nullptr);

    // Compute sub-entry index within sector for start address.
    size_t fast_sub_idx = (sector_size_ > 1) ? ((start_addr >> 12) & (sector_size_ - 1)) : 0;

    // Optimization: fast-path repeated single-page accesses to the same (tag, pid).
    if (tag == final_tag && tag == last_tag_ && pid == last_pid_) {
        // Retrieve the cached block from the last accessed set and way.
        caching_device_block_t *tlb_entry =
            &get_caching_device_block(last_block_idx_, last_way_);
        tlb_entry_t *typed_fast = (tlb_entry_t *)tlb_entry;
        // Verify entry tag matches, PID matches, and sector sub-entry is valid.
        if (tag != TAG_INVALID && tag == tlb_entry->tag_ && pid == typed_fast->pid_ &&
            typed_fast->is_sub_entry_valid(fast_sub_idx)) {
            // If this access is a write, mark the TLB entry dirty.
            if (memref_in.data.type == TRACE_TYPE_WRITE) {
                typed_fast->set_sub_entry_dirty(fast_sub_idx, true);
            }
            // Record the access hit in base stats and TLB page-size stats.
            record_access_stats(memref_in, true /*hit*/, tlb_entry);
            if (stats_ != nullptr) {
                ((tlb_stats_t *)stats_)
                    ->record_page_size_access(typed_fast->get_sub_entry_kind(fast_sub_idx),
                                              true /*hit*/);
            }
            // Update replacement policy state (e.g., LFU counter or LRU age).
            access_update(last_block_idx_, last_way_);
            return;
        }
    }

    // Make a mutable copy of `memref_in` to slice across page boundaries if needed.
    memref_t memref = memref_in;
    addr_t cur_addr = start_addr;
    while (true) {
        // Resolve canonical tag, page kind, and page byte size for `cur_addr`.
        page_size_kind_t cur_kind = page_size_kind_t::PAGE_SIZE_4KB;
        uint64_t cur_page_bytes = static_cast<uint64_t>(block_size_);
        addr_t cur_tag =
            compute_canonical_tag(pid, cur_addr, &cur_kind, &cur_page_bytes);
        // Compute the exclusive end address of the current page.
        addr_t cur_page_end =
            (cur_addr & ~static_cast<addr_t>(cur_page_bytes - 1ULL)) +
            static_cast<addr_t>(cur_page_bytes);
        // Check if the memory reference extends into a subsequent page.
        bool has_more_pages = (cur_page_end - 1 < final_addr) && (cur_tag != final_tag);
        if (has_more_pages) {
            // Slice `memref.data.size` to the bytes within the current page.
            memref.data.size = cur_page_end - cur_addr;
        }
        // Compute the set start index for `cur_tag`.
        int block_idx = compute_block_idx(cur_tag);
        int way = 0;
        // Compute sub-entry index for current page address.
        size_t cur_sub_idx = (sector_size_ > 1) ? ((cur_addr >> 12) & (sector_size_ - 1)) : 0;
        // Scan all ways in the set for a valid entry matching (cur_tag, pid).
        for (way = 0; way < associativity_; ++way) {
            caching_device_block_t *tlb_entry = &get_caching_device_block(block_idx, way);
            tlb_entry_t *typed_entry = (tlb_entry_t *)tlb_entry;
            if (tlb_entry->tag_ == cur_tag && typed_entry->pid_ == pid) {
                // If sectored TLB, verify specific sub-entry is valid.
                if (sector_size_ > 1 && !typed_entry->is_sub_entry_valid(cur_sub_idx)) {
                    continue; // Sub-entry not valid; fall through to service miss.
                }
                // TLB Hit! Mark dirty on write.
                if (memref.data.type == TRACE_TYPE_WRITE) {
                    typed_entry->set_sub_entry_dirty(cur_sub_idx, true);
                }
                // Record hit in base stats and TLB page-size stats.
                record_access_stats(memref, true /*hit*/, tlb_entry);
                if (stats_ != nullptr) {
                    ((tlb_stats_t *)stats_)
                        ->record_page_size_access(
                            typed_entry->get_sub_entry_kind(cur_sub_idx), true /*hit*/);
                }
                break;
            }
        }

        // If no way matched (`way == associativity_`), handle the TLB miss.
        if (way == associativity_) {
            // Check if there is an existing block with matching tag but missing sub-entry.
            int existing_way = -1;
            if (sector_size_ > 1) {
                for (int w = 0; w < associativity_; ++w) {
                    caching_device_block_t *b = &get_caching_device_block(block_idx, w);
                    tlb_entry_t *tb = (tlb_entry_t *)b;
                    if (b->tag_ == cur_tag && tb->pid_ == pid) {
                        existing_way = w;
                        break;
                    }
                }
            }
            if (existing_way >= 0) {
                way = existing_way; // Reuse existing sector block slot.
            } else {
                // Select the victim way in this set according to the replacement policy.
                way = replace_which_way(block_idx);
            }
            caching_device_block_t *tlb_entry = &get_caching_device_block(block_idx, way);
            tlb_entry_t *typed_entry = (tlb_entry_t *)tlb_entry;
            // Track loaded_blocks_ when filling a previously invalid slot.
            if (tlb_entry->tag_ == TAG_INVALID) {
                loaded_blocks_++;
            }
            // Record the miss in base stats and TLB page-size stats.
            record_access_stats(memref, false /*miss*/, tlb_entry);
            if (stats_ != nullptr) {
                ((tlb_stats_t *)stats_)->record_page_size_access(cur_kind, false /*miss*/);
            }
            // Default resolved PPN to cur_tag and resolved kind to cur_kind.
            addr_t resolved_ppn = cur_tag;
            page_size_kind_t resolved_kind = cur_kind;
            // If this TLB has a parent TLB (e.g., L1 -> L2 TLB), forward the miss.
            if (parent_ != NULL) {
                parent_->request(memref);
                // Retrieve the resolved PPN and page size kind installed in the parent TLB.
                ((tlb_t *)parent_)
                    ->lookup_entry(pid, memref.data.addr, &resolved_ppn, &resolved_kind);
            } else if (page_table_walker_ != nullptr) {
                // This is a last-level (L2) TLB miss and a hardware page table walker is
                // attached: execute the multi-level radix page table walk!
                ptw_result_t ptw_res;
                ptw_res.sector_size = sector_size_;
                if (page_table_walker_->walk(memref, memref.data.addr, &ptw_res)) {
                    resolved_ppn = ptw_res.page_base_paddr >> 12;
                    resolved_kind = ptw_res.page_size_kind;
                    // Install all sector entries brought in from the leaf PTE block!
                    if (sector_size_ > 1) {
                        typed_entry->sector_size_ = sector_size_;
                        for (size_t s = 0; s < sector_size_; ++s) {
                            if (s < ptw_res.sector_entries.size()) {
                                typed_entry->set_sub_entry(
                                    s, ptw_res.sector_entries[s].ppn,
                                    ptw_res.sector_entries[s].page_size_kind,
                                    ptw_res.sector_entries[s].dirty);
                            } else {
                                typed_entry->set_sub_entry(s, resolved_ppn + s, resolved_kind, false);
                            }
                        }
                    }
                }
            }
            // Install the translation into the selected way of this TLB.
            tlb_entry->tag_ = cur_tag;
            typed_entry->pid_ = pid;
            typed_entry->ppn_ = resolved_ppn;
            typed_entry->page_size_kind_ = resolved_kind;
            typed_entry->set_sub_entry_dirty(cur_sub_idx, memref.data.type == TRACE_TYPE_WRITE);
        }

        // Update replacement policy state for the accessed way.
        access_update(block_idx, way);

        // Cache the last accessed tag, way, set index, and PID for fast-pathing.
        last_tag_ = cur_tag;
        last_way_ = way;
        last_block_idx_ = block_idx;
        last_pid_ = pid;

        // If no further pages are spanned by this memory reference, exit the loop.
        if (!has_more_pages) {
            break;
        }
        // Advance `cur_addr` and `memref` to the start of the next page.
        cur_addr = cur_page_end;
        memref.data.addr = cur_addr;
        memref.data.size = final_addr - cur_addr + 1 /*undo the -1*/;
    }
}

bool
tlb_t::contains_tag_pid(addr_t tag, memref_pid_t pid) const
{
    // Compute the set index for `tag`.
    int block_idx = compute_block_idx(tag);
    // Check all ways in this TLB for a matching (tag, pid) entry.
    for (int way = 0; way < associativity_; ++way) {
        const caching_device_block_t &block = get_caching_device_block(block_idx, way);
        if (block.tag_ == tag && ((const tlb_entry_t &)block).pid_ == pid) {
            return true; // Found in this TLB.
        }
    }
    // If inclusive or no children, no need to check children.
    if (children_.empty() || is_inclusive()) {
        return false;
    }
    // Check child TLBs recursively.
    for (auto *child : children_) {
        if (((const tlb_t *)child)->contains_tag_pid(tag, pid)) {
            return true;
        }
    }
    return false;
}

bool
tlb_t::lookup_entry(memref_pid_t pid, addr_t vaddr, addr_t *ppn_out,
                    page_size_kind_t *kind_out) const
{
    // Compute the canonical tag for (pid, vaddr).
    addr_t tag = compute_canonical_tag(pid, vaddr, nullptr, nullptr);
    int block_idx = compute_block_idx(tag);
    // Compute sub-entry index within sector for given vaddr.
    size_t sub_idx = (sector_size_ > 1) ? ((vaddr >> 12) & (sector_size_ - 1)) : 0;
    // Search all ways in the set.
    for (int way = 0; way < associativity_; ++way) {
        const caching_device_block_t &block = get_caching_device_block(block_idx, way);
        const tlb_entry_t &tlb_entry = (const tlb_entry_t &)block;
        if (block.tag_ == tag && tlb_entry.pid_ == pid) {
            // In sector mode, verify that the requested sub-entry is valid.
            if (sector_size_ > 1) {
                if (!tlb_entry.is_sub_entry_valid(sub_idx)) {
                    return false; // Sub-entry not present in sector block.
                }
                if (ppn_out != nullptr) {
                    *ppn_out = tlb_entry.get_sub_entry_ppn(sub_idx); // Return sub-entry PPN.
                }
                if (kind_out != nullptr) {
                    *kind_out = tlb_entry.get_sub_entry_kind(sub_idx); // Return sub-entry kind.
                }
                return true;
            } else {
                if (ppn_out != nullptr) {
                    *ppn_out = tlb_entry.ppn_; // Return baseline PPN.
                }
                if (kind_out != nullptr) {
                    *kind_out = tlb_entry.page_size_kind_; // Return baseline kind.
                }
                return true;
            }
        }
    }
    return false;
}

void
tlb_t::invalidate_tag_pid(addr_t tag, memref_pid_t pid,
                          invalidation_type_t invalidation_type)
{
    // Compute the set index for `tag`.
    int block_idx = compute_block_idx(tag);
    // Scan ways in the set and invalidate any matching (tag, pid) entry.
    for (int way = 0; way < associativity_; ++way) {
        caching_device_block_t &block = get_caching_device_block(block_idx, way);
        tlb_entry_t &tlb_entry = (tlb_entry_t &)block;
        if (block.tag_ == tag && tlb_entry.pid_ == pid) {
            invalidate_caching_device_block(&block);
            loaded_blocks_--;
            // Invalidate all sector sub-entries.
            for (size_t s = 0; s < MAX_TLB_SECTOR_SIZE; ++s) {
                tlb_entry.sub_entries_[s].valid = false;
            }
            if (stats_ != nullptr) {
                stats_->invalidate(invalidation_type);
            }
            if (last_tag_ == tag && last_pid_ == pid) {
                last_tag_ = TAG_INVALID;
            }
        }
    }
    // Cascade invalidation to any child TLBs.
    for (auto *child : children_) {
        ((tlb_t *)child)->invalidate_tag_pid(tag, pid, invalidation_type);
    }
}

void
tlb_t::invalidate_vaddr(memref_pid_t pid, addr_t vaddr)
{
    // Invalidate canonical tag for (pid, vaddr) as well as 4KB, 2MB, and 1GB alignments
    // in case the page size was demoted/promoted prior to shootdown.
    addr_t canon_tag = compute_canonical_tag(pid, vaddr, nullptr, nullptr);
    invalidate_tag_pid(canon_tag, pid, INVALIDATION_COHERENCE);
    addr_t tag_4kb = compute_tag(
        page_size_helper_t::align_to_page(vaddr, page_size_kind_t::PAGE_SIZE_4KB));
    if (tag_4kb != canon_tag) {
        invalidate_tag_pid(tag_4kb, pid, INVALIDATION_COHERENCE);
    }
    addr_t tag_2mb = compute_tag(
        page_size_helper_t::align_to_page(vaddr, page_size_kind_t::PAGE_SIZE_2MB));
    if (tag_2mb != canon_tag && tag_2mb != tag_4kb) {
        invalidate_tag_pid(tag_2mb, pid, INVALIDATION_COHERENCE);
    }
}

void
tlb_t::invalidate_pid(memref_pid_t pid)
{
    // Scan all blocks across all sets and ways, invalidating entries for `pid`.
    for (int i = 0; i < num_blocks_; ++i) {
        caching_device_block_t *block = blocks_[i];
        tlb_entry_t *tlb_entry = (tlb_entry_t *)block;
        if (block->tag_ != TAG_INVALID && tlb_entry->pid_ == pid) {
            invalidate_caching_device_block(block);
            loaded_blocks_--;
            if (stats_ != nullptr) {
                stats_->invalidate(INVALIDATION_COHERENCE);
            }
        }
    }
    if (last_pid_ == pid) {
        last_tag_ = TAG_INVALID;
    }
    // Cascade PID flush to any child TLBs.
    for (auto *child : children_) {
        ((tlb_t *)child)->invalidate_pid(pid);
    }
}

void
tlb_t::invalidate(addr_t tag, invalidation_type_t invalidation_type)
{
    // Invalidate all ways matching `tag` in its set regardless of PID.
    int block_idx = compute_block_idx(tag);
    for (int way = 0; way < associativity_; ++way) {
        caching_device_block_t &block = get_caching_device_block(block_idx, way);
        if (block.tag_ == tag) {
            invalidate_caching_device_block(&block);
            loaded_blocks_--;
            if (stats_ != nullptr) {
                stats_->invalidate(invalidation_type);
            }
            if (last_tag_ == tag) {
                last_tag_ = TAG_INVALID;
            }
        }
    }
    for (auto *child : children_) {
        child->invalidate(tag, invalidation_type);
    }
}

/***************************************************************************
 * tlb_lru_t implementation (Least Recently Used replacement policy)
 */

bool
tlb_lru_t::init(int associativity, int block_size, int num_blocks,
                caching_device_t *parent, caching_device_stats_t *stats,
                prefetcher_t *prefetcher, cache_inclusion_policy_t inclusion_policy,
                bool coherent_cache, int id, snoop_filter_t *snoop_filter,
                const std::vector<caching_device_t *> &children)
{
    // Initialize base caching_device_t structure.
    if (!tlb_t::init(associativity, block_size, num_blocks, parent, stats, prefetcher,
                     inclusion_policy, coherent_cache, id, snoop_filter, children)) {
        return false;
    }
    // Seed LRU counters in each set to 0, 1, ..., associativity_ - 1.
    for (int i = 0; i < blocks_per_way_; i++) {
        for (int way = 0; way < associativity_; ++way) {
            get_caching_device_block(i * associativity_, way).counter_ = way;
        }
    }
    return true;
}

void
tlb_lru_t::access_update(int block_idx, int way)
{
    // Read the accessed entry's current age counter.
    int cnt = get_caching_device_block(block_idx, way).counter_;
    // Fast-path: if counter is already 0 (most recently used), no aging needed.
    if (cnt == 0) {
        return;
    }
    // Increment counters of all ways in the set that were more recent than `way`.
    for (int i = 0; i < associativity_; ++i) {
        if (i != way && get_caching_device_block(block_idx, i).counter_ <= cnt) {
            get_caching_device_block(block_idx, i).counter_++;
        }
    }
    // Mark the accessed `way` as most recently used (counter = 0).
    get_caching_device_block(block_idx, way).counter_ = 0;
}

int
tlb_lru_t::replace_which_way(int block_idx)
{
    // Return the way selected by get_next_way_to_replace().
    return get_next_way_to_replace(block_idx);
}

int
tlb_lru_t::get_next_way_to_replace(const int block_idx) const
{
    // Pick the first invalid slot, or the slot with the maximum age counter.
    int max_counter = 0;
    int max_way = 0;
    for (int way = 0; way < associativity_; ++way) {
        if (get_caching_device_block(block_idx, way).tag_ == TAG_INVALID) {
            max_way = way;
            break;
        }
        if (get_caching_device_block(block_idx, way).counter_ > max_counter) {
            max_counter = get_caching_device_block(block_idx, way).counter_;
            max_way = way;
        }
    }
    return max_way;
}

/***************************************************************************
 * tlb_fifo_t implementation (First-In-First-Out replacement policy)
 */

bool
tlb_fifo_t::init(int associativity, int block_size, int num_blocks,
                 caching_device_t *parent, caching_device_stats_t *stats,
                 prefetcher_t *prefetcher, cache_inclusion_policy_t inclusion_policy,
                 bool coherent_cache, int id, snoop_filter_t *snoop_filter,
                 const std::vector<caching_device_t *> &children)
{
    // Initialize base caching_device_t structure.
    if (!tlb_t::init(associativity, block_size, num_blocks, parent, stats, prefetcher,
                     inclusion_policy, coherent_cache, id, snoop_filter, children)) {
        return false;
    }
    // Set the FIFO replacement pointer (counter_ = 1) on way 0 of each set.
    for (int i = 0; i < blocks_per_way_; i++) {
        get_caching_device_block(i * associativity_, 0).counter_ = 1;
    }
    return true;
}

void
tlb_fifo_t::access_update(int block_idx, int way)
{
    // FIFO replacement order is unaffected by hits.
    (void)block_idx;
    (void)way;
}

int
tlb_fifo_t::replace_which_way(int block_idx)
{
    // Find the current FIFO victim way (whose counter_ == 1).
    int victim_way = get_next_way_to_replace(block_idx);
    // Clear the victim way's pointer bit.
    get_caching_device_block(block_idx, victim_way).counter_ = 0;
    // Advance the FIFO pointer to the next way in the set.
    get_caching_device_block(block_idx, (victim_way + 1) & (associativity_ - 1))
        .counter_ = 1;
    return victim_way;
}

int
tlb_fifo_t::get_next_way_to_replace(const int block_idx) const
{
    // Scan the set for the way marked with counter_ == 1.
    for (int i = 0; i < associativity_; i++) {
        if (get_caching_device_block(block_idx, i).counter_ == 1) {
            return i;
        }
    }
    assert(false);
    return 0;
}

} // namespace drmemtrace
} // namespace dynamorio
