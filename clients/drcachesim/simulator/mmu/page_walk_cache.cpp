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

/* page_walk_cache.cpp: Implementation of page_walk_cache_t.
 */

#include "mmu/page_walk_cache.h" // Include class declaration.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

page_walk_cache_t::page_walk_cache_t(unsigned int num_entries, unsigned int assoc)
    // Store total entry count.
    : num_entries_(num_entries)
    // Clamp associativity so it is at least 1 and at most num_entries_ when enabled.
    , assoc_(num_entries == 0 ? 0 : (assoc == 0 ? 1 : (assoc > num_entries ? num_entries : assoc)))
    // Compute number of sets as num_entries_ / assoc_.
    , num_sets_(num_entries == 0 ? 0 : (num_entries_ / assoc_))
    // Initialize monotonic LRU clock to zero.
    , access_clock_(0)
{
    // If enabled, ensure num_sets_ is at least 1 and allocate the 2D set-way table.
    if (num_entries_ > 0) {
        if (num_sets_ == 0) {
            num_sets_ = 1;           // Guarantee at least one set.
            assoc_ = num_entries_;   // Fully associative fallback.
        }
        // Resize outer vector to `num_sets_` and each inner vector to `assoc_`.
        sets_.resize(num_sets_, std::vector<pwc_entry_t>(assoc_));
    }
}

size_t
page_walk_cache_t::compute_set_index(addr_t vaddr_tag, ptw_level_t level) const
{
    // Combine the shifted virtual address tag with the radix level to spread sets.
    uint64_t hash_val =
        static_cast<uint64_t>(vaddr_tag) ^ (static_cast<uint64_t>(level) * 0x9e3779b9ULL);
    // Modulo by the number of sets.
    return static_cast<size_t>(hash_val % num_sets_);
}

bool
page_walk_cache_t::lookup(memref_pid_t pid, ptw_level_t level, addr_t vaddr,
                          addr_t *next_table_paddr_out)
{
    // If the Page Walk Cache is disabled or level is not an intermediate directory, miss.
    if (!is_enabled() || level == ptw_level_t::PTW_LEVEL_1_PTE ||
        level == ptw_level_t::PTW_LEVEL_INVALID) {
        return false; // PWC only caches intermediate levels L4, L3, and L2.
    }
    // Compute the virtual address prefix tag for `level`.
    addr_t tag = compute_vaddr_tag(vaddr, level);
    // Compute the target set index.
    size_t set_idx = compute_set_index(tag, level);
    // Scan all ways in the target set.
    std::vector<pwc_entry_t> &ways = sets_[set_idx];
    for (size_t w = 0; w < ways.size(); ++w) {
        pwc_entry_t &entry = ways[w];
        // Check if this way is valid and matches (pid, level, tag).
        if (entry.valid && entry.pid == pid && entry.level == level &&
            entry.vaddr_tag == tag) {
            // Update the LRU timestamp on hit.
            entry.lru_tick = ++access_clock_;
            // Store the cached child table physical base address if requested.
            if (next_table_paddr_out != nullptr) {
                *next_table_paddr_out = entry.next_table_paddr;
            }
            return true; // PWC Hit!
        }
    }
    // No matching entry found in the set.
    return false;
}

void
page_walk_cache_t::insert(memref_pid_t pid, ptw_level_t level, addr_t vaddr,
                          addr_t next_table_paddr)
{
    // Do not insert if disabled or if `level` is a leaf PTE (which goes in the TLB).
    if (!is_enabled() || level == ptw_level_t::PTW_LEVEL_1_PTE ||
        level == ptw_level_t::PTW_LEVEL_INVALID) {
        return;
    }
    // Compute the virtual address prefix tag and set index.
    addr_t tag = compute_vaddr_tag(vaddr, level);
    size_t set_idx = compute_set_index(tag, level);
    std::vector<pwc_entry_t> &ways = sets_[set_idx];
    // First check if an entry for (pid, level, tag) already exists in the set.
    size_t victim_way = 0;
    bool found_invalid = false;
    uint64_t oldest_tick = UINT64_MAX;
    for (size_t w = 0; w < ways.size(); ++w) {
        pwc_entry_t &entry = ways[w];
        if (entry.valid && entry.pid == pid && entry.level == level &&
            entry.vaddr_tag == tag) {
            // Update existing entry in place.
            entry.next_table_paddr = next_table_paddr;
            entry.lru_tick = ++access_clock_;
            return;
        }
        // Prefer an invalid slot if one is available.
        if (!entry.valid && !found_invalid) {
            victim_way = w;
            found_invalid = true;
        } else if (!found_invalid && entry.lru_tick < oldest_tick) {
            // Track the least-recently-used valid way.
            oldest_tick = entry.lru_tick;
            victim_way = w;
        }
    }
    // Populate the chosen victim way with the new intermediate translation.
    pwc_entry_t &victim = ways[victim_way];
    victim.valid = true;
    victim.pid = pid;
    victim.level = level;
    victim.vaddr_tag = tag;
    victim.next_table_paddr = next_table_paddr;
    victim.lru_tick = ++access_clock_;
}

void
page_walk_cache_t::invalidate_pid(memref_pid_t pid)
{
    // Iterate over all sets and ways, invalidating entries belonging to `pid`.
    for (auto &ways : sets_) {
        for (auto &entry : ways) {
            if (entry.valid && entry.pid == pid) {
                entry.valid = false; // Invalidate matching PID entry.
            }
        }
    }
}

void
page_walk_cache_t::invalidate_vaddr(memref_pid_t pid, addr_t vaddr)
{
    // If disabled, nothing to invalidate.
    if (!is_enabled()) {
        return;
    }
    // Check each intermediate level (L4, L3, L2) and invalidate matching tags.
    const ptw_level_t levels[] = { ptw_level_t::PTW_LEVEL_4_PML4E,
                                   ptw_level_t::PTW_LEVEL_3_PDPTE,
                                   ptw_level_t::PTW_LEVEL_2_PDE };
    for (ptw_level_t lvl : levels) {
        addr_t tag = compute_vaddr_tag(vaddr, lvl);
        size_t set_idx = compute_set_index(tag, lvl);
        for (auto &entry : sets_[set_idx]) {
            if (entry.valid && entry.pid == pid && entry.level == lvl &&
                entry.vaddr_tag == tag) {
                entry.valid = false; // Invalidate matching prefix entry.
            }
        }
    }
}

void
page_walk_cache_t::flush_all()
{
    // Mark every entry across all sets and ways as invalid.
    for (auto &ways : sets_) {
        for (auto &entry : ways) {
            entry.valid = false;
        }
    }
}

} // namespace drmemtrace
} // namespace dynamorio
