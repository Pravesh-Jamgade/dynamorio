/* **********************************************************
 * Copyright (c) 2015-2023 Google, Inc.  All rights reserved.
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

#include "caching_device_stats.h"

#include <assert.h>
#include <stdint.h>
#ifdef HAS_ZLIB
#    include <zlib.h>
#endif

#include <inttypes.h>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <string>
#include <utility>

#include "memref.h"
#include "options.h"
#include "caching_device.h"       // Provide caching_device_t definition for capacity query.
#include "caching_device_block.h"
#include "trace_entry.h"

namespace dynamorio {
namespace drmemtrace {

caching_device_stats_t::caching_device_stats_t(const std::string &miss_file,
                                               int block_size, bool warmup_enabled,
                                               bool is_coherent)
    : success_(true)
    , num_hits_(0)
    , num_misses_(0)
    , num_compulsory_misses_(0)
    , num_cold_misses_(0)
    , num_capacity_misses_(0)
    , num_conflict_misses_(0)
    , device_capacity_in_blocks_(0)
    , enable_histogram_(false)
    , reuse_histogram_(14, 0) // Initialize power-of-2 histogram with 14 bins (0..1024, >1024, inf).
    , reuse_tracker_()
    , block_size_bits_(compute_log2(block_size))
    , num_child_hits_(0)
    , num_inclusive_invalidates_(0)
    , num_coherence_invalidates_(0)
    , num_exclusive_invalidates_(0)
    , num_hits_at_reset_(0)
    , num_misses_at_reset_(0)
    , num_child_hits_at_reset_(0)
    , warmup_enabled_(warmup_enabled)
    , is_coherent_(is_coherent)
    , access_count_(block_size)
    , file_(nullptr)
    , caching_device_(nullptr)
{
    if (miss_file.empty()) {
        dump_misses_ = false;
    } else {
#ifdef HAS_ZLIB
        file_ = gzopen(miss_file.c_str(), "w");
#else
        file_ = fopen(miss_file.c_str(), "w");
#endif
        if (file_ == nullptr) {
            dump_misses_ = false;
            success_ = false;
        } else
            dump_misses_ = true;
    }

    stats_map_.emplace(metric_name_t::HITS, num_hits_);
    stats_map_.emplace(metric_name_t::MISSES, num_misses_);
    stats_map_.emplace(metric_name_t::HITS_AT_RESET, num_hits_at_reset_);
    stats_map_.emplace(metric_name_t::MISSES_AT_RESET, num_misses_at_reset_);
    stats_map_.emplace(metric_name_t::COMPULSORY_MISSES, num_compulsory_misses_);
    stats_map_.emplace(metric_name_t::COLD_MISSES, num_cold_misses_);
    stats_map_.emplace(metric_name_t::CAPACITY_MISSES, num_capacity_misses_);
    stats_map_.emplace(metric_name_t::CONFLICT_MISSES, num_conflict_misses_);
    stats_map_.emplace(metric_name_t::CHILD_HITS_AT_RESET, num_child_hits_at_reset_);
    stats_map_.emplace(metric_name_t::CHILD_HITS, num_child_hits_);
    stats_map_.emplace(metric_name_t::INCLUSIVE_INVALIDATES, num_inclusive_invalidates_);
    stats_map_.emplace(metric_name_t::COHERENCE_INVALIDATES, num_coherence_invalidates_);
    stats_map_.emplace(metric_name_t::EXCLUSIVE_INVALIDATES, num_exclusive_invalidates_);
}

caching_device_stats_t::~caching_device_stats_t()
{
    if (file_ != nullptr) {
#ifdef HAS_ZLIB
        gzclose(file_);
#else
        fclose(file_);
#endif
    }
}

void
caching_device_stats_t::access(const memref_t &memref, bool hit,
                               caching_device_block_t *cache_block)
{
    // Compute block address from memory reference address and block size bits.
    addr_t block_addr = (block_size_bits_ >= 0) ? (memref.data.addr >> block_size_bits_) : memref.data.addr;
    // Query exact LRU reuse distance from tracker.
    int64_t dist = reuse_tracker_.record_access(block_addr);

    // We assume we're single-threaded.
    // We're only computing miss rate so we just inc counters here.
    if (hit) {
        num_hits_++;
    } else {
        num_misses_++;
        if (dump_misses_)
            dump_miss(memref);

        if (dist < 0) {
            // First-time reference to this block/page: compulsory / cold miss.
            check_compulsory_miss(memref.data.addr);
        } else {
            // Re-reference miss: line was previously in cache and was evicted.
            int64_t cap = get_capacity_in_blocks();
            if (dist >= cap) {
                num_capacity_misses_++; // Capacity miss: reuse distance exceeds device capacity.
            } else {
                num_conflict_misses_++; // Conflict miss: evicted despite working set fitting in capacity.
            }
        }
    }

    // Record access event into power-of-2 reuse distance histogram if enabled.
    if (enable_histogram_) {
        record_reuse_histogram_event(dist);
    }
}

void
caching_device_stats_t::child_access(const memref_t &memref, bool hit,
                                     caching_device_block_t *cache_block)
{
    if (hit)
        num_child_hits_++;
    // else being computed in access()
}

void
caching_device_stats_t::check_compulsory_miss(addr_t addr)
{
    auto lookup_pair = access_count_.lookup(addr);

    // If the address has never been accessed insert proper bound into access_count_
    // and count it as a compulsory miss and cold miss.
    if (!lookup_pair.first) {
        num_compulsory_misses_++;
        num_cold_misses_++;
        access_count_.insert(addr, lookup_pair.second);
    }
}

void
caching_device_stats_t::dump_miss(const memref_t &memref)
{
    addr_t pc, addr;
    memref_pid_t pid;
    if (type_is_instr(memref.instr.type))
        pc = memref.instr.addr;
    else { // data ref: others shouldn't get here
        assert(type_is_prefetch(memref.data.type) ||
               memref.data.type == TRACE_TYPE_READ ||
               memref.data.type == TRACE_TYPE_WRITE);
        pc = memref.data.pc;
    }
    addr = memref.data.addr;
    pid = memref.data.pid;

    // XXX: This writing method to the same file from multiple processes is racy.
    // It works most of the time but consider using a directory with individual files
    // per process as a future safer alternative.
#ifdef HAS_ZLIB
    gzprintf(file_, "%" PRId64 ",0x%zx,0x%zx\n", pid, pc, addr);
#else
    fprintf(file_, "%" PRId64 ",0x%zx,0x%zx\n", pid, pc, addr);
#endif
}

void
caching_device_stats_t::print_warmup(std::string prefix)
{
    std::cerr << prefix << std::setw(18) << std::left << "Warmup hits:" << std::setw(20)
              << std::right << num_hits_at_reset_ << std::endl;
    std::cerr << prefix << std::setw(18) << std::left << "Warmup misses:" << std::setw(20)
              << std::right << num_misses_at_reset_ << std::endl;
}

void
caching_device_stats_t::print_counts(std::string prefix)
{
    std::cerr << prefix << std::setw(18) << std::left << "Hits:" << std::setw(20)
              << std::right << num_hits_ << std::endl;
    std::cerr << prefix << std::setw(18) << std::left << "Misses:" << std::setw(20)
              << std::right << num_misses_ << std::endl;
    std::cerr << prefix << std::setw(18) << std::left
              << "Compulsory misses:" << std::setw(20) << std::right
              << num_compulsory_misses_ << std::endl;
    if (is_coherent_) {
        std::cerr << prefix << std::setw(21) << std::left
                  << "Parent invalidations:" << std::setw(17) << std::right
                  << num_inclusive_invalidates_ + num_exclusive_invalidates_ << std::endl;
        std::cerr << prefix << std::setw(20) << std::left
                  << "Write invalidations:" << std::setw(18) << std::right
                  << num_coherence_invalidates_ << std::endl;
    } else {
        std::cerr << prefix << std::setw(18) << std::left
                  << "Invalidations:" << std::setw(20) << std::right
                  << num_inclusive_invalidates_ + num_exclusive_invalidates_ << std::endl;
    }
}

void
caching_device_stats_t::print_rates(std::string prefix)
{
    if (num_hits_ + num_misses_ > 0) {
        std::string miss_label = "Miss rate:";
        if (num_child_hits_ != 0)
            miss_label = "Local miss rate:";
        std::cerr << prefix << std::setw(18) << std::left << miss_label << std::setw(20)
                  << std::fixed << std::setprecision(2) << std::right
                  << ((float)num_misses_ * 100 / (num_hits_ + num_misses_)) << "%"
                  << std::endl;
    }
}

void
caching_device_stats_t::print_child_stats(std::string prefix)
{
    if (num_child_hits_ != 0) {
        std::cerr << prefix << std::setw(18) << std::left
                  << "Child hits:" << std::setw(20) << std::right << num_child_hits_
                  << std::endl;
        std::cerr << prefix << std::setw(18) << std::left
                  << "Total miss rate:" << std::setw(20) << std::fixed
                  << std::setprecision(2) << std::right
                  << ((float)num_misses_ * 100 /
                      (num_hits_ + num_child_hits_ + num_misses_))
                  << "%" << std::endl;
    }
}

void
caching_device_stats_t::print_stats(std::string prefix)
{
    std::cerr.imbue(std::locale("")); // Add commas, at least for my locale
    if (warmup_enabled_) {
        print_warmup(prefix);
    }
    print_counts(prefix);
    print_rates(prefix);
    print_child_stats(prefix);
    std::cerr.imbue(std::locale("C")); // Reset to avoid affecting later prints.
}

void
caching_device_stats_t::reset()
{
    num_hits_at_reset_ = num_hits_;
    num_misses_at_reset_ = num_misses_;
    num_child_hits_at_reset_ = num_child_hits_;
    num_hits_ = 0;
    num_misses_ = 0;
    num_compulsory_misses_ = 0;
    num_cold_misses_ = 0;
    num_capacity_misses_ = 0;
    num_conflict_misses_ = 0;
    num_child_hits_ = 0;
    num_inclusive_invalidates_ = 0;
    num_coherence_invalidates_ = 0;
    num_exclusive_invalidates_ = 0;
    reuse_tracker_.reset();
    std::fill(reuse_histogram_.begin(), reuse_histogram_.end(), 0);
}

int64_t
caching_device_stats_t::get_capacity_in_blocks() const
{
    // Return explicit capacity if configured.
    if (device_capacity_in_blocks_ > 0) {
        return device_capacity_in_blocks_;
    }
    // Query attached caching device if available.
    if (caching_device_ != nullptr) {
        int64_t cap = caching_device_->get_num_blocks();
        return (cap > 0) ? cap : 1;
    }
    // Fallback default capacity.
    return 1;
}

const std::vector<std::string> &
caching_device_stats_t::get_reuse_histogram_labels()
{
    // Static vector of power-of-2 reuse distance histogram bin ranges up to 1024.
    static const std::vector<std::string> labels = {
        "0", "1", "2", "3-4", "5-8", "9-16", "17-32", "33-64", "65-128", "129-256", "257-512",
        "513-1024", ">1024", "inf"
    };
    return labels; // Return power-of-2 bin range vector.
}

void
caching_device_stats_t::record_reuse_histogram_event(int64_t dist)
{
    // Map negative distance (first-time reference / cold / compulsory) to inf bin.
    if (dist < 0) {
        reuse_histogram_[13]++; // inf bin (bin index 13).
        return;                 // Done recording compulsory event.
    }
    // Handle distance 0 (immediate repeated access).
    if (dist == 0) {
        reuse_histogram_[0]++; // Bin 0 (label "0").
        return;                // Done recording distance 0.
    }
    // Handle distance 1 (1 unique block between references).
    if (dist == 1) {
        reuse_histogram_[1]++; // Bin 1 (label "1").
        return;                // Done recording distance 1.
    }
    // Handle distance 2 (2 unique blocks between references).
    if (dist == 2) {
        reuse_histogram_[2]++; // Bin 2 (label "2").
        return;                // Done recording distance 2.
    }
    // For distances >= 3, compute upper power-of-2 index ceil(log2(dist)).
    uint64_t v = static_cast<uint64_t>(dist) - 1; // Subtract 1 for ceiling calculation.
    int bit = 0;                                  // Power-of-2 exponent tracker.
    while (v > 0) {                               // Loop until all bits shifted out.
        v >>= 1;                                  // Shift right by 1 bit.
        bit++;                                    // Increment bit exponent.
    }
    // Map powers of 2 up to 2^10 (1024).
    if (bit <= 10) {                              // If ceiling power of 2 is <= 1024 (bit <= 10):
        reuse_histogram_[bit + 1]++;              // Map to bins 3 (label "4") through 11 (label "1024").
    } else {                                      // If distance is greater than 1024:
        reuse_histogram_[12]++;                   // Map to bin 12 (label ">1024").
    }
}

void
caching_device_stats_t::invalidate(invalidation_type_t invalidation_type)
{
    if (invalidation_type == INVALIDATION_INCLUSIVE) {
        num_inclusive_invalidates_++;
    } else if (invalidation_type == INVALIDATION_COHERENCE) {
        num_coherence_invalidates_++;
    } else if (invalidation_type == INVALIDATION_EXCLUSIVE) {
        num_exclusive_invalidates_++;
    }
}

} // namespace drmemtrace
} // namespace dynamorio
