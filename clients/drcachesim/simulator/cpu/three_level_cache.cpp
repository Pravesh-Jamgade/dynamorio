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
    // Configure LLC capacity in blocks (total bytes / line size bytes).
    l3_stats_->set_device_capacity(knobs_.l3_size / knobs_.line_size);
    // Enable power-of-2 reuse distance histogram tracking on Shared LLC.
    l3_stats_->enable_reuse_distance_histogram(true);
    // Instantiate shared L3 / LLC caching device.
    l3_cache_.reset(new cache_lru_t("Shared_LLC"));
    // Initialize L3 with configured associativity, line size, total capacity, and stats.
    l3_cache_->init(knobs_.l3_assoc, static_cast<int>(knobs_.line_size),
                    static_cast<int>(knobs_.l3_size), /*parent=*/nullptr,
                    l3_stats_.get());

    // Allocate shared L3 MSHR and queues.
    l3_mshr_.reset(new mshr_table_t(knobs_.l3_mshr_capacity, knobs_.line_size));
    l3_rq_.reset(new read_queue_t(knobs_.l3_rq_capacity, knobs_.line_size));
    l3_wq_.reset(new write_queue_t(knobs_.l3_wq_capacity, knobs_.line_size));
    l3_pq_.reset(new prefetch_queue_t(knobs_.l3_pq_capacity, knobs_.line_size));

    // Allocate per-core vectors for L1I, L1D, and L2 caches and statistics.
    l1i_caches_.resize(knobs_.num_cores);
    l1i_stats_.resize(knobs_.num_cores);
    l1d_caches_.resize(knobs_.num_cores);
    l1d_stats_.resize(knobs_.num_cores);
    l2_caches_.resize(knobs_.num_cores);
    l2_stats_.resize(knobs_.num_cores);

    // Allocate per-core vectors for MSHRs and queues.
    l1i_mshrs_.resize(knobs_.num_cores);
    l1i_rqs_.resize(knobs_.num_cores);
    l1i_pqs_.resize(knobs_.num_cores);
    l1d_mshrs_.resize(knobs_.num_cores);
    l1d_rqs_.resize(knobs_.num_cores);
    l1d_wqs_.resize(knobs_.num_cores);
    l1d_pqs_.resize(knobs_.num_cores);
    l2_mshrs_.resize(knobs_.num_cores);
    l2_rqs_.resize(knobs_.num_cores);
    l2_wqs_.resize(knobs_.num_cores);
    l2_pqs_.resize(knobs_.num_cores);

    // 2. Instantiate and connect private L1I, L1D, and L2 caches for each simulated core.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        // Build descriptive names for L2, L1D, and L1I caches on core i.
        std::string l2_name = "Core_" + std::to_string(i) + "_L2";
        std::string l1d_name = "Core_" + std::to_string(i) + "_L1D";
        std::string l1i_name = "Core_" + std::to_string(i) + "_L1I";

        // Allocate L2 cache and stats for core i.
        l2_stats_[i].reset(new cache_stats_t(static_cast<int>(knobs_.line_size)));
        // Set L2 capacity in blocks for capacity miss classification.
        l2_stats_[i]->set_device_capacity(knobs_.l2_size / knobs_.line_size);
        l2_caches_[i].reset(new cache_lru_t(l2_name));
        // L2's parent is the shared L3 / Last-Level Cache.
        l2_caches_[i]->init(knobs_.l2_assoc, static_cast<int>(knobs_.line_size),
                            static_cast<int>(knobs_.l2_size), l3_cache_.get(),
                            l2_stats_[i].get());

        // Allocate L2 MSHR and queues.
        l2_mshrs_[i].reset(new mshr_table_t(knobs_.l2_mshr_capacity, knobs_.line_size));
        l2_rqs_[i].reset(new read_queue_t(knobs_.l2_rq_capacity, knobs_.line_size));
        l2_wqs_[i].reset(new write_queue_t(knobs_.l2_wq_capacity, knobs_.line_size));
        l2_pqs_[i].reset(new prefetch_queue_t(knobs_.l2_pq_capacity, knobs_.line_size));

        // Allocate L1D cache and stats for core i.
        l1d_stats_[i].reset(new cache_stats_t(static_cast<int>(knobs_.line_size)));
        // Set L1D capacity in blocks for capacity miss classification.
        l1d_stats_[i]->set_device_capacity(knobs_.l1d_size / knobs_.line_size);
        l1d_caches_[i].reset(new cache_lru_t(l1d_name));
        // L1D's parent is core i's private L2 cache.
        l1d_caches_[i]->init(knobs_.l1d_assoc, static_cast<int>(knobs_.line_size),
                             static_cast<int>(knobs_.l1d_size), l2_caches_[i].get(),
                             l1d_stats_[i].get());

        // Allocate L1D MSHR and queues.
        l1d_mshrs_[i].reset(new mshr_table_t(knobs_.l1d_mshr_capacity, knobs_.line_size));
        l1d_rqs_[i].reset(new read_queue_t(knobs_.l1d_rq_capacity, knobs_.line_size));
        l1d_wqs_[i].reset(new write_queue_t(knobs_.l1d_wq_capacity, knobs_.line_size));
        l1d_pqs_[i].reset(new prefetch_queue_t(knobs_.l1d_pq_capacity, knobs_.line_size));

        // Allocate L1I cache and stats for core i.
        l1i_stats_[i].reset(new cache_stats_t(static_cast<int>(knobs_.line_size)));
        // Set L1I capacity in blocks for capacity miss classification.
        l1i_stats_[i]->set_device_capacity(knobs_.l1i_size / knobs_.line_size);
        l1i_caches_[i].reset(new cache_lru_t(l1i_name));
        // L1I's parent is also core i's private unified L2 cache!
        l1i_caches_[i]->init(knobs_.l1i_assoc, static_cast<int>(knobs_.line_size),
                             static_cast<int>(knobs_.l1i_size), l2_caches_[i].get(),
                             l1i_stats_[i].get());

        // Allocate L1I MSHR and queues.
        l1i_mshrs_[i].reset(new mshr_table_t(knobs_.l1i_mshr_capacity, knobs_.line_size));
        l1i_rqs_[i].reset(new read_queue_t(knobs_.l1i_rq_capacity, knobs_.line_size));
        l1i_pqs_[i].reset(new prefetch_queue_t(knobs_.l1i_pq_capacity, knobs_.line_size));
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

    // If the memory reference is an instruction fetch, route it to the L1I hierarchy!
    if (type_is_instr(memref.data.type)) {
        return request_instruction(core, memref);
    }

    // Check for RAW hazard against pending stores in L1D write queue.
    if (!type_is_prefetch(memref.data.type) && memref.data.type != TRACE_TYPE_WRITE) {
        if (l1d_wqs_[core]->has_raw_hazard(memref.data.addr)) {
            // Forward from write queue in 1 cycle!
            res.hit_l1d = true;
            res.level = cache_hit_level_t::HIT_L1D;
            res.latency_cycles = 1;
            return res;
        }
    }

    // Enqueue into L1D queues.
    cache_queue_entry_t qentry;
    qentry.addr = memref.data.addr;
    qentry.size = memref.data.size;
    qentry.op_type = (memref.data.type == TRACE_TYPE_WRITE) ? cpu_op_type_t::OP_STORE : cpu_op_type_t::OP_LOAD;
    qentry.ref_id = memref.data.pc;
    if (memref.data.type == TRACE_TYPE_WRITE) {
        l1d_wqs_[core]->enqueue(qentry, 0);
    } else {
        l1d_rqs_[core]->enqueue(qentry, 0);
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

    // On L1D miss for demand reads, evaluate MSHR allocation or secondary coalescing!
    if (!res.hit_l1d && memref.data.type != TRACE_TYPE_WRITE) {
        mshr_entry_t *existing_mshr = l1d_mshrs_[core]->lookup(memref.data.addr);
        if (existing_mshr != nullptr) {
            // Secondary miss: coalesce onto in-flight line fill.
            l1d_mshrs_[core]->coalesce_secondary(existing_mshr, memref.data.pc);
            res.latency_cycles = 1; // Absorbed into in-flight line fill.
        } else {
            uint64_t stall = 0;
            if (l1d_mshrs_[core]->is_full()) {
                stall = l1d_mshrs_[core]->get_cycles_until_next_available(0);
            }
            l1d_mshrs_[core]->allocate(memref.data.addr, 0, res.latency_cycles, qentry.op_type, memref.data.pc);
            res.latency_cycles += stall;
        }
    }

    // Return the completed access outcome.
    return res;
}

cache_access_result_t
three_level_cache_t::request_instruction(unsigned int core, const memref_t &memref)
{
    // Initialize result structure.
    cache_access_result_t res;

    // Check if core index is valid.
    if (core >= knobs_.num_cores) {
        return res;
    }

    // Enqueue fetch into L1I read queue.
    cache_queue_entry_t qentry;
    qentry.addr = memref.instr.addr;
    qentry.size = memref.instr.size;
    qentry.op_type = cpu_op_type_t::OP_FETCH;
    qentry.ref_id = memref.instr.addr;
    l1i_rqs_[core]->enqueue(qentry, 0);

    // Capture hit counts across L1I, L2, and L3 prior to processing the instruction fetch.
    int64_t pre_l1i_hits = l1i_stats_[core]->get_metric(metric_name_t::HITS);
    int64_t pre_l2_hits = l2_stats_[core]->get_metric(metric_name_t::HITS);
    int64_t pre_l3_hits = l3_stats_->get_metric(metric_name_t::HITS);

    // Issue the instruction fetch to core's private L1I cache.
    // If L1I misses, caching_device_t automatically cascades the request to L2, and L2 to L3!
    l1i_caches_[core]->request(memref);

    // Capture post-access hit counts to determine which level serviced the instruction fetch.
    int64_t post_l1i_hits = l1i_stats_[core]->get_metric(metric_name_t::HITS);
    int64_t post_l2_hits = l2_stats_[core]->get_metric(metric_name_t::HITS);
    int64_t post_l3_hits = l3_stats_->get_metric(metric_name_t::HITS);

    // Check if the instruction fetch was satisfied by L1I.
    if (post_l1i_hits > pre_l1i_hits) {
        res.hit_l1i = true;
        res.level = cache_hit_level_t::HIT_L1I;
        res.latency_cycles = knobs_.l1i_latency;
    }
    // Check if the instruction fetch missed in L1I but was satisfied by private L2.
    else if (post_l2_hits > pre_l2_hits) {
        res.hit_l2 = true;
        res.level = cache_hit_level_t::HIT_L2;
        res.latency_cycles = knobs_.l1i_latency + knobs_.l2_latency;
    }
    // Check if the instruction fetch missed in L1I and L2 but was satisfied by shared L3 / LLC.
    else if (post_l3_hits > pre_l3_hits) {
        res.hit_l3 = true;
        res.level = cache_hit_level_t::HIT_L3;
        res.latency_cycles = knobs_.l1i_latency + knobs_.l2_latency + knobs_.l3_latency;
    }
    // Instruction fetch missed in all cache levels; serviced by main memory DRAM.
    else {
        res.level = cache_hit_level_t::HIT_DRAM;
        res.latency_cycles =
            knobs_.l1i_latency + knobs_.l2_latency + knobs_.l3_latency + knobs_.dram_latency;
    }

    // On L1I miss, evaluate MSHR allocation or secondary coalescing!
    if (!res.hit_l1i) {
        mshr_entry_t *existing_mshr = l1i_mshrs_[core]->lookup(memref.instr.addr);
        if (existing_mshr != nullptr) {
            // Secondary miss: coalesce onto in-flight line fill.
            l1i_mshrs_[core]->coalesce_secondary(existing_mshr, memref.instr.addr);
            res.latency_cycles = 1;
        } else {
            uint64_t stall = 0;
            if (l1i_mshrs_[core]->is_full()) {
                stall = l1i_mshrs_[core]->get_cycles_until_next_available(0);
            }
            l1i_mshrs_[core]->allocate(memref.instr.addr, 0, res.latency_cycles, cpu_op_type_t::OP_FETCH, memref.instr.addr);
            res.latency_cycles += stall;
        }
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
three_level_cache_t::get_l1i_cache(unsigned int core) const
{
    // Validate core index.
    if (core < l1i_caches_.size()) {
        return l1i_caches_[core].get(); // Return private L1I cache device.
    }
    return nullptr;
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

mshr_table_t *
three_level_cache_t::get_l1i_mshr(unsigned int core) const
{
    if (core < l1i_mshrs_.size()) {
        return l1i_mshrs_[core].get();
    }
    return nullptr;
}

mshr_table_t *
three_level_cache_t::get_l1d_mshr(unsigned int core) const
{
    if (core < l1d_mshrs_.size()) {
        return l1d_mshrs_[core].get();
    }
    return nullptr;
}

mshr_table_t *
three_level_cache_t::get_l2_mshr(unsigned int core) const
{
    if (core < l2_mshrs_.size()) {
        return l2_mshrs_[core].get();
    }
    return nullptr;
}

mshr_table_t *
three_level_cache_t::get_l3_mshr() const
{
    return l3_mshr_.get();
}

read_queue_t *
three_level_cache_t::get_l1i_rq(unsigned int core) const
{
    if (core < l1i_rqs_.size()) {
        return l1i_rqs_[core].get();
    }
    return nullptr;
}

read_queue_t *
three_level_cache_t::get_l1d_rq(unsigned int core) const
{
    if (core < l1d_rqs_.size()) {
        return l1d_rqs_[core].get();
    }
    return nullptr;
}

write_queue_t *
three_level_cache_t::get_l1d_wq(unsigned int core) const
{
    if (core < l1d_wqs_.size()) {
        return l1d_wqs_[core].get();
    }
    return nullptr;
}

prefetch_queue_t *
three_level_cache_t::get_l1i_pq(unsigned int core) const
{
    if (core < l1i_pqs_.size()) {
        return l1i_pqs_[core].get();
    }
    return nullptr;
}

prefetch_queue_t *
three_level_cache_t::get_l1d_pq(unsigned int core) const
{
    if (core < l1d_pqs_.size()) {
        return l1d_pqs_[core].get();
    }
    return nullptr;
}

read_queue_t *
three_level_cache_t::get_l2_rq(unsigned int core) const
{
    if (core < l2_rqs_.size()) {
        return l2_rqs_[core].get();
    }
    return nullptr;
}

write_queue_t *
three_level_cache_t::get_l2_wq(unsigned int core) const
{
    if (core < l2_wqs_.size()) {
        return l2_wqs_[core].get();
    }
    return nullptr;
}

prefetch_queue_t *
three_level_cache_t::get_l2_pq(unsigned int core) const
{
    if (core < l2_pqs_.size()) {
        return l2_pqs_[core].get();
    }
    return nullptr;
}

read_queue_t *
three_level_cache_t::get_l3_rq() const
{
    return l3_rq_.get();
}

write_queue_t *
three_level_cache_t::get_l3_wq() const
{
    return l3_wq_.get();
}

prefetch_queue_t *
three_level_cache_t::get_l3_pq() const
{
    return l3_pq_.get();
}

void
three_level_cache_t::tick_mshrs(uint64_t current_cycle)
{
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        l1i_mshrs_[i]->retire_completed_fills(current_cycle);
        l1d_mshrs_[i]->retire_completed_fills(current_cycle);
        l2_mshrs_[i]->retire_completed_fills(current_cycle);
    }
    l3_mshr_->retire_completed_fills(current_cycle);
}

void
three_level_cache_t::drain_all_mshrs_and_queues(uint64_t &current_cycle)
{
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        l1i_mshrs_[i]->drain_all(current_cycle);
        l1d_mshrs_[i]->drain_all(current_cycle);
        l2_mshrs_[i]->drain_all(current_cycle);
    }
    l3_mshr_->drain_all(current_cycle);
}

void
three_level_cache_t::print_stats(const std::string &prefix) const
{
    // Print statistics for private L1I, L1D, and L2 caches for each core.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        std::cerr << prefix << "Core #" << i << " 3-Level Cache stats:\n";
        std::cerr << prefix << "  L1I stats:\n";
        l1i_stats_[i]->print_stats(prefix + "    ");
        std::cerr << prefix << "    L1I MSHR stats: allocs=" << l1i_mshrs_[i]->get_stats().total_allocations
                  << ", secondary_merges=" << l1i_mshrs_[i]->get_stats().secondary_merges
                  << ", full_stalls=" << l1i_mshrs_[i]->get_stats().full_stalls
                  << ", peak_occ=" << l1i_mshrs_[i]->get_stats().peak_occupancy << "\n";
        std::cerr << prefix << "    L1I Queue stats: RQ_enqueued=" << l1i_rqs_[i]->total_enqueued()
                  << ", RQ_peak=" << l1i_rqs_[i]->peak_occupancy() << "\n";

        std::cerr << prefix << "  L1D stats:\n";
        l1d_stats_[i]->print_stats(prefix + "    ");
        std::cerr << prefix << "    L1D MSHR stats: allocs=" << l1d_mshrs_[i]->get_stats().total_allocations
                  << ", secondary_merges=" << l1d_mshrs_[i]->get_stats().secondary_merges
                  << ", full_stalls=" << l1d_mshrs_[i]->get_stats().full_stalls
                  << ", peak_occ=" << l1d_mshrs_[i]->get_stats().peak_occupancy << "\n";
        std::cerr << prefix << "    L1D Queue stats: RQ_enqueued=" << l1d_rqs_[i]->total_enqueued()
                  << ", WQ_enqueued=" << l1d_wqs_[i]->total_enqueued()
                  << ", WQ_coalesced=" << l1d_wqs_[i]->coalesced_writes()
                  << ", WQ_peak=" << l1d_wqs_[i]->peak_occupancy() << "\n";

        std::cerr << prefix << "  L2 stats:\n";
        l2_stats_[i]->print_stats(prefix + "    ");
        std::cerr << prefix << "    L2 MSHR stats: allocs=" << l2_mshrs_[i]->get_stats().total_allocations
                  << ", secondary_merges=" << l2_mshrs_[i]->get_stats().secondary_merges
                  << ", full_stalls=" << l2_mshrs_[i]->get_stats().full_stalls
                  << ", peak_occ=" << l2_mshrs_[i]->get_stats().peak_occupancy << "\n";
    }
    // Print statistics for the shared L3 / Last-Level Cache.
    std::cerr << prefix << "Shared L3/LLC stats:\n";
    l3_stats_->print_stats(prefix + "  ");
    std::cerr << prefix << "  L3 MSHR stats: allocs=" << l3_mshr_->get_stats().total_allocations
              << ", secondary_merges=" << l3_mshr_->get_stats().secondary_merges
              << ", full_stalls=" << l3_mshr_->get_stats().full_stalls
              << ", peak_occ=" << l3_mshr_->get_stats().peak_occupancy << "\n";
}

void
three_level_cache_t::reset_stats()
{
    // Reset per-core L1I, L1D, and L2 statistics.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        l1i_stats_[i]->reset();
        l1d_stats_[i]->reset();
        l2_stats_[i]->reset();
    }
    // Reset shared L3 statistics.
    l3_stats_->reset();
}

} // namespace drmemtrace
} // namespace dynamorio
