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

/* three_level_cache.h: Implements a 3-level cache hierarchy (private split L1I and L1D,
 * private unified L2, and shared unified L3/LLC) through which both hardware Page Table Walks
 * (on Page Walk Cache misses), CPU instruction fetches, and CPU demand memory accesses flow.
 */

#ifndef _DRMEMTRACE_THREE_LEVEL_CACHE_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_THREE_LEVEL_CACHE_H_ 1 // Define inclusion guard macro.

// Provide std::unique_ptr for RAII ownership.
#include <memory>
// Provide std::string for formatting names and stats.
#include <string>
// Provide std::vector for per-core cache arrays.
#include <vector>
// Provide std::iostream for output reporting.
#include <iostream>

// Provide caching_device_t and cache_lru_t definitions.
#include "cache_lru.h"
// Provide cache_stats_t definitions.
#include "cache_stats.h"
// Provide Miss Status Holding Register (MSHR) table definition.
#include "cpu/mshr.h"
// Provide Read, Write, and Prefetch queues definition.
#include "cpu/cache_queues.h"
// Provide memref_t definition.
#include "memref.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Identifies which level of the memory hierarchy serviced a cache request.
 */
enum class cache_hit_level_t {
    HIT_L1I,  // Serviced by private L1 Instruction Cache.
    HIT_L1D,  // Serviced by private L1 Data Cache.
    HIT_L2,   // Serviced by private L2 Cache.
    HIT_L3,   // Serviced by shared Last-Level Cache (L3/LLC).
    HIT_DRAM  // Missed in all cache levels; serviced by main memory DRAM.
};

/**
 * @brief Configuration parameters for a 3-level cache hierarchy.
 */
struct three_level_cache_knobs_t {
    // Default constructor configuring standard modern CPU cache capacities.
    three_level_cache_knobs_t()
        : num_cores(1)                   // Default 1 simulated CPU core.
        , line_size(64)                  // Default 64-byte cache line width.
        , l1i_size(32 * 1024)            // Default 32 KB private L1 Instruction Cache per core.
        , l1i_assoc(8)                   // Default 8-way associativity for L1I.
        , l1d_size(32 * 1024)            // Default 32 KB private L1 Data Cache per core.
        , l1d_assoc(8)                   // Default 8-way associativity for L1D.
        , l2_size(256 * 1024)            // Default 256 KB private L2 Cache per core.
        , l2_assoc(8)                    // Default 8-way associativity for L2.
        , l3_size(8 * 1024 * 1024)       // Default 8 MB shared L3 / Last-Level Cache.
        , l3_assoc(16)                   // Default 16-way associativity for L3.
        , l1i_latency(1)                 // Default 1-cycle L1I hit latency.
        , l1d_latency(4)                 // Default 4-cycle L1D hit latency.
        , l2_latency(12)                 // Default 12-cycle L2 hit latency.
        , l3_latency(35)                 // Default 35-cycle L3 hit latency.
        , dram_latency(150)              // Default 150-cycle main memory DRAM latency.
        , l1i_mshr_capacity(16)          // Default 16 MSHR entries for L1I.
        , l1i_rq_capacity(32)            // Default 32 Read Queue entries for L1I.
        , l1i_pq_capacity(32)            // Default 32 Prefetch Queue entries for L1I.
        , l1d_mshr_capacity(16)          // Default 16 MSHR entries for L1D.
        , l1d_rq_capacity(32)            // Default 32 Read Queue entries for L1D.
        , l1d_wq_capacity(32)            // Default 32 Write Queue entries for L1D.
        , l1d_pq_capacity(32)            // Default 32 Prefetch Queue entries for L1D.
        , l2_mshr_capacity(32)           // Default 32 MSHR entries for unified L2.
        , l2_rq_capacity(48)             // Default 48 Read Queue entries for L2.
        , l2_wq_capacity(48)             // Default 48 Write Queue entries for L2.
        , l2_pq_capacity(32)             // Default 32 Prefetch Queue entries for L2.
        , l3_mshr_capacity(64)           // Default 64 MSHR entries for shared L3/LLC.
        , l3_rq_capacity(64)             // Default 64 Read Queue entries for L3.
        , l3_wq_capacity(64)             // Default 64 Write Queue entries for L3.
        , l3_pq_capacity(64)             // Default 64 Prefetch Queue entries for L3.
    {
    }

    unsigned int num_cores;              // Number of CPU cores sharing the hierarchy.
    uint64_t line_size;                  // Cache block/line size in bytes.
    uint64_t l1i_size;                   // Byte size of each private L1I cache.
    unsigned int l1i_assoc;              // Associativity of each private L1I cache.
    uint64_t l1d_size;                   // Byte size of each private L1D cache.
    unsigned int l1d_assoc;              // Associativity of each private L1D cache.
    uint64_t l2_size;                    // Byte size of each private L2 cache.
    unsigned int l2_assoc;               // Associativity of each private L2 cache.
    uint64_t l3_size;                    // Byte size of shared L3 / LLC cache.
    unsigned int l3_assoc;               // Associativity of shared L3 / LLC cache.
    uint64_t l1i_latency;                // Hit latency in cycles for L1I.
    uint64_t l1d_latency;                // Hit latency in cycles for L1D.
    uint64_t l2_latency;                 // Hit latency in cycles for L2.
    uint64_t l3_latency;                 // Hit latency in cycles for L3.
    uint64_t dram_latency;               // Access latency in cycles for main memory DRAM.
    size_t l1i_mshr_capacity;            // MSHR capacity for L1I.
    size_t l1i_rq_capacity;              // Read Queue capacity for L1I.
    size_t l1i_pq_capacity;              // Prefetch Queue capacity for L1I.
    size_t l1d_mshr_capacity;            // MSHR capacity for L1D.
    size_t l1d_rq_capacity;              // Read Queue capacity for L1D.
    size_t l1d_wq_capacity;              // Write Queue capacity for L1D.
    size_t l1d_pq_capacity;              // Prefetch Queue capacity for L1D.
    size_t l2_mshr_capacity;             // MSHR capacity for L2.
    size_t l2_rq_capacity;               // Read Queue capacity for L2.
    size_t l2_wq_capacity;               // Write Queue capacity for L2.
    size_t l2_pq_capacity;               // Prefetch Queue capacity for L2.
    size_t l3_mshr_capacity;             // MSHR capacity for L3.
    size_t l3_rq_capacity;               // Read Queue capacity for L3.
    size_t l3_wq_capacity;               // Write Queue capacity for L3.
    size_t l3_pq_capacity;               // Prefetch Queue capacity for L3.
};

/**
 * @brief Result outcome for a single access through the 3-level cache hierarchy.
 */
struct cache_access_result_t {
    bool hit_l1i = false;                                    // True if hit in L1I.
    bool hit_l1d = false;                                    // True if hit in L1D.
    bool hit_l2 = false;                                     // True if hit in L2.
    bool hit_l3 = false;                                     // True if hit in L3.
    cache_hit_level_t level = cache_hit_level_t::HIT_DRAM;  // Servicing level.
    uint64_t latency_cycles = 150;                           // Total latency in cycles.
};

/**
 * @brief Complete 3-level cache hierarchy (Private L1I/L1D -> Private L2 -> Shared L3/LLC).
 */
class three_level_cache_t {
public:
    /**
     * @brief Constructs and initializes the 3-level cache hierarchy.
     *
     * @param knobs Configuration sizing, associativity, and latencies.
     */
    explicit three_level_cache_t(const three_level_cache_knobs_t &knobs);

    /**
     * @brief Destructor freeing all allocated caches and statistics objects.
     */
    virtual ~three_level_cache_t();

    /**
     * @brief Processes a CPU demand data read or write through the 3-level hierarchy.
     *
     * @param core   Core index originating the demand memory reference.
     * @param memref The trace memory reference record.
     * @return Completed cache_access_result_t describing servicing level and latency.
     */
    cache_access_result_t
    request(unsigned int core, const memref_t &memref);

    /**
     * @brief Processes an instruction fetch reference through the private L1I -> L2 -> L3 hierarchy.
     *
     * @param core   Core index originating the instruction fetch.
     * @param memref The trace instruction fetch memory reference record.
     * @return Completed cache_access_result_t describing servicing level and latency.
     */
    cache_access_result_t
    request_instruction(unsigned int core, const memref_t &memref);

    /**
     * @brief Processes an 8-byte Page Table Entry (PTE) physical read issued by the
     *        hardware Page Table Walker upon a Page Walk Cache (PWC) miss.
     *
     * @param core          Core index whose walker is reading the PTE.
     * @param pte_phys_addr 8-byte aligned physical address of the page table entry.
     * @return Completed cache_access_result_t describing servicing level and latency.
     */
    cache_access_result_t
    ptw_request(unsigned int core, addr_t pte_phys_addr);

    /**
     * @brief Returns the private L1I cache device pointer for `core`.
     */
    caching_device_t *
    get_l1i_cache(unsigned int core) const;

    /**
     * @brief Returns the private L1D cache device pointer for `core`.
     */
    caching_device_t *
    get_l1d_cache(unsigned int core) const;

    /**
     * @brief Returns the private L2 cache device pointer for `core`.
     */
    caching_device_t *
    get_l2_cache(unsigned int core) const;

    /**
     * @brief Returns the shared L3/LLC cache device pointer.
     */
    caching_device_t *
    get_l3_cache() const;

    // MSHR and Queue accessors.
    mshr_table_t *get_l1i_mshr(unsigned int core) const;
    mshr_table_t *get_l1d_mshr(unsigned int core) const;
    mshr_table_t *get_l2_mshr(unsigned int core) const;
    mshr_table_t *get_l3_mshr() const;

    read_queue_t *get_l1i_rq(unsigned int core) const;
    read_queue_t *get_l1d_rq(unsigned int core) const;
    write_queue_t *get_l1d_wq(unsigned int core) const;
    prefetch_queue_t *get_l1i_pq(unsigned int core) const;
    prefetch_queue_t *get_l1d_pq(unsigned int core) const;
    read_queue_t *get_l2_rq(unsigned int core) const;
    write_queue_t *get_l2_wq(unsigned int core) const;
    prefetch_queue_t *get_l2_pq(unsigned int core) const;
    read_queue_t *get_l3_rq() const;
    write_queue_t *get_l3_wq() const;
    prefetch_queue_t *get_l3_pq() const;

    /**
     * @brief Advances simulation time for all MSHRs across all levels, completing fills.
     *
     * @param current_cycle Current processor simulation cycle.
     */
    void
    tick_mshrs(uint64_t current_cycle);

    /**
     * @brief Drains all outstanding MSHRs and write queues to memory.
     *
     * @param current_cycle Reference to processor simulation cycle counter.
     */
    void
    drain_all_mshrs_and_queues(uint64_t &current_cycle);

    /**
     * @brief Returns the configuration knobs.
     */
    const three_level_cache_knobs_t &
    get_knobs() const
    {
        return knobs_; // Return active configuration knobs.
    }

    /**
     * @brief Prints formatted statistics for all L1I, L1D, L2, and L3 caches to std::cerr.
     *
     * @param prefix Indentation prefix string for each line.
     */
    void
    print_stats(const std::string &prefix = "") const;

    /**
     * @brief Resets all statistics counters across all L1I, L1D, L2, and L3 caches.
     */
    void
    reset_stats();

private:
    three_level_cache_knobs_t knobs_;                         // Hierarchy parameters.
    std::vector<std::unique_ptr<cache_lru_t>> l1i_caches_;    // Per-core private L1Is.
    std::vector<std::unique_ptr<cache_stats_t>> l1i_stats_;   // Per-core L1I statistics.
    std::vector<std::unique_ptr<cache_lru_t>> l1d_caches_;    // Per-core private L1Ds.
    std::vector<std::unique_ptr<cache_stats_t>> l1d_stats_;   // Per-core L1D statistics.
    std::vector<std::unique_ptr<cache_lru_t>> l2_caches_;     // Per-core private L2s.
    std::vector<std::unique_ptr<cache_stats_t>> l2_stats_;    // Per-core L2 statistics.
    std::unique_ptr<cache_lru_t> l3_cache_;                   // Shared unified L3/LLC.
    std::unique_ptr<cache_stats_t> l3_stats_;                 // Shared L3 statistics.

    // Per-core MSHR tables.
    std::vector<std::unique_ptr<mshr_table_t>> l1i_mshrs_;    // Per-core L1I MSHR tables.
    std::vector<std::unique_ptr<mshr_table_t>> l1d_mshrs_;    // Per-core L1D MSHR tables.
    std::vector<std::unique_ptr<mshr_table_t>> l2_mshrs_;     // Per-core L2 MSHR tables.
    std::unique_ptr<mshr_table_t> l3_mshr_;                   // Shared L3 MSHR table.

    // Per-core queues.
    std::vector<std::unique_ptr<read_queue_t>> l1i_rqs_;      // Per-core L1I Read Queues.
    std::vector<std::unique_ptr<prefetch_queue_t>> l1i_pqs_;  // Per-core L1I Prefetch Queues.
    std::vector<std::unique_ptr<read_queue_t>> l1d_rqs_;      // Per-core L1D Read Queues.
    std::vector<std::unique_ptr<write_queue_t>> l1d_wqs_;     // Per-core L1D Write Queues.
    std::vector<std::unique_ptr<prefetch_queue_t>> l1d_pqs_;  // Per-core L1D Prefetch Queues.
    std::vector<std::unique_ptr<read_queue_t>> l2_rqs_;       // Per-core L2 Read Queues.
    std::vector<std::unique_ptr<write_queue_t>> l2_wqs_;      // Per-core L2 Write Queues.
    std::vector<std::unique_ptr<prefetch_queue_t>> l2_pqs_;   // Per-core L2 Prefetch Queues.

    // Shared L3 queues.
    std::unique_ptr<read_queue_t> l3_rq_;                     // Shared L3 Read Queue.
    std::unique_ptr<write_queue_t> l3_wq_;                    // Shared L3 Write Queue.
    std::unique_ptr<prefetch_queue_t> l3_pq_;                 // Shared L3 Prefetch Queue.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_THREE_LEVEL_CACHE_H_ */
