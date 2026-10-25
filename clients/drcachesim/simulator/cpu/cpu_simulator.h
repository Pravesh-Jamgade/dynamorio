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

/* cpu_simulator.h: Full multi-core out-of-order CPU simulator integrating
 * Front-End, BTB, ROB, Register Dependencies, LSQ with store forwarding,
 * 4-level radix MMU/PTW, and a 3-level data cache hierarchy.
 */

#ifndef _DRMEMTRACE_CPU_SIMULATOR_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_CPU_SIMULATOR_H_ 1 // Define inclusion guard macro.

// Provide std::unique_ptr for RAII ownership.
#include <memory>
// Provide dynamic array container.
#include <vector>
// Provide standard string class.
#include <string>

// Provide CPU Core pipeline definitions.
#include "cpu/cpu_core.h"
// Provide 3-Level Cache hierarchy.
#include "cpu/three_level_cache.h"
// Provide MMU page table and walker definitions.
#include "mmu/page_table.h"
#include "mmu/page_table_walker.h"
// Provide TLB definitions.
#include "tlb.h"
// Provide base simulator_t class.
#include "simulator.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Configuration knobs for the full CPU simulator.
 */
struct cpu_simulator_knobs_t {
    // Default constructor configuring a high-performance modern multi-core system.
    cpu_simulator_knobs_t()
        : num_cores(1)                          // Default 1 simulated CPU core.
        , fetch_width(4)                        // 4-wide fetch.
        , decode_width(4)                       // 4-wide decode.
        , dispatch_width(4)                     // 4-wide dispatch.
        , commit_width(4)                       // 4-wide commit.
        , rob_capacity(256)                     // 256-entry ROB.
        , lq_capacity(72)                       // 72-entry Load Queue.
        , sq_capacity(56)                       // 56-entry Store Queue.
        , btb_entries(1024)                     // 1024-entry BTB.
        , branch_mispredict_penalty(14)         // 14-cycle branch flush penalty.
        , alu_latency(1)                        // 1-cycle ALU latency.
        , enable_wrong_path(true)               // Enable speculative wrong-path execution.
        , max_speculative_depth(32)             // Max instructions fetched along wrong path.
        , line_size(64)                         // 64-byte cache lines.
        , l1i_size(32 * 1024)                   // 32 KB private L1I cache per core.
        , l1i_assoc(8)                          // 8-way L1I.
        , l1i_latency(1)                        // 1-cycle L1I hit latency.
        , l1d_size(32 * 1024)                   // 32 KB private L1D cache per core.
        , l1d_assoc(8)                          // 8-way L1D.
        , l2_size(256 * 1024)                   // 256 KB private L2 cache per core.
        , l2_assoc(8)                           // 8-way L2.
        , l3_size(8 * 1024 * 1024)              // 8 MB shared L3 / LLC cache.
        , l3_assoc(16)                          // 16-way L3.
        , pwc_entries(32)                       // 32-entry Page Walk Cache per core.
        , pwc_assoc(4)                          // 4-way PWC.
        , page_size(4096)                       // 4 KB base page size.
        , l2_tlb_entries(1536)                  // Default 1536 total PTE entries for L2 TLB.
        , l2_tlb_assoc(6)                       // Default 6-way associativity (baseline: 256x6).
        , l2_tlb_sector_size(1)                 // Default 1 (baseline mode; 2, 4, 8 for sector mode).
        , l1i_mshr_capacity(16)                 // 16 MSHRs for L1I.
        , l1i_rq_capacity(32)                   // 32 Read Queue entries for L1I.
        , l1i_pq_capacity(32)                   // 32 Prefetch Queue entries for L1I.
        , l1d_mshr_capacity(16)                 // 16 MSHRs for L1D.
        , l1d_rq_capacity(32)                   // 32 Read Queue entries for L1D.
        , l1d_wq_capacity(32)                   // 32 Write Queue entries for L1D.
        , l1d_pq_capacity(32)                   // 32 Prefetch Queue entries for L1D.
        , l2_mshr_capacity(32)                  // 32 MSHRs for unified L2.
        , l2_rq_capacity(48)                    // 48 Read Queue entries for L2.
        , l2_wq_capacity(48)                    // 48 Write Queue entries for L2.
        , l2_pq_capacity(32)                    // 32 Prefetch Queue entries for L2.
        , l3_mshr_capacity(64)                  // 64 MSHRs for shared L3/LLC.
        , l3_rq_capacity(64)                    // 64 Read Queue entries for L3.
        , l3_wq_capacity(64)                    // 64 Write Queue entries for L3.
        , l3_pq_capacity(64)                    // 64 Prefetch Queue entries for L3.
        , dram_latency(150)                     // 150-cycle main memory DRAM latency.
        , heartbeat_interval(10000000)          // Default 10M simulated instructions heartbeat.
        , stats_file("stats.log")               // Default output file for CSV simulation stats.
    {
    }

    unsigned int num_cores;                     // Number of simulated cores.
    size_t fetch_width;                         // Fetch bandwidth.
    size_t decode_width;                        // Decode bandwidth.
    size_t dispatch_width;                      // Dispatch bandwidth.
    size_t commit_width;                        // Commit bandwidth.
    size_t rob_capacity;                        // ROB size.
    size_t lq_capacity;                         // Load Queue size.
    size_t sq_capacity;                         // Store Queue size.
    size_t btb_entries;                         // BTB size.
    uint64_t branch_mispredict_penalty;         // Pipeline flush penalty.
    uint64_t alu_latency;                       // ALU latency.
    bool enable_wrong_path;                     // Whether to simulate speculative wrong-path execution.
    size_t max_speculative_depth;               // Max speculative instructions along wrong path.
    uint64_t line_size;                         // Cache block size in bytes.
    uint64_t l1i_size;                          // L1I size in bytes.
    unsigned int l1i_assoc;                     // L1I associativity.
    uint64_t l1i_latency;                       // L1I hit latency.
    uint64_t l1d_size;                          // L1D size in bytes.
    unsigned int l1d_assoc;                     // L1D associativity.
    uint64_t l2_size;                           // L2 size in bytes.
    unsigned int l2_assoc;                      // L2 associativity.
    uint64_t l3_size;                           // L3/LLC size in bytes.
    unsigned int l3_assoc;                      // L3 associativity.
    unsigned int pwc_entries;                   // PWC entries.
    unsigned int pwc_assoc;                     // PWC associativity.
    uint64_t page_size;                         // Base page size.
    unsigned int l2_tlb_entries;                // Total L2 TLB PTE entries (baseline capacity).
    unsigned int l2_tlb_assoc;                  // Associativity (ways) for L2 TLB.
    unsigned int l2_tlb_sector_size;            // Sector size: 1 (baseline: 256x6), 2 (128x6), 4 (64x6), or 8 (32x6).
    size_t l1i_mshr_capacity;                   // L1I MSHR size.
    size_t l1i_rq_capacity;                     // L1I Read Queue size.
    size_t l1i_pq_capacity;                     // L1I Prefetch Queue size.
    size_t l1d_mshr_capacity;                   // L1D MSHR size.
    size_t l1d_rq_capacity;                     // L1D Read Queue size.
    size_t l1d_wq_capacity;                     // L1D Write Queue size.
    size_t l1d_pq_capacity;                     // L1D Prefetch Queue size.
    size_t l2_mshr_capacity;                    // L2 MSHR size.
    size_t l2_rq_capacity;                      // L2 Read Queue size.
    size_t l2_wq_capacity;                      // L2 Write Queue size.
    size_t l2_pq_capacity;                      // L2 Prefetch Queue size.
    size_t l3_mshr_capacity;                    // L3 MSHR size.
    size_t l3_rq_capacity;                      // L3 Read Queue size.
    size_t l3_wq_capacity;                      // L3 Write Queue size.
    size_t l3_pq_capacity;                      // L3 Prefetch Queue size.
    uint64_t dram_latency;                      // DRAM access latency in cycles.
    uint64_t heartbeat_interval;                // Instruction interval between progress heartbeats (0 disables).
    std::string stats_file;                     // Output file path to write CSV statistics (default stats.log).
};

/**
 * @brief Analysis tool implementing full CPU pipeline, MMU, and 3-level cache simulation.
 */
class cpu_simulator_t : public simulator_t {
public:
    /**
     * @brief Constructs and initializes the full multi-core CPU and memory simulator.
     */
    explicit cpu_simulator_t(const cpu_simulator_knobs_t &knobs);

    /**
     * @brief Destructor.
     */
    virtual ~cpu_simulator_t();

    /**
     * @brief Ingests and simulates a single trace memory reference.
     */
    bool
    process_memref(const memref_t &memref) override;

    /**
     * @brief Prints comprehensive simulation results across all cores and memory levels.
     */
    bool
    print_results() override;

    /**
     * @brief Formats and writes simulation results in easily scrapable CSV format.
     *
     * @param os Output stream destination (e.g. std::cerr or stats.log file).
     */
    void
    print_csv_stats(std::ostream &os) const;

    /**
     * @brief Prints periodic instruction progress heartbeat to std::cerr.
     *
     * @param instrs Instruction count at current heartbeat trigger.
     */
    void
    print_heartbeat(uint64_t instrs) const;

    /**
     * @brief Returns a specific CPU core simulator instance.
     */
    cpu_core_t *
    get_core(unsigned int core_index) const;

    /**
     * @brief Returns the 3-level data cache hierarchy.
     */
    three_level_cache_t *
    get_cache_hierarchy() const;

    /**
     * @brief Returns the L1 Instruction TLB for the specified core index.
     */
    tlb_t *
    get_itlb(unsigned int core_index) const
    {
        // Validate core index within range.
        if (core_index < itlbs_.size()) {
            // Return raw pointer to core's L1 ITLB.
            return itlbs_[core_index].get();
        }
        // Return nullptr on invalid index.
        return nullptr;
    }

    /**
     * @brief Returns the L1 Data TLB for the specified core index.
     */
    tlb_t *
    get_dtlb(unsigned int core_index) const
    {
        // Validate core index within range.
        if (core_index < dtlbs_.size()) {
            // Return raw pointer to core's L1 DTLB.
            return dtlbs_[core_index].get();
        }
        // Return nullptr on invalid index.
        return nullptr;
    }

    /**
     * @brief Returns the unified L2 TLB for the specified core index.
     */
    tlb_t *
    get_l2_tlb(unsigned int core_index) const
    {
        // Validate core index within range.
        if (core_index < l2_tlbs_.size()) {
            // Return raw pointer to core's unified L2 TLB.
            return l2_tlbs_[core_index].get();
        }
        // Return nullptr on invalid index.
        return nullptr;
    }

    /**
     * @brief Returns the configuration knobs.
     */
    const cpu_simulator_knobs_t &
    get_knobs() const
    {
        return knobs_;
    }

    /**
     * @brief Returns the number of simulated CPU cores.
     */
    size_t
    num_cores() const
    {
        return cores_.size();
    }

private:
    cpu_simulator_knobs_t knobs_;                             // Configuration knobs.
    std::unique_ptr<three_level_cache_t> cache_hierarchy_;    // Unified 3-level data cache.
    std::unique_ptr<mmu_address_space_manager_t> address_space_mgr_; // Radix page tables.
    std::vector<std::unique_ptr<page_table_walker_t>> walkers_; // Per-core PTWs.
    std::vector<std::unique_ptr<tlb_t>> itlbs_;               // Per-core L1 ITLBs.
    std::vector<std::unique_ptr<tlb_t>> dtlbs_;               // Per-core L1 DTLBs.
    std::vector<std::unique_ptr<tlb_t>> l2_tlbs_;             // Per-core unified L2 TLBs.
    std::vector<std::unique_ptr<cpu_core_t>> cores_;          // Per-core OoO CPU pipelines.
    uint64_t total_simulated_instrs_ = 0;                     // Counter of total simulated instructions.
    uint64_t last_heartbeat_instrs_ = 0;                      // Instruction count when last heartbeat printed.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_CPU_SIMULATOR_H_ */
