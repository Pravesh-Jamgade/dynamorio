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

/* reuse_distance_tracker.h: Exact LRU stack and reuse distance tracking
 * for memory caches and TLBs using Bennett-Kruskal Fenwick Trees.
 */

#ifndef _DRMEMTRACE_REUSE_DISTANCE_TRACKER_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_REUSE_DISTANCE_TRACKER_H_ 1 // Define inclusion guard macro.

#include <stdint.h>        // Fixed-width integer types.
#include <stddef.h>        // size_t definition.
#include <algorithm>       // std::sort, std::min, std::fill.
#include <unordered_map>   // std::unordered_map for block tag to timestamp mapping.
#include <vector>          // std::vector for Fenwick tree array and compaction.

#include "memref.h"        // Provide addr_t definition.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Exact LRU Stack / Reuse Distance Tracker using Bennett-Kruskal Fenwick Tree.
 *
 * Computes the exact reuse distance (number of distinct cache blocks / virtual pages
 * referenced between consecutive accesses to the same block) in O(log W) time.
 * Supports compacting timestamps to bound storage to ~4 MB while strictly preserving
 * exact LRU order and distances.
 */
class reuse_distance_tracker_t {
public:
    // Constructs reuse distance tracker with maximum timestamp window capacity before compaction.
    explicit reuse_distance_tracker_t(uint32_t capacity_limit = 1048576)
        : max_time_(capacity_limit)                          // Store maximum timestamp capacity.
        , current_time_(1)                                   // Monotonically increasing time starts at 1.
        , bit_(capacity_limit + 4, 0)                        // Zero-initialize 1-based Fenwick tree array.
    {
    }

    // Default destructor.
    virtual ~reuse_distance_tracker_t() = default;

    /**
     * @brief Records access to a block (cache line tag or virtual page number).
     *
     * @param block_addr Block identifier (e.g. addr >> block_size_bits).
     * @return -1 if this is the first access to block_addr (compulsory / cold miss).
     *         Otherwise, returns the exact reuse distance >= 0.
     */
    int64_t
    record_access(addr_t block_addr)
    {
        // Query hash map for previous access timestamp of this block.
        auto it = last_time_.find(block_addr);
        // Check if block has never been accessed before.
        if (it == last_time_.end()) {
            // Trigger compaction if timestamp buffer is nearly exhausted.
            if (current_time_ >= max_time_ - 2) {
                compact();
            }
            // Allocate new monotonically increasing timestamp.
            uint32_t t = current_time_++;
            // Insert block mapping with current timestamp.
            last_time_[block_addr] = t;
            // Add point weight of +1 at position t in the Fenwick tree.
            bit_add(t, 1);
            // Return -1 sentinel denoting compulsory / cold access (infinite reuse distance).
            return -1;
        }

        // Retrieve prior access timestamp for this block.
        uint32_t prev_t = it->second;
        // Compute exact count of distinct blocks accessed between prev_t and current_time_:
        // sum(prev_t + 1 .. current_time_ - 1) = bit_query(current_time_ - 1) - bit_query(prev_t).
        int64_t dist = bit_query(current_time_ - 1) - bit_query(prev_t);
        // Clamp to 0 if underflow occurs due to boundary conditions.
        if (dist < 0) {
            dist = 0;
        }

        // Remove old timestamp point weight from the Fenwick tree.
        bit_add(prev_t, -1);

        // Trigger compaction if timestamp buffer is nearly exhausted.
        if (current_time_ >= max_time_ - 2) {
            compact();
        }

        // Allocate new monotonically increasing timestamp for this access.
        uint32_t t = current_time_++;
        // Update hash map with newly assigned timestamp.
        it->second = t;
        // Add point weight of +1 at position t in the Fenwick tree.
        bit_add(t, 1);

        // Return exact computed reuse distance.
        return dist;
    }

    /**
     * @brief Resets all tracking state and clears the Fenwick tree.
     */
    void
    reset()
    {
        // Clear all block-to-timestamp mappings.
        last_time_.clear();
        // Zero all elements of the Fenwick tree.
        std::fill(bit_.begin(), bit_.end(), 0);
        // Reset timestamp counter back to 1.
        current_time_ = 1;
    }

    /**
     * @brief Returns total number of distinct blocks observed so far.
     */
    size_t
    unique_blocks() const
    {
        // Return size of the block timestamp hash map.
        return last_time_.size();
    }

private:
    // Adds delta value to 1-based index idx in Fenwick tree.
    void
    bit_add(uint32_t idx, int delta)
    {
        // Traverse ancestors in binary indexed tree by adding lowest set bit.
        for (uint32_t i = idx; i < bit_.size(); i += (i & -i)) {
            bit_[i] += delta; // Update node value with delta.
        }
    }

    // Computes prefix sum [1 .. idx] in Fenwick tree.
    int64_t
    bit_query(uint32_t idx) const
    {
        int64_t sum = 0; // Initialize prefix sum.
        // Clamp query index to tree boundary.
        uint32_t i = std::min(idx, static_cast<uint32_t>(bit_.size() - 1));
        // Traverse tree upward by subtracting lowest set bit.
        for (; i > 0; i -= (i & -i)) {
            sum += bit_[i]; // Accumulate tree node value.
        }
        return sum; // Return accumulated prefix sum.
    }

    // Compacts active timestamps into a dense 1..K range, preserving exact relative LRU ordering.
    void
    compact()
    {
        // If no active blocks exist, reset tree and return.
        if (last_time_.empty()) {
            std::fill(bit_.begin(), bit_.end(), 0); // Zero Fenwick tree.
            current_time_ = 1;                     // Reset time counter.
            return;
        }

        // Collect all currently active (timestamp, block_addr) pairs.
        std::vector<std::pair<uint32_t, addr_t>> active;
        active.reserve(last_time_.size()); // Reserve space to avoid reallocations.
        for (const auto &kv : last_time_) {
            active.push_back({kv.second, kv.first}); // Push timestamp and block address.
        }

        // Sort by timestamp in ascending order to strictly maintain chronological LRU sequence.
        std::sort(active.begin(), active.end());

        // Zero-fill Fenwick tree for rebuild.
        std::fill(bit_.begin(), bit_.end(), 0);

        // Assign dense consecutive timestamps starting at 1.
        uint32_t new_t = 1;
        for (const auto &entry : active) {
            last_time_[entry.second] = new_t; // Update block's timestamp in hash map.
            bit_add(new_t, 1);                // Add point weight of 1 at new position in tree.
            new_t++;                          // Advance dense timestamp.
        }

        // Set current_time_ to point directly after last assigned dense timestamp.
        current_time_ = new_t;
    }

    uint32_t max_time_;                                 // Maximum timestamp capacity limit.
    uint32_t current_time_;                             // Current monotonically increasing timestamp.
    std::vector<int32_t> bit_;                          // 1-based Fenwick Tree (Binary Indexed Tree) buffer.
    std::unordered_map<addr_t, uint32_t> last_time_;    // Hash map from block tag to most recent timestamp.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_REUSE_DISTANCE_TRACKER_H_ */
