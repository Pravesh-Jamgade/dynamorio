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

// Define inclusion guard macro.
#ifndef _DRMEMTRACE_MSHR_H_
#define _DRMEMTRACE_MSHR_H_ 1

// Provide standard integer types.
#include <stdint.h>
// Provide size_t definition.
#include <stddef.h>
// Provide vector container for tracking secondary coalesced requests.
#include <vector>
// Provide unordered map for fast line address lookups.
#include <unordered_map>
// Provide algorithm header for min/max operations.
#include <algorithm>

// Provide CPU operation types.
#include "cpu/cpu_types.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Represents a single Miss Status Holding Register (MSHR) entry.
 * Tracks an in-flight cache line fill and all secondary requests merged onto it.
 */
struct mshr_entry_t {
    uint64_t line_addr = 0;              // Base address of the cache line being fetched.
    uint64_t alloc_cycle = 0;            // Cycle when the primary miss allocated this entry.
    uint64_t ready_cycle = 0;            // Cycle when the line fill data returns and becomes ready.
    cpu_op_type_t op_type = cpu_op_type_t::OP_LOAD; // Operation type that caused the primary miss.
    uint64_t primary_id = 0;             // Sequence ID of the primary triggering reference.
    bool valid = false;                  // Whether this entry is currently active/allocated.
    std::vector<uint64_t> secondary_ids; // Sequence IDs of secondary coalesced requests.
};

/**
 * @brief Statistics collected for an MSHR table.
 */
struct mshr_stats_t {
    uint64_t total_allocations = 0;      // Total primary misses that allocated an MSHR entry.
    uint64_t secondary_merges = 0;       // Secondary misses coalesced into an existing MSHR entry.
    uint64_t full_stalls = 0;            // Occurrences where a miss stalled because all MSHRs were full.
    uint64_t stall_cycles = 0;           // Total cycles spent waiting for an MSHR slot to free up.
    size_t peak_occupancy = 0;           // Maximum simultaneous active MSHR entries observed.
};

/**
 * @brief Miss Status Holding Register (MSHR) table modeling a non-blocking cache.
 * Supports primary miss allocation, secondary miss coalescing, structural stall modeling,
 * and cycle-accurate line fill wakeups.
 */
class mshr_table_t {
public:
    /**
     * @brief Constructs an MSHR table with specified capacity and line size.
     *
     * @param capacity Maximum number of concurrent outstanding line fills (default: 16).
     * @param line_size Cache line size in bytes (default: 64).
     */
    explicit mshr_table_t(size_t capacity = 16, uint64_t line_size = 64);

    /**
     * @brief Looks up whether a cache line is already in-flight.
     *
     * @param addr Memory address being accessed.
     * @return Pointer to active mshr_entry_t if found; nullptr otherwise.
     */
    mshr_entry_t *
    lookup(uint64_t addr);

    /**
     * @brief Const lookup for query operations.
     */
    const mshr_entry_t *
    lookup(uint64_t addr) const;

    /**
     * @brief Allocates an MSHR entry for a primary cache miss.
     *
     * @param addr Memory address that missed in cache.
     * @param current_cycle Current processor simulation cycle.
     * @param latency_cycles Latency in cycles until line returns from lower level.
     * @param op_type Operation type (load, store, fetch, PTE read).
     * @param ref_id Unique sequence ID of the primary reference.
     * @return Pointer to allocated entry, or nullptr if MSHR table is full.
     */
    mshr_entry_t *
    allocate(uint64_t addr,
             uint64_t current_cycle,
             uint64_t latency_cycles,
             cpu_op_type_t op_type,
             uint64_t ref_id);

    /**
     * @brief Coalesces a secondary miss onto an existing in-flight MSHR entry.
     *
     * @param entry Pointer to the existing in-flight MSHR entry.
     * @param ref_id Unique sequence ID of the secondary reference.
     */
    void
    coalesce_secondary(mshr_entry_t *entry, uint64_t ref_id);

    /**
     * @brief Advances simulation time, completing fills whose ready_cycle <= current_cycle.
     *
     * @param current_cycle Current simulation cycle.
     * @return Number of MSHR entries that completed and retired this cycle.
     */
    size_t
    retire_completed_fills(uint64_t current_cycle);

    /**
     * @brief Computes the stall penalty in cycles until the earliest in-flight MSHR completes.
     *
     * @param current_cycle Current simulation cycle.
     * @return Cycles until the next MSHR entry becomes available.
     */
    uint64_t
    get_cycles_until_next_available(uint64_t current_cycle) const;

    /**
     * @brief Flushes and completes all in-flight MSHRs, advancing simulation cycle.
     *
     * @param current_cycle Reference to simulation cycle counter (advanced to latest ready cycle).
     */
    void
    drain_all(uint64_t &current_cycle);

    // Queries.
    bool is_full() const { return active_count_ >= capacity_; }
    bool is_empty() const { return active_count_ == 0; }
    size_t occupancy() const { return active_count_; }
    size_t capacity() const { return capacity_; }
    const mshr_stats_t &get_stats() const { return stats_; }
    void reset_stats() { stats_ = mshr_stats_t(); }

private:
    // Computes aligned cache line base address.
    uint64_t get_line_addr(uint64_t addr) const {
        return addr & ~(line_size_ - 1);
    }

    size_t capacity_;                                // Maximum in-flight MSHR entries.
    uint64_t line_size_;                             // Cache block line size in bytes.
    size_t active_count_ = 0;                        // Current number of allocated entries.
    std::vector<mshr_entry_t> entries_;              // Pool of pre-allocated MSHR entries.
    std::unordered_map<uint64_t, size_t> addr_map_;  // Map: line_addr -> entries_ index.
    mshr_stats_t stats_;                             // Accumulated MSHR metrics.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_MSHR_H_ */
