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
#include "cpu/cpu_simulator.h"

// Provide standard output formatting.
#include <iomanip>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

cpu_simulator_t::cpu_simulator_t(const cpu_simulator_knobs_t &knobs)
    // Initialize base simulator with core count.
    : simulator_t(knobs.num_cores, /*skip_refs=*/0, /*warmup_refs=*/0,
                  /*warmup_fraction=*/0.0, /*sim_refs=*/UINT64_MAX,
                  /*cpu_scheduling=*/false, /*use_physical=*/false, /*verbose=*/0)
    , knobs_(knobs)
    // Allocate the shared multi-process 4-level radix page table manager.
    , address_space_mgr_(new mmu_address_space_manager_t())
{
    // 1. Initialize the 3-Level Data Cache hierarchy (L1D -> L2 -> Shared L3/LLC).
    three_level_cache_knobs_t c_knobs;
    c_knobs.num_cores = knobs_.num_cores;
    c_knobs.line_size = knobs_.line_size;
    c_knobs.l1d_size = knobs_.l1d_size;
    c_knobs.l1d_assoc = knobs_.l1d_assoc;
    c_knobs.l2_size = knobs_.l2_size;
    c_knobs.l2_assoc = knobs_.l2_assoc;
    c_knobs.l3_size = knobs_.l3_size;
    c_knobs.l3_assoc = knobs_.l3_assoc;
    cache_hierarchy_.reset(new three_level_cache_t(c_knobs));

    // Resize per-core vectors.
    walkers_.resize(knobs_.num_cores);
    itlbs_.resize(knobs_.num_cores);
    dtlbs_.resize(knobs_.num_cores);
    l2_tlbs_.resize(knobs_.num_cores);
    cores_.resize(knobs_.num_cores);

    // 2. Instantiate and connect TLBs, Walkers, and Out-of-Order CPU Core pipelines per core.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        // Construct the hardware Page Table Walker for core i.
        // The walker's uncached PTE memory reads are directly coupled to core i's private L1D cache!
        walkers_[i].reset(new page_table_walker_t(
            static_cast<int>(i), address_space_mgr_.get(), knobs_.pwc_entries,
            knobs_.pwc_assoc, cache_hierarchy_->get_l1d_cache(i)));

        // Construct private L1I, L1D, and unified L2 TLBs.
        itlbs_[i].reset(new tlb_t());
        dtlbs_[i].reset(new tlb_t());
        l2_tlbs_[i].reset(new tlb_t());

        // Initialize L2 TLB and attach the hardware Page Table Walker.
        l2_tlbs_[i]->init(4, static_cast<int>(knobs_.page_size), 512, nullptr,
                          new tlb_stats_t(static_cast<int>(knobs_.page_size)));
        l2_tlbs_[i]->set_page_table_walker(walkers_[i].get());

        // Initialize L1 ITLB and L1 DTLB, parenting them to L2 TLB.
        itlbs_[i]->init(32, static_cast<int>(knobs_.page_size), 32, l2_tlbs_[i].get(),
                        new tlb_stats_t(static_cast<int>(knobs_.page_size)));
        dtlbs_[i]->init(32, static_cast<int>(knobs_.page_size), 32, l2_tlbs_[i].get(),
                        new tlb_stats_t(static_cast<int>(knobs_.page_size)));

        // Configure Out-of-Order CPU core pipeline parameters.
        cpu_core_knobs_t core_knobs;
        core_knobs.core_id = i;
        core_knobs.fetch_width = knobs_.fetch_width;
        core_knobs.decode_width = knobs_.decode_width;
        core_knobs.dispatch_width = knobs_.dispatch_width;
        core_knobs.commit_width = knobs_.commit_width;
        core_knobs.rob_capacity = knobs_.rob_capacity;
        core_knobs.lq_capacity = knobs_.lq_capacity;
        core_knobs.sq_capacity = knobs_.sq_capacity;
        core_knobs.btb_entries = knobs_.btb_entries;
        core_knobs.branch_mispredict_penalty = knobs_.branch_mispredict_penalty;
        core_knobs.alu_latency = knobs_.alu_latency;
        core_knobs.enable_wrong_path = knobs_.enable_wrong_path;
        core_knobs.max_speculative_depth = knobs_.max_speculative_depth;

        // Construct the core pipeline coupling Front-End, ROB, LSQ, TLBs, and 3-level cache!
        cores_[i].reset(new cpu_core_t(core_knobs, cache_hierarchy_.get(),
                                       itlbs_[i].get(), dtlbs_[i].get(),
                                       l2_tlbs_[i].get(), walkers_[i].get()));
    }
}

cpu_simulator_t::~cpu_simulator_t()
{
    // Clean up TLB statistics objects.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        if (itlbs_[i] != nullptr && itlbs_[i]->get_stats() != nullptr) {
            delete itlbs_[i]->get_stats();
        }
        if (dtlbs_[i] != nullptr && dtlbs_[i]->get_stats() != nullptr) {
            delete dtlbs_[i]->get_stats();
        }
        if (l2_tlbs_[i] != nullptr && l2_tlbs_[i]->get_stats() != nullptr) {
            delete l2_tlbs_[i]->get_stats();
        }
    }
}

bool
cpu_simulator_t::process_memref(const memref_t &memref)
{
    // Identify target CPU core index based on thread ID.
    int core_index = core_for_thread(memref.data.tid);
    if (core_index < 0 || static_cast<unsigned int>(core_index) >= knobs_.num_cores) {
        core_index = 0;
    }

    // Ingest trace record into the core's out-of-order execution pipeline.
    return cores_[core_index]->process_memref(memref);
}

bool
cpu_simulator_t::print_results()
{
    // Print comprehensive simulation results.
    std::cerr << "===================================================================\n";
    std::cerr << "       OUT-OF-ORDER CPU, MMU & 3-LEVEL CACHE SIMULATION RESULTS     \n";
    std::cerr << "===================================================================\n";

    // 1. Print per-core CPU pipeline statistics (Fetch, BTB, ROB, LSQ, Cycles, IPC).
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        // Drain any remaining in-flight instructions from the pipeline.
        cores_[i]->flush_and_drain_pipeline();
        cores_[i]->print_stats(std::cerr, "  ");
    }

    // 2. Print 3-level data cache hierarchy statistics (L1D, L2, L3 hits & misses).
    std::cerr << "-------------------------------------------------------------------\n";
    std::cerr << "  3-Level Data Cache Hierarchy (Application + PTW accesses):\n";
    std::cerr << "-------------------------------------------------------------------\n";
    cache_hierarchy_->print_stats("  ");

    // 3. Print MMU / Page Table Walker statistics.
    std::cerr << "-------------------------------------------------------------------\n";
    std::cerr << "  MMU, Page Table Walk & Page Walk Cache (PWC) Statistics:\n";
    std::cerr << "-------------------------------------------------------------------\n";
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        std::cerr << "  Core #" << i << " PTW stats:\n";
        walkers_[i]->get_stats().print_stats("    ");
    }

    return true;
}

cpu_core_t *
cpu_simulator_t::get_core(unsigned int core_index) const
{
    // Validate core index.
    if (core_index < cores_.size()) {
        return cores_[core_index].get();
    }
    return nullptr;
}

three_level_cache_t *
cpu_simulator_t::get_cache_hierarchy() const
{
    return cache_hierarchy_.get();
}

} // namespace drmemtrace
} // namespace dynamorio
