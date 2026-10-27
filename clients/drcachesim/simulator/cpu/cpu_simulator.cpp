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

// Provide standard file stream support for stats.log.
#include <fstream>
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
    // 1. Initialize the 3-Level Cache hierarchy (split L1I/L1D -> L2 -> Shared L3/LLC).
    three_level_cache_knobs_t c_knobs;
    c_knobs.num_cores = knobs_.num_cores;
    c_knobs.line_size = knobs_.line_size;
    c_knobs.l1i_size = knobs_.l1i_size;
    c_knobs.l1i_assoc = knobs_.l1i_assoc;
    c_knobs.l1i_latency = knobs_.l1i_latency;
    c_knobs.l1d_size = knobs_.l1d_size;
    c_knobs.l1d_assoc = knobs_.l1d_assoc;
    c_knobs.l2_size = knobs_.l2_size;
    c_knobs.l2_assoc = knobs_.l2_assoc;
    c_knobs.l3_size = knobs_.l3_size;
    c_knobs.l3_assoc = knobs_.l3_assoc;
    c_knobs.l1i_mshr_capacity = knobs_.l1i_mshr_capacity;
    c_knobs.l1i_rq_capacity = knobs_.l1i_rq_capacity;
    c_knobs.l1i_pq_capacity = knobs_.l1i_pq_capacity;
    c_knobs.l1d_mshr_capacity = knobs_.l1d_mshr_capacity;
    c_knobs.l1d_rq_capacity = knobs_.l1d_rq_capacity;
    c_knobs.l1d_wq_capacity = knobs_.l1d_wq_capacity;
    c_knobs.l1d_pq_capacity = knobs_.l1d_pq_capacity;
    c_knobs.l2_mshr_capacity = knobs_.l2_mshr_capacity;
    c_knobs.l2_rq_capacity = knobs_.l2_rq_capacity;
    c_knobs.l2_wq_capacity = knobs_.l2_wq_capacity;
    c_knobs.l2_pq_capacity = knobs_.l2_pq_capacity;
    c_knobs.l3_mshr_capacity = knobs_.l3_mshr_capacity;
    c_knobs.l3_rq_capacity = knobs_.l3_rq_capacity;
    c_knobs.l3_wq_capacity = knobs_.l3_wq_capacity;
    c_knobs.l3_pq_capacity = knobs_.l3_pq_capacity;
    c_knobs.dram_latency = knobs_.dram_latency;
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

        // Compute L2 TLB sector geometry:
        // Total PTE entries remains constant between baseline and sector modes.
        // Baseline (sector=1): 256 sets x 6 ways = 1536 entries.
        // Sector=2: 128 sets x 6 ways (768 blocks, 2 PTEs per block = 1536 entries).
        // Sector=4: 64 sets x 6 ways (384 blocks, 4 PTEs per block = 1536 entries).
        // Sector=8: 32 sets x 6 ways (192 blocks, 8 PTEs per block = 1536 entries).
        unsigned int sector = (knobs_.l2_tlb_sector_size > 0) ? knobs_.l2_tlb_sector_size : 1; // Effective sector size.
        unsigned int l2_assoc = (knobs_.l2_tlb_assoc > 0) ? knobs_.l2_tlb_assoc : 6;           // L2 TLB associativity.
        unsigned int l2_total_entries = (knobs_.l2_tlb_entries > 0) ? knobs_.l2_tlb_entries : 1536; // Total entries.
        unsigned int l2_num_blocks = l2_total_entries / sector;                                 // Number of sector blocks.
        unsigned int l2_block_size = static_cast<unsigned int>(knobs_.page_size * sector);      // Block size in bytes.

        // Configure walker sector size so leaf walks fetch full sector chunks into cache.
        walkers_[i]->set_sector_size(sector);
        // Configure hardware walker latencies (PWC=4, L1D=4, L2=14, L3=34, DRAM).
        walkers_[i]->set_latencies(4, 4, 14, 34, knobs_.dram_latency);

        // Configure L2 TLB sector size.
        l2_tlbs_[i]->set_sector_size(sector);
        // Configure unified L2 TLB hit latency (9 cycles).
        l2_tlbs_[i]->set_hit_latency(9);
        // Initialize L2 TLB and attach the hardware Page Table Walker.
        l2_tlbs_[i]->init(l2_assoc, static_cast<int>(l2_block_size), l2_num_blocks, nullptr,
                          new tlb_stats_t(static_cast<int>(knobs_.page_size)));
        // Set L2 TLB capacity in entries for capacity miss classification.
        l2_tlbs_[i]->get_stats()->set_device_capacity(l2_total_entries);
        l2_tlbs_[i]->set_page_table_walker(walkers_[i].get());

        // Initialize L1 ITLB and L1 DTLB, parenting them to L2 TLB.
        itlbs_[i]->set_hit_latency(1); // 1-cycle L1 ITLB hit latency.
        itlbs_[i]->init(32, static_cast<int>(knobs_.page_size), 32, l2_tlbs_[i].get(),
                        new tlb_stats_t(static_cast<int>(knobs_.page_size)));
        // Set L1 ITLB capacity in entries.
        itlbs_[i]->get_stats()->set_device_capacity(32);
        dtlbs_[i]->set_hit_latency(1); // 1-cycle L1 DTLB hit latency.
        dtlbs_[i]->init(32, static_cast<int>(knobs_.page_size), 32, l2_tlbs_[i].get(),
                        new tlb_stats_t(static_cast<int>(knobs_.page_size)));
        // Set L1 DTLB capacity in entries.
        dtlbs_[i]->get_stats()->set_device_capacity(32);

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
    // Check if user specified a maximum reference limit and threshold reached.
    if (knobs_.sim_refs > 0 && total_simulated_refs_ >= knobs_.sim_refs) {
        return true; // Ignore subsequent references once reference limit reached.
    }
    ++total_simulated_refs_; // Increment total memory reference count.

    // Check if this memory reference represents an instruction fetch.
    if (type_is_instr(memref.instr.type)) {
        ++total_simulated_instrs_; // Increment total simulated instruction count.
        // Check if heartbeat interval is configured and threshold has been reached.
        if (knobs_.heartbeat_interval > 0 &&
            (total_simulated_instrs_ - last_heartbeat_instrs_ >= knobs_.heartbeat_interval)) {
            last_heartbeat_instrs_ = total_simulated_instrs_; // Record last heartbeat marker.
            print_heartbeat(total_simulated_instrs_);         // Print progress heartbeat to std::cerr.
        }
    }

    // Identify target CPU core index based on thread ID.
    int core_index = core_for_thread(memref.data.tid);
    if (core_index < 0 || static_cast<unsigned int>(core_index) >= knobs_.num_cores) {
        core_index = 0;
    }

    // Ingest trace record into the core's out-of-order execution pipeline.
    return cores_[core_index]->process_memref(memref);
}

void
cpu_simulator_t::print_heartbeat(uint64_t instrs) const
{
    // Accumulate total execution cycles and retired instructions across all cores.
    uint64_t total_cycles = 0;   // Aggregated processor clock cycles.
    uint64_t total_retired = 0;  // Aggregated committed instruction count.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        if (cores_[i] != nullptr) {
            total_cycles += cores_[i]->get_current_cycle();          // Add core cycle count.
            total_retired += cores_[i]->total_instructions_retired(); // Add core retired count.
        }
    }
    // Calculate current cumulative instructions per cycle (IPC).
    double cur_ipc = (total_cycles > 0)
        ? (static_cast<double>(total_retired) / static_cast<double>(total_cycles))
        : 0.0; // Avoid divide-by-zero if no cycles executed.
    // Format and emit the progress heartbeat message to std::cerr.
    std::cerr << "[HEARTBEAT] Simulated " << instrs << " instructions (Retired: " << total_retired
              << ", Cycles: " << total_cycles << ", IPC: " << std::fixed << std::setprecision(4)
              << cur_ipc << ")" << std::endl;
}

void
cpu_simulator_t::print_csv_stats(std::ostream &os) const
{
    // Drain remaining in-flight instructions from CPU cores.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        cores_[i]->flush_and_drain_pipeline(); // Ensure pipeline is fully drained.
    }

    // Gather aggregated scalar values.
    uint64_t total_cycles = 0;              // Aggregate processor cycles.
    int64_t total_retired = 0;             // Aggregate instructions retired.
    int64_t total_fetched = 0;             // Aggregate instructions fetched.
    int64_t total_loads = 0;               // Aggregate load instructions.
    int64_t total_stores = 0;              // Aggregate store instructions.
    int64_t total_branch_squash_cycles = 0; // Aggregate branch squash cycles.
    int64_t total_icache_miss_cycles = 0;   // Aggregate L1I miss stall cycles.
    int64_t total_itlb_miss_cycles = 0;     // Aggregate L1 ITLB miss stall cycles.

    int64_t bp_branches = 0;               // Aggregate branch count.
    int64_t bp_mispredicted = 0;           // Aggregate branch mispredictions.

    int64_t rob_dispatches = 0;            // Aggregate ROB dispatches.
    int64_t rob_commits = 0;               // Aggregate ROB commits.
    int64_t rob_squashes = 0;              // Aggregate ROB squashes.
    int64_t rob_head_stalls = 0;           // Aggregate ROB head stalls.

    int64_t lsq_loads = 0;                 // Aggregate LSQ loads.
    int64_t lsq_stores = 0;                // Aggregate LSQ stores.
    int64_t lsq_forwardings = 0;           // Aggregate store-to-load forwardings.

    int64_t wp_branches = 0;               // Speculative branches handled.
    int64_t wp_instrs_fetched = 0;         // Speculative instructions fetched.
    int64_t wp_instrs_squashed = 0;        // Speculative instructions squashed.
    int64_t wp_loads = 0;                  // Speculative loads executed.
    int64_t wp_stores = 0;                 // Speculative stores buffered.

    // Sum metrics across all CPU cores.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        if (cores_[i] != nullptr) {
            total_cycles += cores_[i]->get_current_cycle();                                         // Cycles.
            total_retired += cores_[i]->total_instructions_retired();                              // Commits.
            total_fetched += cores_[i]->total_instructions_fetched();                              // Fetches.
            total_loads += cores_[i]->total_loads();                                               // Loads.
            total_stores += cores_[i]->total_stores();                                             // Stores.
            total_branch_squash_cycles += cores_[i]->branch_squash_cycles();                        // Squash cycles.
            total_icache_miss_cycles += cores_[i]->icache_miss_stall_cycles();                      // Icache stalls.
            total_itlb_miss_cycles += cores_[i]->itlb_miss_stall_cycles();                          // ITLB stalls.

            bp_branches += cores_[i]->get_branch_predictor().total_branches();                    // Branches.
            bp_mispredicted += cores_[i]->get_branch_predictor().mispredicted_branches();          // Mispredicts.

            rob_dispatches += cores_[i]->get_rob().total_dispatches();                             // ROB dispatches.
            rob_commits += cores_[i]->get_rob().total_commits();                                   // ROB commits.
            rob_squashes += cores_[i]->get_rob().total_squashes();                                 // ROB squashes.
            rob_head_stalls += cores_[i]->get_rob().head_of_line_stalls();                         // ROB stalls.

            lsq_loads += cores_[i]->get_lsq().total_loads_dispatched();                           // LSQ loads.
            lsq_stores += cores_[i]->get_lsq().total_stores_dispatched();                          // LSQ stores.
            lsq_forwardings += cores_[i]->get_lsq().store_forwardings();                           // Forwardings.

            wp_branches += cores_[i]->get_wrong_path_engine().get_stats().mispredictions_handled;       // WP branches.
            wp_instrs_fetched += cores_[i]->get_wrong_path_engine().get_stats().instructions_fetched; // WP fetched.
            wp_instrs_squashed += cores_[i]->get_wrong_path_engine().get_stats().instructions_squashed; // WP squashed.
            wp_loads += cores_[i]->get_wrong_path_engine().get_stats().loads_executed;                 // WP loads.
            wp_stores += cores_[i]->get_wrong_path_engine().get_stats().stores_buffered;               // WP stores.
        }
    }

    // Compute derived IPC, CPI, and branch misprediction rates.
    double overall_ipc = (total_cycles > 0) ? (static_cast<double>(total_retired) / total_cycles) : 0.0; // IPC.
    double overall_cpi = (total_retired > 0) ? (static_cast<double>(total_cycles) / total_retired) : 0.0; // CPI.
    double bp_mispredict_rate = (bp_branches > 0) ? (static_cast<double>(bp_mispredicted) / bp_branches) : 0.0; // Rate.

    // Determine git commit ID string with runtime environment and build macro fallbacks.
    std::string commit_str = knobs_.commit_id;
    if (commit_str.empty() || commit_str == "unknown") {
        // Check runtime environment variable GIT_COMMIT_ID.
        const char *env_git = getenv("GIT_COMMIT_ID");
        if (env_git != nullptr && env_git[0] != '\0') {
            // Use environment commit ID.
            commit_str = env_git;
        }
#ifdef DYNAMORIO_GIT_COMMIT
        else {
            // Use build-time commit definition.
            commit_str = DYNAMORIO_GIT_COMMIT;
        }
#endif
    }

    // 1. Output simulation metadata: config file used and git commit ID.
    os << "config_file, " << (knobs_.config_file.empty() ? "default" : knobs_.config_file) << "\n";
    os << "commit_id, " << (commit_str.empty() ? "unknown" : commit_str) << "\n";

    // 2. Output all CPU pipeline, cache hierarchy, MMU, and simulation configuration values.
    os << "num_cores, " << knobs_.num_cores << "\n";
    os << "fetch_width, " << knobs_.fetch_width << "\n";
    os << "decode_width, " << knobs_.decode_width << "\n";
    os << "dispatch_width, " << knobs_.dispatch_width << "\n";
    os << "commit_width, " << knobs_.commit_width << "\n";
    os << "rob_capacity, " << knobs_.rob_capacity << "\n";
    os << "lq_capacity, " << knobs_.lq_capacity << "\n";
    os << "sq_capacity, " << knobs_.sq_capacity << "\n";
    os << "btb_entries, " << knobs_.btb_entries << "\n";
    os << "branch_mispredict_penalty, " << knobs_.branch_mispredict_penalty << "\n";
    os << "alu_latency, " << knobs_.alu_latency << "\n";
    os << "enable_wrong_path, " << (knobs_.enable_wrong_path ? 1 : 0) << "\n";
    os << "max_speculative_depth, " << knobs_.max_speculative_depth << "\n";
    os << "line_size, " << knobs_.line_size << "\n";
    os << "l1i_size, " << knobs_.l1i_size << "\n";
    os << "l1i_assoc, " << knobs_.l1i_assoc << "\n";
    os << "l1i_latency, " << knobs_.l1i_latency << "\n";
    os << "l1i_mshr_capacity, " << knobs_.l1i_mshr_capacity << "\n";
    os << "l1i_rq_capacity, " << knobs_.l1i_rq_capacity << "\n";
    os << "l1i_pq_capacity, " << knobs_.l1i_pq_capacity << "\n";
    os << "l1d_size, " << knobs_.l1d_size << "\n";
    os << "l1d_assoc, " << knobs_.l1d_assoc << "\n";
    os << "l1d_mshr_capacity, " << knobs_.l1d_mshr_capacity << "\n";
    os << "l1d_rq_capacity, " << knobs_.l1d_rq_capacity << "\n";
    os << "l1d_wq_capacity, " << knobs_.l1d_wq_capacity << "\n";
    os << "l1d_pq_capacity, " << knobs_.l1d_pq_capacity << "\n";
    os << "l2_size, " << knobs_.l2_size << "\n";
    os << "l2_assoc, " << knobs_.l2_assoc << "\n";
    os << "l2_mshr_capacity, " << knobs_.l2_mshr_capacity << "\n";
    os << "l2_rq_capacity, " << knobs_.l2_rq_capacity << "\n";
    os << "l2_wq_capacity, " << knobs_.l2_wq_capacity << "\n";
    os << "l2_pq_capacity, " << knobs_.l2_pq_capacity << "\n";
    os << "l3_size, " << knobs_.l3_size << "\n";
    os << "l3_assoc, " << knobs_.l3_assoc << "\n";
    os << "l3_mshr_capacity, " << knobs_.l3_mshr_capacity << "\n";
    os << "l3_rq_capacity, " << knobs_.l3_rq_capacity << "\n";
    os << "l3_wq_capacity, " << knobs_.l3_wq_capacity << "\n";
    os << "l3_pq_capacity, " << knobs_.l3_pq_capacity << "\n";
    os << "page_size, " << knobs_.page_size << "\n";
    os << "pwc_entries, " << knobs_.pwc_entries << "\n";
    os << "pwc_assoc, " << knobs_.pwc_assoc << "\n";
    os << "l2_tlb_entries, " << knobs_.l2_tlb_entries << "\n";
    os << "l2_tlb_assoc, " << knobs_.l2_tlb_assoc << "\n";
    os << "l2_tlb_sector_size, " << knobs_.l2_tlb_sector_size << "\n";
    os << "dram_latency, " << knobs_.dram_latency << "\n";
    os << "sim_refs, " << knobs_.sim_refs << "\n";
    os << "heartbeat_interval, " << knobs_.heartbeat_interval << "\n";

    // Output scalar metrics in CSV format.
    os << std::fixed << std::setprecision(4);                                       // Set precision.
    os << "ipc, " << overall_ipc << "\n";                                           // Output IPC.
    os << "cpi, " << overall_cpi << "\n";                                           // Output CPI.
    os << "cycles, " << total_cycles << "\n";                                       // Output total cycles.
    os << "instructions_retired, " << total_retired << "\n";                         // Output committed instrs.
    os << "instructions_fetched, " << total_fetched << "\n";                         // Output fetched instrs.
    os << "loads, " << total_loads << "\n";                                         // Output total loads.
    os << "stores, " << total_stores << "\n";                                       // Output total stores.
    os << "branch_squash_cycles, " << total_branch_squash_cycles << "\n";           // Output squash cycles.
    os << "icache_miss_stall_cycles, " << total_icache_miss_cycles << "\n";         // Output icache stalls.
    os << "itlb_miss_stall_cycles, " << total_itlb_miss_cycles << "\n";             // Output ITLB stalls.

    // Output branch predictor metrics.
    os << "BP branches, " << bp_branches << "\n";                                   // Output branches.
    os << "BP mispredicted, " << bp_mispredicted << "\n";                           // Output mispredictions.
    os << "BP misprediction rate, " << bp_mispredict_rate << "\n";                   // Output mispredict rate.

    // Output ROB metrics.
    os << "ROB capacity, " << knobs_.rob_capacity << "\n";                          // Output ROB size.
    os << "ROB dispatches, " << rob_dispatches << "\n";                             // Output ROB dispatches.
    os << "ROB commits, " << rob_commits << "\n";                                   // Output ROB commits.
    os << "ROB squashes, " << rob_squashes << "\n";                                 // Output ROB squashes.
    os << "ROB head stalls, " << rob_head_stalls << "\n";                           // Output ROB head stalls.

    // Output LSQ metrics.
    os << "LSQ loads, " << lsq_loads << "\n";                                       // Output LSQ loads.
    os << "LSQ stores, " << lsq_stores << "\n";                                     // Output LSQ stores.
    os << "LSQ forwardings, " << lsq_forwardings << "\n";                           // Output LSQ forwardings.

    // Output speculative wrong-path metrics.
    os << "WP branches handled, " << wp_branches << "\n";                           // Output WP branches.
    os << "WP instrs fetched, " << wp_instrs_fetched << "\n";                       // Output WP fetched.
    os << "WP instrs squashed, " << wp_instrs_squashed << "\n";                     // Output WP squashed.
    os << "WP loads executed, " << wp_loads << "\n";                               // Output WP loads.
    os << "WP stores buffered, " << wp_stores << "\n";                             // Output WP stores.

    // Output Cache hierarchy metrics.
    if (cache_hierarchy_ != nullptr) {
        caching_device_stats_t *l1i_stats = cache_hierarchy_->get_l1i_stats(0);     // L1I stats.
        caching_device_stats_t *l1d_stats = cache_hierarchy_->get_l1d_stats(0);     // L1D stats.
        caching_device_stats_t *l2_stats = cache_hierarchy_->get_l2_stats(0);       // L2 stats.
        caching_device_stats_t *l3_stats = cache_hierarchy_->get_l3_stats();        // L3/LLC stats.

        // L1I cache metrics.
        if (l1i_stats != nullptr) {
            int64_t hits = l1i_stats->get_metric(metric_name_t::HITS);              // L1I hits.
            int64_t misses = l1i_stats->get_metric(metric_name_t::MISSES);          // L1I misses.
            int64_t comp_misses = l1i_stats->get_metric(metric_name_t::COMPULSORY_MISSES); // Compulsory.
            int64_t cold_misses = l1i_stats->get_metric(metric_name_t::COLD_MISSES);       // Cold misses.
            int64_t cap_misses = l1i_stats->get_metric(metric_name_t::CAPACITY_MISSES);    // Capacity misses.
            double miss_rate = (hits + misses > 0) ? (static_cast<double>(misses) / (hits + misses)) : 0.0; // Rate.
            os << "L1I hits, " << hits << "\n";                                     // Output L1I hits.
            os << "L1I misses, " << misses << "\n";                                 // Output L1I misses.
            os << "L1I cold misses, " << cold_misses << "\n";                       // Output L1I cold misses.
            os << "L1I capacity misses, " << cap_misses << "\n";                   // Output L1I capacity misses.
            os << "L1I compulsory misses, " << comp_misses << "\n";                 // Output compulsory.
            os << "L1I miss rate, " << miss_rate << "\n";                           // Output miss rate.
        }
        if (cache_hierarchy_->get_l1i_mshr(0) != nullptr) {
            const auto &mshr_s = cache_hierarchy_->get_l1i_mshr(0)->get_stats();    // L1I MSHR stats.
            os << "L1I MSHR allocs, " << mshr_s.total_allocations << "\n";          // Output MSHR allocs.
            os << "L1I MSHR secondary merges, " << mshr_s.secondary_merges << "\n"; // Output MSHR merges.
            os << "L1I MSHR full stalls, " << mshr_s.full_stalls << "\n";           // Output MSHR stalls.
            os << "L1I MSHR peak occupancy, " << mshr_s.peak_occupancy << "\n";     // Output MSHR peak.
        }
        if (cache_hierarchy_->get_l1i_rq(0) != nullptr) {
            os << "L1I RQ enqueued, " << cache_hierarchy_->get_l1i_rq(0)->total_enqueued() << "\n"; // RQ enqueued.
            os << "L1I RQ peak, " << cache_hierarchy_->get_l1i_rq(0)->peak_occupancy() << "\n";      // RQ peak.
        }

        // L1D cache metrics.
        if (l1d_stats != nullptr) {
            int64_t hits = l1d_stats->get_metric(metric_name_t::HITS);              // L1D hits.
            int64_t misses = l1d_stats->get_metric(metric_name_t::MISSES);          // L1D misses.
            int64_t comp_misses = l1d_stats->get_metric(metric_name_t::COMPULSORY_MISSES); // Compulsory.
            int64_t cold_misses = l1d_stats->get_metric(metric_name_t::COLD_MISSES);       // Cold misses.
            int64_t cap_misses = l1d_stats->get_metric(metric_name_t::CAPACITY_MISSES);    // Capacity misses.
            double miss_rate = (hits + misses > 0) ? (static_cast<double>(misses) / (hits + misses)) : 0.0; // Rate.
            os << "L1D hits, " << hits << "\n";                                     // Output L1D hits.
            os << "L1D misses, " << misses << "\n";                                 // Output L1D misses.
            os << "L1D cold misses, " << cold_misses << "\n";                       // Output L1D cold misses.
            os << "L1D capacity misses, " << cap_misses << "\n";                   // Output L1D capacity misses.
            os << "L1D compulsory misses, " << comp_misses << "\n";                 // Output compulsory.
            os << "L1D miss rate, " << miss_rate << "\n";                           // Output miss rate.
        }
        if (cache_hierarchy_->get_l1d_mshr(0) != nullptr) {
            const auto &mshr_s = cache_hierarchy_->get_l1d_mshr(0)->get_stats();    // L1D MSHR stats.
            os << "L1D MSHR allocs, " << mshr_s.total_allocations << "\n";          // Output MSHR allocs.
            os << "L1D MSHR secondary merges, " << mshr_s.secondary_merges << "\n"; // Output MSHR merges.
            os << "L1D MSHR full stalls, " << mshr_s.full_stalls << "\n";           // Output MSHR stalls.
            os << "L1D MSHR peak occupancy, " << mshr_s.peak_occupancy << "\n";     // Output MSHR peak.
        }
        if (cache_hierarchy_->get_l1d_rq(0) != nullptr) {
            os << "L1D RQ enqueued, " << cache_hierarchy_->get_l1d_rq(0)->total_enqueued() << "\n"; // RQ enqueued.
            os << "L1D RQ peak, " << cache_hierarchy_->get_l1d_rq(0)->peak_occupancy() << "\n";      // RQ peak.
        }
        if (cache_hierarchy_->get_l1d_wq(0) != nullptr) {
            os << "L1D WQ enqueued, " << cache_hierarchy_->get_l1d_wq(0)->total_enqueued() << "\n";   // WQ enqueued.
            os << "L1D WQ coalesced, " << cache_hierarchy_->get_l1d_wq(0)->coalesced_writes() << "\n"; // WQ coalesced.
            os << "L1D WQ peak, " << cache_hierarchy_->get_l1d_wq(0)->peak_occupancy() << "\n";        // WQ peak.
        }

        // L2 cache metrics.
        if (l2_stats != nullptr) {
            int64_t hits = l2_stats->get_metric(metric_name_t::HITS);                // L2 hits.
            int64_t misses = l2_stats->get_metric(metric_name_t::MISSES);            // L2 misses.
            int64_t comp_misses = l2_stats->get_metric(metric_name_t::COMPULSORY_MISSES); // Compulsory.
            int64_t cold_misses = l2_stats->get_metric(metric_name_t::COLD_MISSES);       // Cold misses.
            int64_t cap_misses = l2_stats->get_metric(metric_name_t::CAPACITY_MISSES);    // Capacity misses.
            int64_t child_hits = l2_stats->get_metric(metric_name_t::CHILD_HITS);    // Child hits.
            double local_miss_rate = (hits + misses > 0) ? (static_cast<double>(misses) / (hits + misses)) : 0.0;
            double total_miss_rate = (child_hits + hits + misses > 0)
                ? (static_cast<double>(misses) / (child_hits + hits + misses))
                : 0.0;
            os << "L2 hits, " << hits << "\n";                                       // Output L2 hits.
            os << "L2 misses, " << misses << "\n";                                   // Output L2 misses.
            os << "L2 cold misses, " << cold_misses << "\n";                         // Output L2 cold misses.
            os << "L2 capacity misses, " << cap_misses << "\n";                     // Output L2 capacity misses.
            os << "L2 compulsory misses, " << comp_misses << "\n";                   // Output compulsory.
            os << "L2 local miss rate, " << local_miss_rate << "\n";                 // Output local miss rate.
            os << "L2 child hits, " << child_hits << "\n";                           // Output child hits.
            os << "L2 total miss rate, " << total_miss_rate << "\n";                 // Output total miss rate.
        }
        if (cache_hierarchy_->get_l2_mshr(0) != nullptr) {
            const auto &mshr_s = cache_hierarchy_->get_l2_mshr(0)->get_stats();      // L2 MSHR stats.
            os << "L2 MSHR allocs, " << mshr_s.total_allocations << "\n";            // Output MSHR allocs.
            os << "L2 MSHR secondary merges, " << mshr_s.secondary_merges << "\n";   // Output MSHR merges.
            os << "L2 MSHR full stalls, " << mshr_s.full_stalls << "\n";             // Output MSHR stalls.
            os << "L2 MSHR peak occupancy, " << mshr_s.peak_occupancy << "\n";       // Output MSHR peak.
        }

        // LLC / L3 cache metrics.
        if (l3_stats != nullptr) {
            int64_t hits = l3_stats->get_metric(metric_name_t::HITS);                // LLC hits.
            int64_t misses = l3_stats->get_metric(metric_name_t::MISSES);            // LLC misses.
            int64_t comp_misses = l3_stats->get_metric(metric_name_t::COMPULSORY_MISSES); // Compulsory.
            int64_t cold_misses = l3_stats->get_metric(metric_name_t::COLD_MISSES);       // Cold misses.
            int64_t cap_misses = l3_stats->get_metric(metric_name_t::CAPACITY_MISSES);    // Capacity misses.
            int64_t child_hits = l3_stats->get_metric(metric_name_t::CHILD_HITS);    // Child hits.
            double local_miss_rate = (hits + misses > 0) ? (static_cast<double>(misses) / (hits + misses)) : 0.0;
            double total_miss_rate = (child_hits + hits + misses > 0)
                ? (static_cast<double>(misses) / (child_hits + hits + misses))
                : 0.0;
            os << "LLC hits, " << hits << "\n";                                      // Output LLC hits.
            os << "LLC misses, " << misses << "\n";                                  // Output LLC misses.
            os << "LLC cold misses, " << cold_misses << "\n";                         // Output LLC cold misses.
            os << "LLC capacity misses, " << cap_misses << "\n";                     // Output LLC capacity misses.
            os << "LLC compulsory misses, " << comp_misses << "\n";                  // Output compulsory.
            os << "LLC local miss rate, " << local_miss_rate << "\n";                // Output local miss rate.
            os << "LLC child hits, " << child_hits << "\n";                          // Output child hits.
            os << "LLC total miss rate, " << total_miss_rate << "\n";                // Output total miss rate.

            // LLC Reuse Distance Histogram (Power of 2):
            // Row 1: Header with power-of-2 labels.
            os << "LLC, labels of histogram of power of 2";
            for (const auto &lbl : caching_device_stats_t::get_reuse_histogram_labels()) {
                os << ", " << lbl; // Print each power-of-2 bin label.
            }
            os << "\n";
            // Row 2: Event counts corresponding to each power-of-2 bin.
            os << "LLC, event countof histogram power of 2";
            const auto &hist = l3_stats->get_reuse_histogram();
            for (int64_t cnt : hist) {
                os << ", " << cnt; // Print event count for bin.
            }
            os << "\n";
        }
        if (cache_hierarchy_->get_l3_mshr() != nullptr) {
            const auto &mshr_s = cache_hierarchy_->get_l3_mshr()->get_stats();       // L3 MSHR stats.
            os << "L3 MSHR allocs, " << mshr_s.total_allocations << "\n";            // Output MSHR allocs.
            os << "L3 MSHR secondary merges, " << mshr_s.secondary_merges << "\n";   // Output MSHR merges.
            os << "L3 MSHR full stalls, " << mshr_s.full_stalls << "\n";             // Output MSHR stalls.
            os << "L3 MSHR peak occupancy, " << mshr_s.peak_occupancy << "\n";       // Output MSHR peak.
        }
    }

    // Output TLB hierarchy metrics (L1 ITLB, L1 DTLB, Unified L2 TLB).
    int64_t total_itlb_hits = 0;                                                    // Aggregate ITLB hits.
    int64_t total_itlb_misses = 0;                                                  // Aggregate ITLB misses.
    int64_t total_itlb_comp_misses = 0;                                             // Aggregate ITLB compulsory misses.
    int64_t total_itlb_cold_misses = 0;                                             // Aggregate ITLB cold misses.
    int64_t total_itlb_cap_misses = 0;                                              // Aggregate ITLB capacity misses.
    int64_t total_dtlb_hits = 0;                                                    // Aggregate DTLB hits.
    int64_t total_dtlb_misses = 0;                                                  // Aggregate DTLB misses.
    int64_t total_dtlb_comp_misses = 0;                                             // Aggregate DTLB compulsory misses.
    int64_t total_dtlb_cold_misses = 0;                                             // Aggregate DTLB cold misses.
    int64_t total_dtlb_cap_misses = 0;                                              // Aggregate DTLB capacity misses.
    int64_t total_l2tlb_hits = 0;                                                   // Aggregate L2TLB hits.
    int64_t total_l2tlb_misses = 0;                                                 // Aggregate L2TLB misses.
    int64_t total_l2tlb_comp_misses = 0;                                            // Aggregate L2TLB compulsory misses.
    int64_t total_l2tlb_cold_misses = 0;                                            // Aggregate L2TLB cold misses.
    int64_t total_l2tlb_cap_misses = 0;                                             // Aggregate L2TLB capacity misses.
    // Aggregate count of sector block tag hits in L2 TLB.
    int64_t total_l2tlb_sector_tag_hits = 0;
    // Aggregate count of full sector block tag misses in L2 TLB.
    int64_t total_l2tlb_sector_tag_misses = 0;
    // Aggregate count of sub-entry invalid misses in L2 TLB.
    int64_t total_l2tlb_sector_subentry_misses = 0;

    // Sum TLB statistics across all simulated CPU cores.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {                           // Iterate cores.
        if (i < itlbs_.size() && itlbs_[i] != nullptr && itlbs_[i]->get_stats() != nullptr) { // Check ITLB.
            total_itlb_hits += itlbs_[i]->get_stats()->get_metric(metric_name_t::HITS); // Sum hits.
            total_itlb_misses += itlbs_[i]->get_stats()->get_metric(metric_name_t::MISSES); // Sum misses.
            total_itlb_comp_misses += itlbs_[i]->get_stats()->get_metric(metric_name_t::COMPULSORY_MISSES); // Sum compulsory.
            total_itlb_cold_misses += itlbs_[i]->get_stats()->get_metric(metric_name_t::COLD_MISSES); // Sum cold.
            total_itlb_cap_misses += itlbs_[i]->get_stats()->get_metric(metric_name_t::CAPACITY_MISSES); // Sum capacity.
        }
        if (i < dtlbs_.size() && dtlbs_[i] != nullptr && dtlbs_[i]->get_stats() != nullptr) { // Check DTLB.
            total_dtlb_hits += dtlbs_[i]->get_stats()->get_metric(metric_name_t::HITS); // Sum hits.
            total_dtlb_misses += dtlbs_[i]->get_stats()->get_metric(metric_name_t::MISSES); // Sum misses.
            total_dtlb_comp_misses += dtlbs_[i]->get_stats()->get_metric(metric_name_t::COMPULSORY_MISSES); // Sum compulsory.
            total_dtlb_cold_misses += dtlbs_[i]->get_stats()->get_metric(metric_name_t::COLD_MISSES); // Sum cold.
            total_dtlb_cap_misses += dtlbs_[i]->get_stats()->get_metric(metric_name_t::CAPACITY_MISSES); // Sum capacity.
        }
        if (i < l2_tlbs_.size() && l2_tlbs_[i] != nullptr && l2_tlbs_[i]->get_stats() != nullptr) { // Check L2TLB.
            total_l2tlb_hits += l2_tlbs_[i]->get_stats()->get_metric(metric_name_t::HITS); // Sum hits.
            total_l2tlb_misses += l2_tlbs_[i]->get_stats()->get_metric(metric_name_t::MISSES); // Sum misses.
            total_l2tlb_comp_misses += l2_tlbs_[i]->get_stats()->get_metric(metric_name_t::COMPULSORY_MISSES); // Sum compulsory.
            total_l2tlb_cold_misses += l2_tlbs_[i]->get_stats()->get_metric(metric_name_t::COLD_MISSES); // Sum cold.
            total_l2tlb_cap_misses += l2_tlbs_[i]->get_stats()->get_metric(metric_name_t::CAPACITY_MISSES); // Sum capacity.
            // Cast stats pointer to access extended TLB sector counters.
            tlb_stats_t *ls = (tlb_stats_t *)l2_tlbs_[i]->get_stats();
            // Aggregate sector block tag hits.
            total_l2tlb_sector_tag_hits += ls->get_sector_tag_hits();
            // Aggregate full sector block tag misses.
            total_l2tlb_sector_tag_misses += ls->get_sector_tag_misses();
            // Aggregate sub-entry invalid misses.
            total_l2tlb_sector_subentry_misses += ls->get_sector_subentry_misses();
        }
    }

    // Compute TLB miss rates.
    double itlb_miss_rate = (total_itlb_hits + total_itlb_misses > 0)               // Calculate ITLB rate.
        ? (static_cast<double>(total_itlb_misses) / (total_itlb_hits + total_itlb_misses)) // Compute fraction.
        : 0.0;                                                                      // Zero fallback.
    double dtlb_miss_rate = (total_dtlb_hits + total_dtlb_misses > 0)               // Calculate DTLB rate.
        ? (static_cast<double>(total_dtlb_misses) / (total_dtlb_hits + total_dtlb_misses)) // Compute fraction.
        : 0.0;                                                                      // Zero fallback.
    double l2tlb_miss_rate = (total_l2tlb_hits + total_l2tlb_misses > 0)             // Calculate L2TLB rate.
        ? (static_cast<double>(total_l2tlb_misses) / (total_l2tlb_hits + total_l2tlb_misses)) // Compute fraction.
        : 0.0;                                                                      // Zero fallback.

    // Emit ITLB CSV statistics.
    os << "ITLB hits, " << total_itlb_hits << "\n";                                 // Output ITLB hits.
    os << "ITLB misses, " << total_itlb_misses << "\n";                             // Output ITLB misses.
    os << "ITLB cold misses, " << total_itlb_cold_misses << "\n";                   // Output ITLB cold misses.
    os << "ITLB capacity misses, " << total_itlb_cap_misses << "\n";               // Output ITLB capacity misses.
    os << "ITLB compulsory misses, " << total_itlb_comp_misses << "\n";             // Output ITLB compulsory misses.
    os << "ITLB miss rate, " << itlb_miss_rate << "\n";                             // Output ITLB miss rate.

    // Emit DTLB CSV statistics.
    os << "DTLB hits, " << total_dtlb_hits << "\n";                                 // Output DTLB hits.
    os << "DTLB misses, " << total_dtlb_misses << "\n";                             // Output DTLB misses.
    os << "DTLB cold misses, " << total_dtlb_cold_misses << "\n";                   // Output DTLB cold misses.
    os << "DTLB capacity misses, " << total_dtlb_cap_misses << "\n";               // Output DTLB capacity misses.
    os << "DTLB compulsory misses, " << total_dtlb_comp_misses << "\n";             // Output DTLB compulsory misses.
    os << "DTLB miss rate, " << dtlb_miss_rate << "\n";                             // Output DTLB miss rate.

    // Emit unified L2 TLB CSV statistics.
    os << "L2TLB hits, " << total_l2tlb_hits << "\n";                               // Output L2TLB hits.
    os << "L2TLB misses, " << total_l2tlb_misses << "\n";                           // Output L2TLB misses.
    os << "L2TLB cold misses, " << total_l2tlb_cold_misses << "\n";                 // Output L2TLB cold misses.
    os << "L2TLB capacity misses, " << total_l2tlb_cap_misses << "\n";             // Output L2TLB capacity misses.
    os << "L2TLB compulsory misses, " << total_l2tlb_comp_misses << "\n";           // Output L2TLB compulsory misses.
    os << "L2TLB miss rate, " << l2tlb_miss_rate << "\n";                           // Output L2TLB miss rate.
    os << "L2TLB sector size, " << knobs_.l2_tlb_sector_size << "\n";               // Output L2TLB sector size.
    // Emit sector tag hits in CSV.
    os << "L2TLB sector tag hits, " << total_l2tlb_sector_tag_hits << "\n";
    // Emit full sector tag misses in CSV.
    os << "L2TLB sector tag misses, " << total_l2tlb_sector_tag_misses << "\n";
    // Emit sub-entry invalid misses in CSV.
    os << "L2TLB sector subentry misses, " << total_l2tlb_sector_subentry_misses << "\n";
    os << "L2TLB total entries, " << knobs_.l2_tlb_entries << "\n";                 // Output total PTE entries.
    os << "L2TLB associativity, " << knobs_.l2_tlb_assoc << "\n";                   // Output L2TLB associativity.
    unsigned int l2_sets_stat = (knobs_.l2_tlb_sector_size > 0 && knobs_.l2_tlb_assoc > 0) // Calculate set count.
        ? (knobs_.l2_tlb_entries / (knobs_.l2_tlb_sector_size * knobs_.l2_tlb_assoc)) // Sets = entries / (sector * assoc).
        : 256;                                                                      // Fallback default sets.
    os << "L2TLB sets, " << l2_sets_stat << "\n";                                   // Output L2TLB set count.

    // MMU and Page Table Walker metrics.
    if (!walkers_.empty() && walkers_[0] != nullptr) {
        const mmu_stats_t &m_stats = walkers_[0]->get_stats();                       // Core 0 MMU stats.
        os << "PTW walks, " << m_stats.get_total_ptw_walks() << "\n";                // Walks.
        os << "PTW 4KB walks, " << m_stats.get_walks_4kb() << "\n";                  // 4KB walks.
        os << "PTW 2MB walks, " << m_stats.get_walks_2mb() << "\n";                  // 2MB walks.
        os << "PTW 1GB walks, " << m_stats.get_walks_1gb() << "\n";                  // 1GB walks.
        os << "PTW page faults, " << m_stats.get_page_faults() << "\n";              // Page faults.
        os << "PTW memory reads, " << m_stats.get_total_ptw_memory_reads() << "\n";  // PTE memory reads.
        os << "PWC hits, " << m_stats.get_pwc_hits() << "\n";                        // PWC hits.
        os << "PWC misses, " << m_stats.get_pwc_misses() << "\n";                    // PWC misses.
        os << "PTW cache hits, " << m_stats.get_ptw_cache_hits() << "\n";            // Cache hits.
        os << "PTW cache misses, " << m_stats.get_ptw_cache_misses() << "\n";        // Cache misses.
        double avg_steps = (m_stats.get_total_ptw_walks() > 0)
            ? (static_cast<double>(m_stats.get_total_ptw_memory_reads()) / m_stats.get_total_ptw_walks())
            : 0.0;
        os << "Avg PTW mem reads, " << avg_steps << "\n";                            // Avg steps.
    }

    // Vector format for PTW hit where count:
    // First row: "cpu0,ptw_level0, L1I,L1D,L2,LLC,DRAM"
    // Followed by rows for each level.
    for (unsigned int c = 0; c < knobs_.num_cores; ++c) {
        if (c < walkers_.size() && walkers_[c] != nullptr) {
            const mmu_stats_t &m_stats = walkers_[c]->get_stats();                   // Walker stats.
            os << "cpu" << c << ",ptw_level0, L1I,L1D,L2,LLC,DRAM\n";               // First row header.
            // 0-based indexing:
            // ptw_level0: Leaf PTE (Level 1 in 1-based)
            int64_t l1_l1d = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_1_PTE, ptw_cache_hit_level_t::HIT_L1D);
            int64_t l1_l2  = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_1_PTE, ptw_cache_hit_level_t::HIT_L2);
            int64_t l1_l3  = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_1_PTE, ptw_cache_hit_level_t::HIT_L3);
            int64_t l1_dram = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_1_PTE, ptw_cache_hit_level_t::HIT_DRAM);
            os << "cpu" << c << ",ptw_level0, 0, " << l1_l1d << ", " << l1_l2 << ", " << l1_l3 << ", " << l1_dram << "\n";

            // ptw_level1: PDE (Level 2 in 1-based)
            int64_t l2_l1d = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_2_PDE, ptw_cache_hit_level_t::HIT_L1D);
            int64_t l2_l2  = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_2_PDE, ptw_cache_hit_level_t::HIT_L2);
            int64_t l2_l3  = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_2_PDE, ptw_cache_hit_level_t::HIT_L3);
            int64_t l2_dram = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_2_PDE, ptw_cache_hit_level_t::HIT_DRAM);
            os << "cpu" << c << ",ptw_level1, 0, " << l2_l1d << ", " << l2_l2 << ", " << l2_l3 << ", " << l2_dram << "\n";

            // ptw_level2: PDPTE (Level 3 in 1-based)
            int64_t l3_l1d = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_3_PDPTE, ptw_cache_hit_level_t::HIT_L1D);
            int64_t l3_l2  = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_3_PDPTE, ptw_cache_hit_level_t::HIT_L2);
            int64_t l3_l3  = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_3_PDPTE, ptw_cache_hit_level_t::HIT_L3);
            int64_t l3_dram = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_3_PDPTE, ptw_cache_hit_level_t::HIT_DRAM);
            os << "cpu" << c << ",ptw_level2, 0, " << l3_l1d << ", " << l3_l2 << ", " << l3_l3 << ", " << l3_dram << "\n";

            // ptw_level3: PML4E (Level 4 in 1-based)
            int64_t l4_l1d = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_4_PML4E, ptw_cache_hit_level_t::HIT_L1D);
            int64_t l4_l2  = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_4_PML4E, ptw_cache_hit_level_t::HIT_L2);
            int64_t l4_l3  = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_4_PML4E, ptw_cache_hit_level_t::HIT_L3);
            int64_t l4_dram = m_stats.get_level_cache_hits(ptw_level_t::PTW_LEVEL_4_PML4E, ptw_cache_hit_level_t::HIT_DRAM);
            os << "cpu" << c << ",ptw_level3, 0, " << l4_l1d << ", " << l4_l2 << ", " << l4_l3 << ", " << l4_dram << "\n";

            // Also emit ptw_level4 as alias for PML4E for 1-based indexers.
            os << "cpu" << c << ",ptw_level4, 0, " << l4_l1d << ", " << l4_l2 << ", " << l4_l3 << ", " << l4_dram << "\n";
        }
    }
}

bool
cpu_simulator_t::print_results()
{
    // Print comprehensive human-readable simulation results.
    std::cerr << "===================================================================\n";
    std::cerr << "       OUT-OF-ORDER CPU, MMU & 3-LEVEL CACHE SIMULATION RESULTS     \n";
    std::cerr << "===================================================================\n";

    // 1. Print per-core CPU pipeline statistics (Fetch, BTB, ROB, LSQ, Cycles, IPC).
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        cores_[i]->flush_and_drain_pipeline(); // Drain remaining instructions.
        cores_[i]->print_stats(std::cerr, "  "); // Print core stats.
    }

    // 2. Print 3-level data cache hierarchy statistics (L1D, L2, L3 hits & misses).
    std::cerr << "-------------------------------------------------------------------\n";
    std::cerr << "  3-Level Data Cache Hierarchy (Application + PTW accesses):\n";
    std::cerr << "-------------------------------------------------------------------\n";
    cache_hierarchy_->print_stats("  ");

    // 3. Print TLB hierarchy statistics (L1 ITLB, L1 DTLB, Unified L2 TLB).
    std::cerr << "-------------------------------------------------------------------\n";
    std::cerr << "  TLB Hierarchy Statistics (L1 ITLB, L1 DTLB, Unified L2 TLB):\n";
    std::cerr << "-------------------------------------------------------------------\n";
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {                                 // Iterate cores.
        std::cerr << "  Core #" << i << " L1 ITLB stats:\n";                              // Core ITLB header.
        if (i < itlbs_.size() && itlbs_[i] != nullptr && itlbs_[i]->get_stats() != nullptr) { // Check valid ITLB.
            itlbs_[i]->get_stats()->print_stats("    ");                                   // Print ITLB stats.
        }
        std::cerr << "  Core #" << i << " L1 DTLB stats:\n";                              // Core DTLB header.
        if (i < dtlbs_.size() && dtlbs_[i] != nullptr && dtlbs_[i]->get_stats() != nullptr) { // Check valid DTLB.
            dtlbs_[i]->get_stats()->print_stats("    ");                                   // Print DTLB stats.
        }
        unsigned int l2_sets_disp = (knobs_.l2_tlb_sector_size > 0 && knobs_.l2_tlb_assoc > 0) // Calculate set count.
            ? (knobs_.l2_tlb_entries / (knobs_.l2_tlb_sector_size * knobs_.l2_tlb_assoc)) // Sets = entries / (sector * assoc).
            : 256;                                                                          // Fallback default sets.
        std::cerr << "  Core #" << i << " Unified L2 TLB (Sector=" << knobs_.l2_tlb_sector_size // Output sector header.
                  << ", Geometry=" << l2_sets_disp << "x" << knobs_.l2_tlb_assoc << ") stats:\n"; // Output geometry.
        if (i < l2_tlbs_.size() && l2_tlbs_[i] != nullptr && l2_tlbs_[i]->get_stats() != nullptr) { // Check valid L2TLB.
            l2_tlbs_[i]->get_stats()->print_stats("    ");                                 // Print L2 TLB stats.
            // Retrieve typed TLB stats for core L2 TLB.
            tlb_stats_t *ls = (tlb_stats_t *)l2_tlbs_[i]->get_stats();
            // If sector mode is active, display detailed sector breakdown.
            if (knobs_.l2_tlb_sector_size > 1) {
                // Print sector tag hits.
                std::cerr << "      Sector tag hits:                 " << ls->get_sector_tag_hits() << "\n";
                // Print full sector tag misses.
                std::cerr << "      Sector tag misses:               " << ls->get_sector_tag_misses() << "\n";
                // Print sub-entry invalid misses.
                std::cerr << "      Sector sub-entry misses:         " << ls->get_sector_subentry_misses() << "\n";
            }
        }
    }

    // 4. Print MMU / Page Table Walker statistics.
    std::cerr << "-------------------------------------------------------------------\n";
    std::cerr << "  MMU, Page Table Walk & Page Walk Cache (PWC) Statistics:\n";
    std::cerr << "-------------------------------------------------------------------\n";
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        std::cerr << "  Core #" << i << " PTW stats:\n";
        walkers_[i]->get_stats().print_stats("    ");
    }

    // 5. Output CSV formatted simulation results to std::cerr.
    std::cerr << "-------------------------------------------------------------------\n";
    std::cerr << "  CSV Simulation Statistics:\n";
    std::cerr << "-------------------------------------------------------------------\n";
    print_csv_stats(std::cerr);

    // 6. Also write CSV results directly to stats_file (default stats.log).
    if (!knobs_.stats_file.empty()) {
        std::ofstream ofs(knobs_.stats_file);
        if (ofs.is_open()) {
            print_csv_stats(ofs);
            ofs.close();
            std::cerr << "Statistics log saved to: " << knobs_.stats_file << std::endl;
        } else {
            std::cerr << "Warning: Could not open stats file for writing: "
                      << knobs_.stats_file << std::endl;
        }
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

// Factory function creating an instance of the OoO CPU and 3-level cache simulator.
analysis_tool_t *
cpu_simulator_create(const cpu_simulator_knobs_t &knobs)
{
    // Allocate and return a new cpu_simulator_t analysis tool instance.
    return new cpu_simulator_t(knobs);
}

} // namespace drmemtrace
} // namespace dynamorio
