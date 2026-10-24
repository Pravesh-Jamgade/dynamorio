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

/* rob.h: Reorder Buffer (ROB) tracking in-flight instructions, enforcing in-order
 * retirement, modeling head-of-line stalls on cache/TLB misses, and supporting
 * branch misprediction squashes.
 */

#ifndef _DRMEMTRACE_ROB_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_ROB_H_ 1 // Define inclusion guard macro.

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
 * @brief Represents a single slot in the Reorder Buffer (ROB).
 */
struct rob_entry_t {
    dyn_instruction_t instr;          // The in-flight dynamic instruction.
    bool ready_for_commit = false;    // Set true when execution and memory complete.
    bool head_stalled = false;        // Set true if this instruction stalled the ROB head.
};

/**
 * @brief Reorder Buffer (ROB) maintaining program order and in-order retirement.
 */
class rob_t {
public:
    /**
     * @brief Constructs a Reorder Buffer with a specified maximum entry capacity.
     *
     * @param capacity Maximum number of in-flight instructions allowed (default 256).
     */
    explicit rob_t(size_t capacity = 256);

    /**
     * @brief Checks whether the ROB has room to dispatch another instruction.
     *
     * @return True if not full; false if ROB full.
     */
    bool
    can_dispatch() const;

    /**
     * @brief Dispatches an instruction into the tail of the ROB in program order.
     *
     * @param instr Dynamic instruction being dispatched.
     * @return True if dispatch succeeded; false if ROB was full.
     */
    bool
    dispatch(const dyn_instruction_t &instr);

    /**
     * @brief Marks an in-flight instruction as completed (ready to retire).
     *
     * @param seq_id      Unique sequence ID of the completing instruction.
     * @param ready_cycle Cycle when computation or memory access finished.
     * @return True if instruction was found in the ROB; false otherwise.
     */
    bool
    mark_ready(uint64_t seq_id, uint64_t ready_cycle);

    /**
     * @brief Retires up to `commit_width` completed instructions from the head of the ROB.
     *
     * Stops immediately if the instruction at the head of the ROB is not yet ready,
     * faithfully modeling head-of-line blocking stalls (e.g., long-latency cache/TLB misses).
     *
     * @param commit_width Maximum instructions that can commit per cycle.
     * @param current_cycle Current simulation clock cycle.
     * @param committed_out Vector receiving instructions retired in this cycle.
     * @return Number of instructions retired in this cycle.
     */
    size_t
    commit(size_t commit_width, uint64_t current_cycle,
           std::vector<dyn_instruction_t> &committed_out);

    /**
     * @brief Flushes all instructions younger than `squashed_seq_id` (e.g. after a branch
     *        misprediction).
     *
     * @param squashed_seq_id Sequence ID of the branch that mispredicted.
     * @return Number of speculative instructions squashed from the ROB.
     */
    size_t
    squash_after(uint64_t squashed_seq_id);

    /**
     * @brief Prints ROB statistics (dispatches, commits, full stalls, head-of-line stalls).
     *
     * @param os     Output stream (e.g. std::cerr).
     * @param prefix Line indentation prefix.
     */
    void
    print_stats(std::ostream &os, const std::string &prefix = "") const;

    /**
     * @brief Resets the ROB and clears all entries and statistics.
     */
    void
    reset();

    // Query methods.
    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }
    size_t capacity() const { return capacity_; }
    int64_t total_commits() const { return total_commits_; }
    int64_t head_of_line_stalls() const { return head_of_line_stalls_; }
    int64_t rob_full_stalls() const { return rob_full_stalls_; }

private:
    size_t capacity_;                 // Maximum entries allowed in the ROB.
    std::deque<rob_entry_t> entries_; // Double-ended queue for in-order FIFO tracking.

    // Statistics counters.
    int64_t total_dispatches_ = 0;     // Total instructions dispatched into ROB.
    int64_t total_commits_ = 0;        // Total instructions retired in program order.
    int64_t total_squashes_ = 0;       // Total speculative instructions discarded.
    int64_t head_of_line_stalls_ = 0;  // Cycles where commit stalled due to unready head.
    int64_t rob_full_stalls_ = 0;      // Cycles where dispatch stalled due to full ROB.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_ROB_H_ */
