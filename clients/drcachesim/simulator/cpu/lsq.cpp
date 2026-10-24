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
#include "cpu/lsq.h"

// Provide formatting manipulators.
#include <iomanip>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

lsq_t::lsq_t(size_t lq_capacity, size_t sq_capacity)
    // Initialize capacities.
    : lq_capacity_(lq_capacity)
    , sq_capacity_(sq_capacity)
{
    // Constructor body.
}

bool
lsq_t::can_dispatch_load() const
{
    // Load Queue can accept an entry if occupancy is strictly below capacity.
    return lq_entries_.size() < lq_capacity_;
}

bool
lsq_t::can_dispatch_store() const
{
    // Store Queue can accept an entry if occupancy is strictly below capacity.
    return sq_entries_.size() < sq_capacity_;
}

bool
lsq_t::dispatch_load(const dyn_instruction_t &instr)
{
    // Check for Load Queue full condition.
    if (!can_dispatch_load()) {
        ++lq_full_stalls_; // Record backpressure stall.
        return false;
    }

    // Allocate entry in Load Queue.
    lq_entry_t entry;
    entry.instr = instr;
    entry.address_resolved = true;
    entry.forwarded_from_sq = false;
    entry.data_ready = false;

    // Push into the LQ.
    lq_entries_.push_back(entry);
    ++total_loads_dispatched_;
    return true;
}

bool
lsq_t::dispatch_store(const dyn_instruction_t &instr)
{
    // Check for Store Queue full condition.
    if (!can_dispatch_store()) {
        ++sq_full_stalls_; // Record backpressure stall.
        return false;
    }

    // Allocate entry in Store Queue.
    sq_entry_t entry;
    entry.instr = instr;
    entry.address_resolved = true;
    entry.data_ready = true;
    entry.committed = false;

    // Push into the SQ.
    sq_entries_.push_back(entry);
    ++total_stores_dispatched_;
    return true;
}

bool
lsq_t::check_store_forwarding(uint64_t load_seq_id, addr_t load_addr, size_t load_size,
                              uint64_t &forwarding_store_seq_id)
{
    // Search the Store Queue backwards (from youngest store to oldest store)
    // to find the most recent preceding store in program order.
    for (auto it = sq_entries_.rbegin(); it != sq_entries_.rend(); ++it) {
        // Only inspect stores that precede this load in program order.
        if (it->instr.seq_id < load_seq_id) {
            // Check for exact address match or enclosed store address range.
            if (it->instr.mem_addr == load_addr && it->instr.mem_size >= load_size) {
                // Store-to-Load Forwarding hit!
                forwarding_store_seq_id = it->instr.seq_id;
                ++store_forwardings_;
                return true;
            }
        }
    }
    // No matching preceding store found; load must be serviced by the cache hierarchy.
    return false;
}

void
lsq_t::commit_store(uint64_t store_seq_id)
{
    // Locate the store in the SQ and mark it committed.
    for (auto &entry : sq_entries_) {
        if (entry.instr.seq_id == store_seq_id) {
            entry.committed = true;
            break;
        }
    }
}

size_t
lsq_t::drain_stores(size_t drain_width,
                    std::vector<dyn_instruction_t> &drained_stores_out)
{
    // Track count of stores drained to L1D cache in this cycle.
    size_t count = 0;

    // Drain committed stores from the head of the SQ in strict program order.
    while (count < drain_width && !sq_entries_.empty()) {
        auto &head = sq_entries_.front();
        // Stores can only drain to cache once committed by the ROB!
        if (!head.committed) {
            break; // Stop draining once an uncommitted store is reached.
        }

        // Add to output drained list.
        drained_stores_out.push_back(head.instr);
        // Pop from head of the SQ.
        sq_entries_.pop_front();
        ++count;
    }

    // Return number of stores drained.
    return count;
}

void
lsq_t::commit_load(uint64_t load_seq_id)
{
    // Remove the retiring load from the head of the LQ if matched.
    if (!lq_entries_.empty() && lq_entries_.front().instr.seq_id == load_seq_id) {
        lq_entries_.pop_front();
    } else {
        // Search and remove if out-of-order retirement within queue.
        for (auto it = lq_entries_.begin(); it != lq_entries_.end(); ++it) {
            if (it->instr.seq_id == load_seq_id) {
                lq_entries_.erase(it);
                break;
            }
        }
    }
}

void
lsq_t::squash_after(uint64_t squashed_seq_id)
{
    // Flush speculative loads younger than squashed_seq_id from the back.
    while (!lq_entries_.empty() && lq_entries_.back().instr.seq_id > squashed_seq_id) {
        lq_entries_.pop_back();
    }
    // Flush uncommitted speculative stores younger than squashed_seq_id from the back.
    while (!sq_entries_.empty() && sq_entries_.back().instr.seq_id > squashed_seq_id &&
           !sq_entries_.back().committed) {
        sq_entries_.pop_back();
    }
}

void
lsq_t::print_stats(std::ostream &os, const std::string &prefix) const
{
    // Print formatted Load-Store Queue statistics.
    os << prefix << "Load-Store Queue (LSQ) stats:\n";
    os << prefix << "  Total loads dispatched:        " << std::setw(12)
       << total_loads_dispatched_ << "\n";
    os << prefix << "  Total stores dispatched:       " << std::setw(12)
       << total_stores_dispatched_ << "\n";
    os << prefix << "  Store-to-Load forwardings:     " << std::setw(12)
       << store_forwardings_ << "\n";
    os << prefix << "  LQ full dispatch stalls:       " << std::setw(12)
       << lq_full_stalls_ << "\n";
    os << prefix << "  SQ full dispatch stalls:       " << std::setw(12)
       << sq_full_stalls_ << "\n";
}

void
lsq_t::reset()
{
    // Clear all entries and reset counters.
    lq_entries_.clear();
    sq_entries_.clear();
    total_loads_dispatched_ = 0;
    total_stores_dispatched_ = 0;
    store_forwardings_ = 0;
    lq_full_stalls_ = 0;
    sq_full_stalls_ = 0;
}

} // namespace drmemtrace
} // namespace dynamorio
