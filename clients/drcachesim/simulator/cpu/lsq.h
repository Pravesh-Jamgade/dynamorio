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

/* lsq.h: Load-Store Queue (LSQ) consisting of Load Queue (LQ) and Store Queue (SQ),
 * supporting store-to-load forwarding, memory disambiguation, and store buffering
 * until in-order commit.
 */

#ifndef _DRMEMTRACE_LSQ_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_LSQ_H_ 1 // Define inclusion guard macro.

// Provide standard integer types.
#include <stdint.h>
// Provide dynamic array and deque containers.
#include <deque>
#include <vector>
// Provide standard string class.
#include <string>
// Provide standard output streams.
#include <iostream>

// Provide CPU types and dynamic instruction structures.
#include "cpu/cpu_types.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Entry in the Load Queue (LQ).
 */
struct lq_entry_t {
    dyn_instruction_t instr;          // The dynamic load instruction.
    bool address_resolved = false;    // True when address generation unit (AGU) completed.
    bool forwarded_from_sq = false;   // True if data was satisfied by store-forwarding.
    bool data_ready = false;          // True when data returned from forwarding or cache.
};

/**
 * @brief Entry in the Store Queue (SQ).
 */
struct sq_entry_t {
    dyn_instruction_t instr;          // The dynamic store instruction.
    bool address_resolved = false;    // True when AGU completed.
    bool data_ready = false;          // True when store data is available.
    bool committed = false;           // True when retired from ROB; ready to drain to L1D.
};

/**
 * @brief Load-Store Queue (LSQ) coordinating memory accesses, forwarding, and commit writes.
 */
class lsq_t {
public:
    /**
     * @brief Constructs an LSQ with specified capacities.
     *
     * @param lq_capacity Maximum Load Queue entries (default 72).
     * @param sq_capacity Maximum Store Queue entries (default 56).
     */
    explicit lsq_t(size_t lq_capacity = 72, size_t sq_capacity = 56);

    /**
     * @brief Checks if the Load Queue has room for a new load instruction.
     */
    bool
    can_dispatch_load() const;

    /**
     * @brief Checks if the Store Queue has room for a new store instruction.
     */
    bool
    can_dispatch_store() const;

    /**
     * @brief Dispatches a load instruction into the Load Queue.
     *
     * @param instr Load instruction to dispatch.
     * @return True if dispatched; false if LQ full.
     */
    bool
    dispatch_load(const dyn_instruction_t &instr);

    /**
     * @brief Dispatches a store instruction into the Store Queue.
     *
     * @param instr Store instruction to dispatch.
     * @return True if dispatched; false if SQ full.
     */
    bool
    dispatch_store(const dyn_instruction_t &instr);

    /**
     * @brief Probes the Store Queue for preceding stores matching `load_addr` to perform
     *        Store-to-Load Forwarding.
     *
     * @param load_seq_id Sequence ID of the load instruction.
     * @param load_addr   Virtual/physical address requested by the load.
     * @param load_size   Access width in bytes.
     * @param forwarding_store_seq_id Output seq_id of the forwarding store if matched.
     * @return True if an in-flight store forwarded data to this load; false if cache read needed.
     */
    bool
    check_store_forwarding(uint64_t load_seq_id, addr_t load_addr, size_t load_size,
                           uint64_t &forwarding_store_seq_id);

    /**
     * @brief Marks an in-flight store as committed by the ROB, ready to drain into L1D cache.
     *
     * @param store_seq_id Sequence ID of the committing store.
     */
    void
    commit_store(uint64_t store_seq_id);

    /**
     * @brief Drains up to `drain_width` committed stores from the SQ head to the L1D cache.
     *
     * @param drain_width Maximum stores written to cache per cycle.
     * @param drained_stores_out Vector receiving drained store instructions.
     * @return Number of stores drained to cache in this cycle.
     */
    size_t
    drain_stores(size_t drain_width, std::vector<dyn_instruction_t> &drained_stores_out);

    /**
     * @brief Retires a load instruction from the LQ upon ROB commit.
     *
     * @param load_seq_id Sequence ID of the retiring load.
     */
    void
    commit_load(uint64_t load_seq_id);

    /**
     * @brief Flushes all speculative loads and stores younger than `squashed_seq_id`.
     *
     * @param squashed_seq_id Sequence ID of the mispredicted branch.
     */
    void
    squash_after(uint64_t squashed_seq_id);

    /**
     * @brief Prints formatted LSQ statistics (loads, stores, forwardings, buffer stalls).
     *
     * @param os     Output stream (e.g. std::cerr).
     * @param prefix Line indentation prefix.
     */
    void
    print_stats(std::ostream &os, const std::string &prefix = "") const;

    /**
     * @brief Resets the LSQ and clears all entries and statistics.
     */
    void
    reset();

    // Query methods.
    size_t lq_size() const { return lq_entries_.size(); }
    size_t sq_size() const { return sq_entries_.size(); }
    int64_t store_forwardings() const { return store_forwardings_; }

private:
    size_t lq_capacity_;                 // Maximum entries in the Load Queue.
    size_t sq_capacity_;                 // Maximum entries in the Store Queue.
    std::deque<lq_entry_t> lq_entries_;  // FIFO Load Queue.
    std::deque<sq_entry_t> sq_entries_;  // FIFO Store Queue.

    // Statistics counters.
    int64_t total_loads_dispatched_ = 0;   // Total load instructions entered into LQ.
    int64_t total_stores_dispatched_ = 0;  // Total store instructions entered into SQ.
    int64_t store_forwardings_ = 0;        // Count of successful store-to-load forwardings.
    int64_t lq_full_stalls_ = 0;           // Dispatch stalls caused by full LQ.
    int64_t sq_full_stalls_ = 0;           // Dispatch stalls caused by full SQ.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_LSQ_H_ */
