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
#include "cpu/rob.h"

// Provide formatting manipulators.
#include <iomanip>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

rob_t::rob_t(size_t capacity)
    // Initialize ROB capacity.
    : capacity_(capacity)
{
    // Constructor body.
}

bool
rob_t::can_dispatch() const
{
    // ROB can accept a new instruction if current occupancy is below capacity.
    return entries_.size() < capacity_;
}

bool
rob_t::dispatch(const dyn_instruction_t &instr)
{
    // If the ROB is full, dispatch must stall.
    if (!can_dispatch()) {
        ++rob_full_stalls_; // Record dispatch backpressure stall.
        return false;
    }

    // Allocate an entry at the tail of the ROB in program order.
    rob_entry_t entry;
    entry.instr = instr;
    entry.instr.stage = pipeline_stage_t::STAGE_DISPATCH;
    entry.ready_for_commit = false;
    entry.head_stalled = false;

    // Push into the FIFO queue.
    entries_.push_back(entry);
    ++total_dispatches_;
    return true;
}

bool
rob_t::mark_ready(uint64_t seq_id, uint64_t ready_cycle)
{
    // Search the in-flight instructions in the ROB for matching seq_id.
    for (auto &entry : entries_) {
        if (entry.instr.seq_id == seq_id) {
            // Mark the instruction ready for retirement.
            entry.ready_for_commit = true;
            entry.instr.ready_cycle = ready_cycle;
            entry.instr.stage = pipeline_stage_t::STAGE_WRITEBACK;
            return true;
        }
    }
    return false; // Instruction not found in the ROB.
}

size_t
rob_t::commit(size_t commit_width, uint64_t current_cycle,
              std::vector<dyn_instruction_t> &committed_out)
{
    // Track count of instructions retired in this cycle.
    size_t count = 0;

    // Retire up to commit_width instructions in strict program order from the front.
    while (count < commit_width && !entries_.empty()) {
        // Inspect the instruction at the head of the ROB.
        auto &head = entries_.front();

        // If the instruction at the head is not yet ready, or ready at a future cycle:
        if (!head.ready_for_commit || head.instr.ready_cycle > current_cycle) {
            // Head-of-line blocking: in-order retirement cannot proceed past an unready instruction!
            head.head_stalled = true;
            ++head_of_line_stalls_; // Record head-of-line stall cycle.
            break;                  // Stop committing for this cycle.
        }

        // Instruction is completed; record commit time.
        head.instr.commit_cycle = current_cycle;
        head.instr.stage = pipeline_stage_t::STAGE_COMMIT;

        // Move the instruction to the retired output list.
        committed_out.push_back(head.instr);
        // Pop the retired instruction from the head of the ROB.
        entries_.pop_front();
        ++count;
        ++total_commits_;
    }

    // Return the number of instructions committed in this cycle.
    return count;
}

size_t
rob_t::squash_after(uint64_t squashed_seq_id)
{
    // Track count of squashed entries.
    size_t count = 0;

    // Remove all speculative instructions younger than squashed_seq_id from the back.
    while (!entries_.empty() && entries_.back().instr.seq_id > squashed_seq_id) {
        entries_.pop_back();
        ++count;
        ++total_squashes_;
    }

    // Return count of instructions squashed.
    return count;
}

void
rob_t::print_stats(std::ostream &os, const std::string &prefix) const
{
    // Print formatted Reorder Buffer statistics.
    os << prefix << "Reorder Buffer (ROB) stats:\n";
    os << prefix << "  Capacity:                      " << std::setw(12)
       << capacity_ << " entries\n";
    os << prefix << "  Total dispatches:              " << std::setw(12)
       << total_dispatches_ << "\n";
    os << prefix << "  Total commits:                 " << std::setw(12)
       << total_commits_ << "\n";
    os << prefix << "  Speculative squashes:          " << std::setw(12)
       << total_squashes_ << "\n";
    os << prefix << "  ROB full dispatch stalls:      " << std::setw(12)
       << rob_full_stalls_ << "\n";
    os << prefix << "  Head-of-line commit stalls:    " << std::setw(12)
       << head_of_line_stalls_ << "\n";
}

void
rob_t::reset()
{
    // Clear all in-flight entries and reset counters.
    entries_.clear();
    total_dispatches_ = 0;
    total_commits_ = 0;
    total_squashes_ = 0;
    head_of_line_stalls_ = 0;
    rob_full_stalls_ = 0;
}

} // namespace drmemtrace
} // namespace dynamorio
