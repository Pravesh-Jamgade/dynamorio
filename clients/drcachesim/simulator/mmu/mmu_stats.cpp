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

/* mmu_stats.cpp: Implementation of mmu_stats_t.
 */

#include "mmu/mmu_stats.h" // Include class declaration.

#include <iomanip>  // Provide std::setw, std::setprecision, std::fixed.
#include <iostream> // Provide std::cerr.
#include <locale>   // Provide std::locale for number formatting.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

mmu_stats_t::mmu_stats_t()
{
    // Initialize all statistics counters to zero via reset().
    reset();
}

void
mmu_stats_t::reset()
{
    // Reset overall page table walk counters.
    total_ptw_walks_ = 0;
    walks_4kb_ = 0;
    walks_2mb_ = 0;
    walks_1gb_ = 0;
    page_faults_ = 0;
    total_ptw_memory_reads_ = 0;
    // Zero per-level PTW memory read counters.
    ptw_reads_per_level_.fill(0);
    // Reset Page Walk Cache counters.
    total_pwc_hits_ = 0;
    total_pwc_misses_ = 0;
    pwc_hits_per_level_.fill(0);
    pwc_misses_per_level_.fill(0);
    // Reset coupled PTW data cache counters.
    ptw_cache_hits_ = 0;
    ptw_cache_misses_ = 0;
    // Zero per-level cache hits matrix [level 1..4][cache_level 0..3].
    for (auto &arr : ptw_level_cache_hits_) {
        arr.fill(0); // Zero each row.
    }
    // Zero aggregate per-cache-level hit counters.
    ptw_cache_hits_l1d_ = 0;   // Reset L1D hit counter.
    ptw_cache_hits_l2_ = 0;    // Reset L2 hit counter.
    ptw_cache_hits_l3_ = 0;    // Reset L3 hit counter.
    ptw_cache_hits_dram_ = 0;  // Reset DRAM counter.
    // Reset TLB shootdown counter.
    tlb_shootdowns_ = 0;
}

void
mmu_stats_t::record_walk(const ptw_result_t &result)
{
    // Increment the total number of hardware page table walks.
    ++total_ptw_walks_;
    // Increment the counter matching the resolved page granularity.
    switch (result.page_size_kind) {
    case page_size_kind_t::PAGE_SIZE_4KB: // Standard 4 KB walk.
        ++walks_4kb_;
        break;
    case page_size_kind_t::PAGE_SIZE_2MB: // 2 MB huge-page walk.
        ++walks_2mb_;
        break;
    case page_size_kind_t::PAGE_SIZE_1GB: // 1 GB giant-page walk.
        ++walks_1gb_;
        break;
    }
    // If this walk triggered a first-touch demand page fault, increment page_faults_.
    if (result.is_page_fault) {
        ++page_faults_;
    }
    // Add the number of 8-byte PTE memory reads issued during this walk.
    total_ptw_memory_reads_ += result.num_memory_reads;
    // Inspect each step in the walk to count memory reads per radix level.
    for (const auto &step : result.steps) {
        // Only steps that were not skipped/satisfied by a PWC hit issued a memory read.
        if (!step.pwc_hit) {
            int lvl_idx = static_cast<int>(step.level);
            if (lvl_idx >= 1 && lvl_idx <= 4) {
                ++ptw_reads_per_level_[lvl_idx];
            }
        }
    }
}

void
mmu_stats_t::record_pwc_access(ptw_level_t level, bool hit)
{
    // Convert the radix level to an integer index in [1..4].
    int lvl_idx = static_cast<int>(level);
    if (lvl_idx < 1 || lvl_idx > 4) {
        return; // Guard against out-of-range level values.
    }
    // Update hit or miss counters for the PWC.
    if (hit) {
        ++total_pwc_hits_;               // Increment total PWC hits.
        ++pwc_hits_per_level_[lvl_idx];  // Increment PWC hits at `level`.
    } else {
        ++total_pwc_misses_;              // Increment total PWC misses.
        ++pwc_misses_per_level_[lvl_idx]; // Increment PWC misses at `level`.
    }
}

void
mmu_stats_t::record_ptw_cache_access(bool cache_hit)
{
    // Increment coupled data cache hit or miss counter for the 8-byte PTE read.
    if (cache_hit) {
        ++ptw_cache_hits_;
    } else {
        ++ptw_cache_misses_;
    }
}

void
mmu_stats_t::record_ptw_cache_access(ptw_level_t ptw_level,
                                     ptw_cache_hit_level_t cache_hit_level)
{
    // Convert radix level enum to an integer array index [1..4].
    int lvl_idx = static_cast<int>(ptw_level);
    // Convert cache hit level enum to an integer array index [0..3].
    int hit_idx = static_cast<int>(cache_hit_level);
    // Record into per-level cache hits matrix if indices are within bounds.
    if (lvl_idx >= 1 && lvl_idx <= 4 && hit_idx >= 0 && hit_idx <= 3) {
        ++ptw_level_cache_hits_[lvl_idx][hit_idx]; // Increment specific cell.
    }
    // Update aggregate per-cache-level and legacy hit/miss counters.
    switch (cache_hit_level) {
    case ptw_cache_hit_level_t::HIT_L1D: // Hit in L1 Data Cache.
        ++ptw_cache_hits_l1d_;           // Increment L1D aggregate hits.
        ++ptw_cache_hits_;               // Increment legacy cache hit counter.
        break;                           // Break out of switch.
    case ptw_cache_hit_level_t::HIT_L2:  // Hit in L2 Unified Cache.
        ++ptw_cache_hits_l2_;            // Increment L2 aggregate hits.
        ++ptw_cache_misses_;             // Missed L1D from CPU core's perspective.
        break;                           // Break out of switch.
    case ptw_cache_hit_level_t::HIT_L3:  // Hit in L3 / LLC Cache.
        ++ptw_cache_hits_l3_;            // Increment L3 aggregate hits.
        ++ptw_cache_misses_;             // Missed L1D from CPU core's perspective.
        break;                           // Break out of switch.
    case ptw_cache_hit_level_t::HIT_DRAM: // Missed all caches; serviced by DRAM.
        ++ptw_cache_hits_dram_;           // Increment DRAM aggregate accesses.
        ++ptw_cache_misses_;              // Missed L1D from CPU core's perspective.
        break;                            // Break out of switch.
    }
}

void
mmu_stats_t::print_stats(const std::string &prefix) const
{
    // Format numbers with locale thousands separators to match caching_device_stats_t.
    std::cerr.imbue(std::locale(""));
    std::cerr << prefix << std::setw(22) << std::left << "Page table walks:"
              << std::setw(16) << std::right << total_ptw_walks_ << std::endl;
    std::cerr << prefix << std::setw(22) << std::left << "4KB page walks:"
              << std::setw(16) << std::right << walks_4kb_ << std::endl;
    std::cerr << prefix << std::setw(22) << std::left << "2MB huge page walks:"
              << std::setw(16) << std::right << walks_2mb_ << std::endl;
    std::cerr << prefix << std::setw(22) << std::left << "1GB huge page walks:"
              << std::setw(16) << std::right << walks_1gb_ << std::endl;
    std::cerr << prefix << std::setw(22) << std::left << "Demand page faults:"
              << std::setw(16) << std::right << page_faults_ << std::endl;
    std::cerr << prefix << std::setw(22) << std::left << "PTW memory reads:"
              << std::setw(16) << std::right << total_ptw_memory_reads_ << std::endl;
    std::cerr << prefix << std::setw(22) << std::left << "PWC hits:"
              << std::setw(16) << std::right << total_pwc_hits_ << std::endl;
    std::cerr << prefix << std::setw(22) << std::left << "PWC misses:"
              << std::setw(16) << std::right << total_pwc_misses_ << std::endl;
    if (ptw_cache_hits_ + ptw_cache_misses_ > 0 ||
        ptw_cache_hits_l1d_ + ptw_cache_hits_l2_ + ptw_cache_hits_l3_ +
            ptw_cache_hits_dram_ > 0) {
        std::cerr << prefix << std::setw(22) << std::left << "PTW cache hits:"
                  << std::setw(16) << std::right << ptw_cache_hits_ << std::endl;
        std::cerr << prefix << std::setw(22) << std::left << "PTW cache misses:"
                  << std::setw(16) << std::right << ptw_cache_misses_ << std::endl;
        std::cerr << prefix << "PTW cache hits by walk level:" << std::endl;
        // Descriptor table mapping radix levels to display labels.
        const struct {
            ptw_level_t level; // Radix walk level.
            const char *name;  // Formatted level name string.
        } levels[] = {
            { ptw_level_t::PTW_LEVEL_4_PML4E, "  L4 (PML4E):" }, // Level 4 descriptor.
            { ptw_level_t::PTW_LEVEL_3_PDPTE, "  L3 (PDPTE):" }, // Level 3 descriptor.
            { ptw_level_t::PTW_LEVEL_2_PDE,   "  L2 (PDE):  " }, // Level 2 descriptor.
            { ptw_level_t::PTW_LEVEL_1_PTE,   "  L1 (PTE):  " }, // Level 1 descriptor.
        };
        // Print the hit breakdown across L1D, L2, L3, and DRAM for each walk level.
        for (const auto &item : levels) {
            int lvl_idx = static_cast<int>(item.level); // Extract level index.
            int64_t l1d = ptw_level_cache_hits_[lvl_idx]
                [static_cast<int>(ptw_cache_hit_level_t::HIT_L1D)]; // L1D hits.
            int64_t l2 = ptw_level_cache_hits_[lvl_idx]
                [static_cast<int>(ptw_cache_hit_level_t::HIT_L2)];  // L2 hits.
            int64_t l3 = ptw_level_cache_hits_[lvl_idx]
                [static_cast<int>(ptw_cache_hit_level_t::HIT_L3)];  // L3 hits.
            int64_t dram = ptw_level_cache_hits_[lvl_idx]
                [static_cast<int>(ptw_cache_hit_level_t::HIT_DRAM)]; // DRAM accesses.
            std::cerr << prefix << item.name << " "
                      << "L1D=" << std::setw(8) << std::right << l1d << "  "
                      << "L2="  << std::setw(8) << std::right << l2  << "  "
                      << "L3="  << std::setw(8) << std::right << l3  << "  "
                      << "DRAM=" << std::setw(8) << std::right << dram
                      << std::endl; // Output formatted line.
        }
    }
    if (tlb_shootdowns_ > 0) {
        std::cerr << prefix << std::setw(22) << std::left << "TLB shootdowns:"
                  << std::setw(16) << std::right << tlb_shootdowns_ << std::endl;
    }
    if (total_ptw_walks_ > 0) {
        double avg_steps =
            static_cast<double>(total_ptw_memory_reads_) / total_ptw_walks_;
        std::cerr << prefix << std::setw(22) << std::left << "Avg PTW mem reads:"
                  << std::setw(16) << std::fixed << std::setprecision(2) << std::right
                  << avg_steps << std::endl;
    }
    // Restore standard C locale.
    std::cerr.imbue(std::locale("C"));
}

} // namespace drmemtrace
} // namespace dynamorio
