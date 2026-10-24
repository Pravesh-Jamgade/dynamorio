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
#include "cpu/mshr.h"
// Provide limits header.
#include <limits>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

mshr_table_t::mshr_table_t(size_t capacity, uint64_t line_size)
    // Store capacity limit.
    : capacity_(capacity)
    // Store cache line size in bytes.
    , line_size_(line_size)
    // Pre-allocate entry slots.
    , entries_(capacity)
{
    // Constructor body.
}

mshr_entry_t *
mshr_table_t::lookup(uint64_t addr)
{
    // Compute aligned line base address.
    uint64_t line = get_line_addr(addr);
    // Find entry in address lookup map.
    auto it = addr_map_.find(line);
    // Check if an active entry was found.
    if (it != addr_map_.end()) {
        // Return pointer to corresponding MSHR entry.
        return &entries_[it->second];
    }
    // No matching in-flight entry found.
    return nullptr;
}

const mshr_entry_t *
mshr_table_t::lookup(uint64_t addr) const
{
    // Compute aligned line base address.
    uint64_t line = get_line_addr(addr);
    // Find entry in address lookup map.
    auto it = addr_map_.find(line);
    // Check if an active entry was found.
    if (it != addr_map_.end()) {
        // Return const pointer to corresponding MSHR entry.
        return &entries_[it->second];
    }
    // No matching in-flight entry found.
    return nullptr;
}

mshr_entry_t *
mshr_table_t::allocate(uint64_t addr,
                       uint64_t current_cycle,
                       uint64_t latency_cycles,
                       cpu_op_type_t op_type,
                       uint64_t ref_id)
{
    // Check if MSHR table is currently at maximum capacity.
    if (active_count_ >= capacity_) {
        // Increment structural full stall counter.
        stats_.full_stalls++;
        // Cannot allocate new MSHR entry.
        return nullptr;
    }

    // Align address to cache line boundary.
    uint64_t line = get_line_addr(addr);

    // Find the first free entry slot.
    for (size_t i = 0; i < entries_.size(); ++i) {
        // Check if slot is inactive.
        if (!entries_[i].valid) {
            // Mark slot as active.
            entries_[i].valid = true;
            // Record cache line base address.
            entries_[i].line_addr = line;
            // Record allocation cycle.
            entries_[i].alloc_cycle = current_cycle;
            // Calculate ready cycle when line fill returns.
            entries_[i].ready_cycle = current_cycle + latency_cycles;
            // Record triggering operation type.
            entries_[i].op_type = op_type;
            // Record primary reference ID.
            entries_[i].primary_id = ref_id;
            // Reset secondary references list.
            entries_[i].secondary_ids.clear();

            // Insert into fast lookup map.
            addr_map_[line] = i;
            // Increment active entry count.
            active_count_++;
            // Increment total allocation counter.
            stats_.total_allocations++;
            // Update peak occupancy metric.
            stats_.peak_occupancy = std::max(stats_.peak_occupancy, active_count_);

            // Return pointer to freshly allocated entry.
            return &entries_[i];
        }
    }

    // Should not reach here if active_count_ < capacity_.
    return nullptr;
}

void
mshr_table_t::coalesce_secondary(mshr_entry_t *entry, uint64_t ref_id)
{
    // Verify entry is valid.
    if (entry != nullptr && entry->valid) {
        // Append secondary reference ID to list.
        entry->secondary_ids.push_back(ref_id);
        // Increment secondary merge statistic.
        stats_.secondary_merges++;
    }
}

size_t
mshr_table_t::retire_completed_fills(uint64_t current_cycle)
{
    // Track count of completed entries.
    size_t retired = 0;

    // Iterate through all MSHR entry slots.
    for (size_t i = 0; i < entries_.size(); ++i) {
        // Check if entry is active and line fill has arrived.
        if (entries_[i].valid && entries_[i].ready_cycle <= current_cycle) {
            // Remove entry from address lookup map.
            addr_map_.erase(entries_[i].line_addr);
            // Mark slot as free.
            entries_[i].valid = false;
            // Decrement active entry count.
            active_count_--;
            // Increment retired count.
            retired++;
        }
    }

    // Return count of retired entries.
    return retired;
}

uint64_t
mshr_table_t::get_cycles_until_next_available(uint64_t current_cycle) const
{
    // If not full, an entry is immediately available.
    if (active_count_ < capacity_) {
        // No stall needed.
        return 0;
    }

    // Find the earliest ready cycle among all in-flight entries.
    uint64_t earliest_ready = std::numeric_limits<uint64_t>::max();
    // Scan all active entries.
    for (const auto &entry : entries_) {
        // Check if entry is valid.
        if (entry.valid) {
            // Track minimum ready cycle.
            earliest_ready = std::min(earliest_ready, entry.ready_cycle);
        }
    }

    // Compute cycles to wait.
    if (earliest_ready > current_cycle) {
        // Return remaining cycles.
        return earliest_ready - current_cycle;
    }

    // Earliest ready has already completed.
    return 1;
}

void
mshr_table_t::drain_all(uint64_t &current_cycle)
{
    // Find the latest ready cycle among all active entries.
    uint64_t latest_ready = current_cycle;
    // Scan all active entries.
    for (const auto &entry : entries_) {
        // Check if entry is valid.
        if (entry.valid) {
            // Track maximum ready cycle.
            latest_ready = std::max(latest_ready, entry.ready_cycle);
        }
    }

    // Advance simulation time to latest ready cycle.
    current_cycle = latest_ready;
    // Clear all entries.
    for (auto &entry : entries_) {
        // Invalidate entry.
        entry.valid = false;
        // Clear secondary references.
        entry.secondary_ids.clear();
    }
    // Clear address map.
    addr_map_.clear();
    // Reset active count.
    active_count_ = 0;
}

} // namespace drmemtrace
} // namespace dynamorio
