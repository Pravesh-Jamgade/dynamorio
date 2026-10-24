/* **********************************************************
 * Copyright (c) 2026 Google, Inc.  All rights reserved.
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

// Implementation header.
#include "cpu/three_level_cache.h"

// Provide standard string stream for naming caches.
#include <sstream>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

three_level_cache_t::three_level_cache_t(const three_level_cache_knobs_t &knobs)
    // Store active hierarchy configuration knobs.
    : knobs_(knobs)
{
    // 1. Allocate and initialize the shared L3 / Last-Level Cache (LLC).
    l3_stats_.reset(new cache_stats_t(static_cast<int>(knobs_.line_size)));
    l3_cache_.reset(new cache_lru_t("Shared_LLC"));
    // Initialize L3 with configured associativity, line size, total capacity, and stats.
    l3_cache_->init(knobs_.l3_assoc, static_cast<int>(knobs_.line_size),
                    static_cast<int>(knobs_.l3_size), /*parent=*/nullptr,
                    l3_stats_.get());

    // Allocate per-core vectors for L1D and L2 caches and statistics.
    l1d_caches_.resize(knobs_.num_cores);
    l1d_stats_.resize(knobs_.num_cores);
    l2_caches_.resize(knobs_.num_cores);
    l2_stats_.resize(knobs_.num_cores);

    // 2. Instantiate and connect private L1D and L2 caches for each simulated core.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        // Build descriptive names for L2 and L1D caches on core i.
        std::string l2_name = "Core_" + std::to_string(i) + "_L2";
        std::string l1d_name = "Core_" + std::to_string(i) + "_L1D";

        // Allocate L2 cache and stats for core i.
        l2_stats_[i].reset(new cache_stats_t(static_cast<int>(knobs_.line_size)));
        l2_caches_[i].reset(new cache_lru_t(l2_name));
        // L2's parent is the shared L3 / Last-Level Cache.
        l2_caches_[i]->init(knobs_.l2_assoc, static_cast<int>(knobs_.line_size),
                            static_cast<int>(knobs_.l2_size), l3_cache_.get(),
                            l2_stats_[i].get());

        // Allocate L1D cache and stats for core i.
        l1d_stats_[i].reset(new cache_stats_t(static_cast<int>(knobs_.line_size)));
        l1d_caches_[i].reset(new cache_lru_t(l1d_name));
        // L1D's parent is core i's private L2 cache.
        l1d_caches_[i]->init(knobs_.l1d_assoc, static_cast<int>(knobs_.line_size),
                             static_cast<int>(knobs_.l1d_size), l2_caches_[i].get(),
                             l1d_stats_[i].get());
    }
}

three_level_cache_t::~three_level_cache_t()
{
    // unique_ptr automatically cleans up all cache devices and statistics objects.
}

cache_access_result_t
three_level_cache_t::request(unsigned int core, const memref_t &memref)
{
    // Initialize result structure.
    cache_access_result_t res;

    // Guard core index bounds.
    if (core >= knobs_.num_cores) {
        return res;
    }

    // Capture hit counts across L1D, L2, and L3 prior to processing the reference.
    int64_t pre_l1d_hits = l1d_stats_[core]->get_metric(metric_name_t::HITS);
    int64_t pre_l2_hits = l2_stats_[core]->get_metric(metric_name_t::HITS);
    int64_t pre_l3_hits = l3_stats_->get_metric(metric_name_t::HITS);

    // Issue the reference to core's private L1D cache.
    // If L1D misses, caching_device_t automatically cascades the request to L2, and L2 to L3!
    l1d_caches_[core]->request(memref);

    // Capture post-access hit counts to determine which level serviced the access.
    int64_t post_l1d_hits = l1d_stats_[core]->get_metric(metric_name_t::HITS);
    int64_t post_l2_hits = l2_stats_[core]->get_metric(metric_name_t::HITS);
    int64_t post_l3_hits = l3_stats_->get_metric(metric_name_t::HITS);

    // Check if the access was satisfied by L1D.
    if (post_l1d_hits > pre_l1d_hits) {
        res.hit_l1d = true;
        res.level = cache_hit_level_t::HIT_L1D;
        res.latency_cycles = knobs_.l1d_latency;
    }
    // Check if the access missed in L1D but was satisfied by L2.
    else if (post_l2_hits > pre_l2_hits) {
        res.hit_l2 = true;
        res.level = cache_hit_level_t::HIT_L2;
        res.latency_cycles = knobs_.l1d_latency + knobs_.l2_latency;
    }
    // Check if the access missed in L1D and L2 but was satisfied by shared L3 / LLC.
    else if (post_l3_hits > pre_l3_hits) {
        res.hit_l3 = true;
        res.level = cache_hit_level_t::HIT_L3;
        res.latency_cycles = knobs_.l1d_latency + knobs_.l2_latency + knobs_.l3_latency;
    }
    // Access missed in all cache levels; serviced by main memory DRAM.
    else {
        res.level = cache_hit_level_t::HIT_DRAM;
        res.latency_cycles =
            knobs_.l1d_latency + knobs_.l2_latency + knobs_.l3_latency + knobs_.dram_latency;
    }

    // Return the completed access outcome.
    return res;
}

cache_access_result_t
three_level_cache_t::ptw_request(unsigned int core, addr_t pte_phys_addr)
{
    // Synthesize an 8-byte physical memory read reference for the Page Table Entry.
    memref_t ptw_ref = {};
    ptw_ref.data.type = TRACE_TYPE_READ;      // PTE memory reads are 8-byte loads.
    ptw_ref.data.pid = 0;                    // Kernel / architectural address space.
    ptw_ref.data.tid = core;                 // Associate with issuing hardware thread/core.
    ptw_ref.data.addr = pte_phys_addr;       // 8-byte physical address of the PTE.
    ptw_ref.data.size = 8;                   // Size of a 64-bit page table entry.
    ptw_ref.data.pc = 0;                     // Hardware PTW access has no application PC.

    // Route the physical PTE memory read through the core's 3-level data cache hierarchy.
    return request(core, ptw_ref);
}

caching_device_t *
three_level_cache_t::get_l1d_cache(unsigned int core) const
{
    // Validate core index.
    if (core < l1d_caches_.size()) {
        return l1d_caches_[core].get(); // Return private L1D cache device.
    }
    return nullptr;
}

caching_device_t *
three_level_cache_t::get_l2_cache(unsigned int core) const
{
    // Validate core index.
    if (core < l2_caches_.size()) {
        return l2_caches_[core].get(); // Return private L2 cache device.
    }
    return nullptr;
}

caching_device_t *
three_level_cache_t::get_l3_cache() const
{
    return l3_cache_.get(); // Return shared L3 / LLC cache device.
}

void
three_level_cache_t::print_stats(const std::string &prefix) const
{
    // Print statistics for private L1D and L2 caches for each core.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        std::cerr << prefix << "Core #" << i << " 3-Level Cache stats:\n";
        std::cerr << prefix << "  L1D stats:\n";
        l1d_stats_[i]->print_stats(prefix + "    ");
        std::cerr << prefix << "  L2 stats:\n";
        l2_stats_[i]->print_stats(prefix + "    ");
    }
    // Print statistics for the shared L3 / Last-Level Cache.
    std::cerr << prefix << "Shared L3/LLC stats:\n";
    l3_stats_->print_stats(prefix + "  ");
}

void
three_level_cache_t::reset_stats()
{
    // Reset per-core L1D and L2 statistics.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        l1d_stats_[i]->reset();
        l2_stats_[i]->reset();
    }
    // Reset shared L3 statistics.
    l3_stats_->reset();
}

} // namespace drmemtrace
} // namespace dynamorio
