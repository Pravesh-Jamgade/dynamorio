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
#include "cpu/cache_queues.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/* ========================================================================= */
/*                           READ QUEUE (RQ)                                 */
/* ========================================================================= */

read_queue_t::read_queue_t(size_t capacity, uint64_t line_size)
    // Store capacity limit.
    : capacity_(capacity)
    // Store line size in bytes.
    , line_size_(line_size)
{
    // Constructor body.
}

bool
read_queue_t::enqueue(const cache_queue_entry_t &req, uint64_t cycle)
{
    // Check if read queue is saturated.
    if (queue_.size() >= capacity_) {
        // Increment queue full stall count.
        full_stalls_++;
        // Reject enqueue.
        return false;
    }

    // Make local copy of request.
    cache_queue_entry_t entry = req;
    // Align address to cache line boundary.
    entry.line_addr = req.addr & ~(line_size_ - 1);
    // Record enqueue timestamp.
    entry.enter_cycle = cycle;

    // Push into FIFO queue.
    queue_.push_back(entry);
    // Increment total enqueue counter.
    total_enqueued_++;
    // Update peak occupancy metric.
    peak_occupancy_ = std::max(peak_occupancy_, queue_.size());

    // Enqueue successful.
    return true;
}

bool
read_queue_t::dequeue(cache_queue_entry_t &req)
{
    // Check if queue has no entries.
    if (queue_.empty()) {
        // Cannot dequeue from empty queue.
        return false;
    }

    // Retrieve head element.
    req = queue_.front();
    // Pop head element from container.
    queue_.pop_front();
    // Dequeue successful.
    return true;
}

/* ========================================================================= */
/*                           WRITE QUEUE (WQ)                                */
/* ========================================================================= */

write_queue_t::write_queue_t(size_t capacity, uint64_t line_size)
    // Store capacity limit.
    : capacity_(capacity)
    // Store line size in bytes.
    , line_size_(line_size)
{
    // Constructor body.
}

bool
write_queue_t::enqueue(const cache_queue_entry_t &req, uint64_t cycle)
{
    // Compute aligned cache line address.
    uint64_t target_line = req.addr & ~(line_size_ - 1);

    // Scan existing write queue entries for write-coalescing opportunity.
    for (auto &existing : queue_) {
        // Check if write targets the exact same 64-byte cache line.
        if (existing.line_addr == target_line) {
            // Coalesce write: update reference ID to younger store.
            existing.ref_id = req.ref_id;
            // Mark entry as coalesced.
            existing.coalesced = true;
            // Increment coalesced write statistic.
            coalesced_writes_++;
            // Increment total writes counter.
            total_enqueued_++;
            // Write was absorbed into existing entry without allocating new slot!
            return true;
        }
    }

    // Check if write queue is full.
    if (queue_.size() >= capacity_) {
        // Increment queue full stall count.
        full_stalls_++;
        // Reject write enqueue.
        return false;
    }

    // Make local copy of request.
    cache_queue_entry_t entry = req;
    // Set aligned line address.
    entry.line_addr = target_line;
    // Record enqueue cycle.
    entry.enter_cycle = cycle;

    // Push new entry to write queue.
    queue_.push_back(entry);
    // Increment total writes counter.
    total_enqueued_++;
    // Update peak occupancy metric.
    peak_occupancy_ = std::max(peak_occupancy_, queue_.size());

    // Enqueue successful.
    return true;
}

bool
write_queue_t::dequeue(cache_queue_entry_t &req)
{
    // Check if queue is empty.
    if (queue_.empty()) {
        // Cannot dequeue from empty queue.
        return false;
    }

    // Retrieve front element.
    req = queue_.front();
    // Pop front element.
    queue_.pop_front();
    // Dequeue successful.
    return true;
}

bool
write_queue_t::has_raw_hazard(uint64_t addr) const
{
    // Compute target line address.
    uint64_t line = addr & ~(line_size_ - 1);
    // Scan all in-flight writes.
    for (const auto &entry : queue_) {
        // Check if line address matches.
        if (entry.line_addr == line) {
            // RAW hazard detected: pending write in WQ.
            return true;
        }
    }
    // No matching pending write found.
    return false;
}

/* ========================================================================= */
/*                         PREFETCH QUEUE (PQ)                               */
/* ========================================================================= */

prefetch_queue_t::prefetch_queue_t(size_t capacity, uint64_t line_size)
    // Store capacity limit.
    : capacity_(capacity)
    // Store line size in bytes.
    , line_size_(line_size)
{
    // Constructor body.
}

bool
prefetch_queue_t::enqueue(const cache_queue_entry_t &req, uint64_t cycle)
{
    // Check if prefetch queue is full.
    if (queue_.size() >= capacity_) {
        // Prefetches are non-critical: drop silently on saturation.
        dropped_prefetches_++;
        // Indicate drop.
        return false;
    }

    // Make local copy of prefetch request.
    cache_queue_entry_t entry = req;
    // Align address to line boundary.
    entry.line_addr = req.addr & ~(line_size_ - 1);
    // Record cycle.
    entry.enter_cycle = cycle;

    // Push into prefetch queue.
    queue_.push_back(entry);
    // Increment total counter.
    total_enqueued_++;
    // Update peak occupancy metric.
    peak_occupancy_ = std::max(peak_occupancy_, queue_.size());

    // Enqueue successful.
    return true;
}

bool
prefetch_queue_t::dequeue(cache_queue_entry_t &req)
{
    // Check if queue is empty.
    if (queue_.empty()) {
        // Cannot dequeue.
        return false;
    }

    // Retrieve front element.
    req = queue_.front();
    // Pop front element.
    queue_.pop_front();
    // Dequeue successful.
    return true;
}

} // namespace drmemtrace
} // namespace dynamorio
