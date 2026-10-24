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
#ifndef _DRMEMTRACE_CACHE_QUEUES_H_
#define _DRMEMTRACE_CACHE_QUEUES_H_ 1

// Provide standard integer types.
#include <stdint.h>
// Provide size_t definition.
#include <stddef.h>
// Provide deque container for FIFO queue buffering.
#include <deque>
// Provide vector container.
#include <vector>
// Provide algorithm header for min/max.
#include <algorithm>

// Provide CPU operation types.
#include "cpu/cpu_types.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Represents a single memory access request inside a cache queue.
 */
struct cache_queue_entry_t {
    uint64_t addr = 0;                   // Byte address of the request.
    uint64_t line_addr = 0;              // Aligned cache line address.
    size_t size = 0;                     // Access size in bytes.
    uint64_t enter_cycle = 0;            // Cycle when request entered the queue.
    cpu_op_type_t op_type = cpu_op_type_t::OP_LOAD; // Operation type (read, write, prefetch, etc.).
    uint64_t ref_id = 0;                 // Unique reference/instruction sequence ID.
    bool coalesced = false;              // Whether this entry was merged into an existing write.
};

/**
 * @brief Read Queue (RQ) buffering incoming demand read, instruction fetch,
 * and page table walker requests.
 */
class read_queue_t {
public:
    /**
     * @brief Constructs a Read Queue with specified capacity and line size.
     *
     * @param capacity Maximum number of queue entries (default: 32).
     * @param line_size Cache line size in bytes (default: 64).
     */
    explicit read_queue_t(size_t capacity = 32, uint64_t line_size = 64);

    /**
     * @brief Attempts to enqueue a read request.
     *
     * @param req The cache queue request record.
     * @param cycle Current simulation cycle.
     * @return True if enqueued successfully; false if queue is full.
     */
    bool
    enqueue(const cache_queue_entry_t &req, uint64_t cycle);

    /**
     * @brief Dequeues the front read request for processing.
     *
     * @param req Output parameter receiving the dequeued request.
     * @return True if a request was dequeued; false if queue is empty.
     */
    bool
    dequeue(cache_queue_entry_t &req);

    // Queries.
    bool is_full() const { return queue_.size() >= capacity_; }
    bool is_empty() const { return queue_.empty(); }
    size_t size() const { return queue_.size(); }
    size_t capacity() const { return capacity_; }
    uint64_t total_enqueued() const { return total_enqueued_; }
    uint64_t full_stalls() const { return full_stalls_; }
    size_t peak_occupancy() const { return peak_occupancy_; }

private:
    size_t capacity_;                                // Queue capacity limit.
    uint64_t line_size_;                             // Cache line size in bytes.
    std::deque<cache_queue_entry_t> queue_;          // FIFO container.
    uint64_t total_enqueued_ = 0;                    // Cumulative enqueue count.
    uint64_t full_stalls_ = 0;                       // Rejected requests due to full queue.
    size_t peak_occupancy_ = 0;                      // Peak queue occupancy observed.
};

/**
 * @brief Write Queue (WQ) buffering stores and dirty line writebacks.
 * Supports write-buffer coalescing for multiple stores targeting the same 64-byte line.
 */
class write_queue_t {
public:
    /**
     * @brief Constructs a Write Queue with specified capacity and line size.
     *
     * @param capacity Maximum number of queue entries (default: 32).
     * @param line_size Cache line size in bytes (default: 64).
     */
    explicit write_queue_t(size_t capacity = 32, uint64_t line_size = 64);

    /**
     * @brief Enqueues a write request, performing write-coalescing if possible.
     *
     * @param req The write request record.
     * @param cycle Current simulation cycle.
     * @return True if enqueued or coalesced; false if queue is full.
     */
    bool
    enqueue(const cache_queue_entry_t &req, uint64_t cycle);

    /**
     * @brief Dequeues the oldest write request for draining.
     *
     * @param req Output parameter receiving the dequeued request.
     * @return True if dequeued; false if queue is empty.
     */
    bool
    dequeue(cache_queue_entry_t &req);

    /**
     * @brief Checks if a pending write in this queue targets the same cache line
     * (Read-After-Write hazard detection).
     *
     * @param addr Memory address being read.
     * @return True if an in-flight write matches the line address.
     */
    bool
    has_raw_hazard(uint64_t addr) const;

    // Queries.
    bool is_full() const { return queue_.size() >= capacity_; }
    bool is_empty() const { return queue_.empty(); }
    size_t size() const { return queue_.size(); }
    size_t capacity() const { return capacity_; }
    uint64_t total_enqueued() const { return total_enqueued_; }
    uint64_t coalesced_writes() const { return coalesced_writes_; }
    uint64_t full_stalls() const { return full_stalls_; }
    size_t peak_occupancy() const { return peak_occupancy_; }

private:
    uint64_t get_line_addr(uint64_t addr) const {
        return addr & ~(line_size_ - 1);
    }

    size_t capacity_;                                // Queue capacity limit.
    uint64_t line_size_;                             // Cache line size in bytes.
    std::deque<cache_queue_entry_t> queue_;          // FIFO container.
    uint64_t total_enqueued_ = 0;                    // Cumulative write count.
    uint64_t coalesced_writes_ = 0;                  // Writes merged into an existing line entry.
    uint64_t full_stalls_ = 0;                       // Rejected writes due to full queue.
    size_t peak_occupancy_ = 0;                      // Peak queue occupancy observed.
};

/**
 * @brief Prefetch Queue (PQ) buffering speculative hardware prefetches.
 * Operates at lower priority than demand reads and writes.
 */
class prefetch_queue_t {
public:
    /**
     * @brief Constructs a Prefetch Queue.
     *
     * @param capacity Maximum number of queue entries (default: 32).
     * @param line_size Cache line size in bytes (default: 64).
     */
    explicit prefetch_queue_t(size_t capacity = 32, uint64_t line_size = 64);

    /**
     * @brief Enqueues a prefetch request. Drops silently if queue is full.
     *
     * @param req The prefetch request record.
     * @param cycle Current simulation cycle.
     * @return True if enqueued; false if dropped due to queue saturation.
     */
    bool
    enqueue(const cache_queue_entry_t &req, uint64_t cycle);

    /**
     * @brief Dequeues a prefetch request when bus/cache is idle.
     *
     * @param req Output parameter receiving the dequeued request.
     * @return True if dequeued; false if queue is empty.
     */
    bool
    dequeue(cache_queue_entry_t &req);

    // Queries.
    bool is_full() const { return queue_.size() >= capacity_; }
    bool is_empty() const { return queue_.empty(); }
    size_t size() const { return queue_.size(); }
    size_t capacity() const { return capacity_; }
    uint64_t total_enqueued() const { return total_enqueued_; }
    uint64_t dropped_prefetches() const { return dropped_prefetches_; }
    size_t peak_occupancy() const { return peak_occupancy_; }

private:
    size_t capacity_;                                // Queue capacity limit.
    uint64_t line_size_;                             // Cache line size in bytes.
    std::deque<cache_queue_entry_t> queue_;          // FIFO container.
    uint64_t total_enqueued_ = 0;                    // Cumulative prefetches enqueued.
    uint64_t dropped_prefetches_ = 0;                // Prefetches discarded due to queue overflow.
    size_t peak_occupancy_ = 0;                      // Peak queue occupancy observed.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_CACHE_QUEUES_H_ */
