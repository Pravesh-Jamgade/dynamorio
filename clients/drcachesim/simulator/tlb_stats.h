/* **********************************************************
 * Copyright (c) 2015-2026 Google, Inc.  All rights reserved.
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

/* tlb_stats.h: Collects per-TLB statistics including hits, misses, compulsory
 * misses, invalidations/shootdowns, and breakdown by page size (4KB, 2MB, 1GB).
 */

#ifndef _TLB_STATS_H_ // Guard against multiple inclusions.
#define _TLB_STATS_H_ 1 // Define inclusion guard macro.

#include "caching_device_stats.h" // Provide base caching_device_stats_t class.
#include "mmu/mmu_types.h"        // Provide page_size_kind_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Statistics collector for a single hardware TLB device.
 */
class tlb_stats_t : public caching_device_stats_t {
public:
    /**
     * @brief Constructs a TLB statistics collector for a given base page size.
     *
     * @param block_size     Base page size in bytes (e.g., 4096).
     * @param warmup_enabled Whether warmup stats reporting is enabled.
     */
    explicit tlb_stats_t(int block_size, bool warmup_enabled = false)
        : caching_device_stats_t("", block_size, warmup_enabled) // Init base class.
        , hits_4kb_(0)   // Initialize 4 KB TLB hit counter.
        , misses_4kb_(0) // Initialize 4 KB TLB miss counter.
        , hits_2mb_(0)   // Initialize 2 MB huge-page TLB hit counter.
        , misses_2mb_(0) // Initialize 2 MB huge-page TLB miss counter.
        , hits_1gb_(0)   // Initialize 1 GB giant-page TLB hit counter.
        , misses_1gb_(0) // Initialize 1 GB giant-page TLB miss counter.
    {
    }

    /**
     * @brief Records a TLB access at a specific page size granularity.
     *
     * @param kind The page size kind (4KB, 2MB, or 1GB) of the accessed page.
     * @param hit  True if the access hit in this TLB; false if it missed.
     */
    void
    record_page_size_access(page_size_kind_t kind, bool hit)
    {
        // Increment the hit or miss counter corresponding to `kind`.
        if (kind == page_size_kind_t::PAGE_SIZE_2MB) {
            if (hit)
                ++hits_2mb_; // Increment 2 MB hit count.
            else
                ++misses_2mb_; // Increment 2 MB miss count.
        } else if (kind == page_size_kind_t::PAGE_SIZE_1GB) {
            if (hit)
                ++hits_1gb_; // Increment 1 GB hit count.
            else
                ++misses_1gb_; // Increment 1 GB miss count.
        } else {
            if (hit)
                ++hits_4kb_; // Increment 4 KB hit count.
            else
                ++misses_4kb_; // Increment 4 KB miss count.
        }
    }

    /**
     * @brief Resets all base and TLB-specific counters (e.g., after warmup completes).
     */
    void
    reset() override
    {
        caching_device_stats_t::reset(); // Reset base hit/miss/compulsory counters.
        hits_4kb_ = 0;                   // Reset 4 KB hits.
        misses_4kb_ = 0;                 // Reset 4 KB misses.
        hits_2mb_ = 0;                   // Reset 2 MB hits.
        misses_2mb_ = 0;                 // Reset 2 MB misses.
        hits_1gb_ = 0;                   // Reset 1 GB hits.
        misses_1gb_ = 0;                 // Reset 1 GB misses.
    }

    // Accessors for page-size specific TLB hits and misses.
    int64_t
    get_hits_4kb() const
    {
        return hits_4kb_; // Return 4 KB TLB hits.
    }

    int64_t
    get_misses_4kb() const
    {
        return misses_4kb_; // Return 4 KB TLB misses.
    }

    int64_t
    get_hits_2mb() const
    {
        return hits_2mb_; // Return 2 MB huge-page TLB hits.
    }

    int64_t
    get_misses_2mb() const
    {
        return misses_2mb_; // Return 2 MB huge-page TLB misses.
    }

    int64_t
    get_hits_1gb() const
    {
        return hits_1gb_; // Return 1 GB giant-page TLB hits.
    }

    int64_t
    get_misses_1gb() const
    {
        return misses_1gb_; // Return 1 GB giant-page TLB misses.
    }

private:
    int64_t hits_4kb_;   // Count of hits on 4 KB base pages.
    int64_t misses_4kb_; // Count of misses on 4 KB base pages.
    int64_t hits_2mb_;   // Count of hits on 2 MB huge pages.
    int64_t misses_2mb_; // Count of misses on 2 MB huge pages.
    int64_t hits_1gb_;   // Count of hits on 1 GB giant pages.
    int64_t misses_1gb_; // Count of misses on 1 GB giant pages.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _TLB_STATS_H_ */
