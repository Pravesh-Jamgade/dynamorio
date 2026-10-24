/* **********************************************************
 * Copyright (c) 2017-2026 Google, Inc.  All rights reserved.
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

/* tlb_simulator_create.h: DrMemtrace TLB and MMU simulator creation and knobs. */

#ifndef _TLB_SIMULATOR_CREATE_H_ // Guard against multiple inclusions.
#define _TLB_SIMULATOR_CREATE_H_ 1 // Define inclusion guard macro.

#include <stdint.h> // Provide uint64_t definition.
#include <string>   // Provide std::string definition.

#include "analysis_tool.h" // Provide analysis_tool_t interface.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @file drmemtrace/tlb_simulator_create.h
 * @brief DrMemtrace TLB and MMU simulator creation.
 */

/**
 * The options for tlb_simulator_create().
 * The options are currently documented in \ref sec_drcachesim_ops.
 */
// The options are currently documented in ../common/options.cpp.
struct tlb_simulator_knobs_t {
    // Default constructor initializing all TLB and MMU simulator knobs.
    tlb_simulator_knobs_t()
        : num_cores(4)                    // Simulate 4 CPU cores by default.
        , page_size(4 * 1024)             // Default base page size is 4 KB (4096 bytes).
        , TLB_L1I_entries(32)             // Default 32 entries in each L1 instruction TLB.
        , TLB_L1D_entries(32)             // Default 32 entries in each L1 data TLB.
        , TLB_L1I_assoc(32)               // Default 32-way associativity for L1I TLB.
        , TLB_L1D_assoc(32)               // Default 32-way associativity for L1D TLB.
        , TLB_L2_entries(1024)            // Default 1024 entries in each unified L2 TLB.
        , TLB_L2_assoc(4)                 // Default 4-way associativity for L2 TLB.
        , TLB_replace_policy("LFU")       // Default LFU replacement policy (supports LRU/FIFO).
        , skip_refs(0)                    // Default 0 initial references skipped.
        , warmup_refs(0)                  // Default 0 warmup references.
        , warmup_fraction(0.0)            // Default 0.0 warmup fraction.
        , sim_refs(1ULL << 63)            // Default 2^63 references simulated.
        , cpu_scheduling(false)           // Default thread-to-core scheduling mode.
        , use_physical(false)             // Default virtual addressing mode.
        , v2p_file("")                    // Default empty path to v2p.textproto.
        , verbose(0)                      // Default verbosity level 0.
        , enable_page_table_walk(false)   // Enable 4-level radix page table & PTW on L2 miss.
        , enable_huge_pages(false)        // Enable 2 MB / 1 GB huge-page detection & TLB tags.
        , TLB_pwc_entries(32)             // Default 32 entries in each core's Page Walk Cache.
        , TLB_pwc_assoc(4)                // Default 4-way associativity for Page Walk Cache.
        , couple_ptw_to_dcache(false)     // Issue 8-byte physical PTE reads into data cache.
        , ptw_dcache_line_size(64)        // Default 64-byte line size for coupled PTW dcache.
        , ptw_dcache_size(32 * 1024)      // Default 32 KB size for coupled PTW dcache.
        , ptw_dcache_assoc(8)             // Default 8-way associativity for coupled PTW dcache.
    {
    }
    unsigned int num_cores;             // Number of simulated processor cores.
    uint64_t page_size;                 // Base virtual/physical page size in bytes.
    unsigned int TLB_L1I_entries;       // Number of entries in each private L1I TLB.
    unsigned int TLB_L1D_entries;       // Number of entries in each private L1D TLB.
    unsigned int TLB_L1I_assoc;         // Associativity of each private L1I TLB.
    unsigned int TLB_L1D_assoc;         // Associativity of each private L1D TLB.
    unsigned int TLB_L2_entries;        // Number of entries in each private unified L2 TLB.
    unsigned int TLB_L2_assoc;          // Associativity of each private unified L2 TLB.
    std::string TLB_replace_policy;     // Replacement policy ("LFU", "LRU", or "FIFO").
    uint64_t skip_refs;                 // Count of initial trace records to skip.
    uint64_t warmup_refs;               // Count of trace records to warm up TLBs.
    double warmup_fraction;             // Fraction of L2 TLB entries to fill for warmup.
    uint64_t sim_refs;                  // Count of trace records to simulate after warmup.
    bool cpu_scheduling;                // Whether to schedule threads using recorded CPUs.
    bool use_physical;                  // Whether to use physical page mappings.
    std::string v2p_file;               // Optional path to v2p.textproto file.
    unsigned int verbose;               // Notification verbosity level.
    bool enable_page_table_walk;        // Whether to simulate 4-level radix PTW on L2 miss.
    bool enable_huge_pages;             // Whether to detect and simulate 2MB/1GB huge pages.
    unsigned int TLB_pwc_entries;       // Capacity of per-core Page Walk Cache (0 = off).
    unsigned int TLB_pwc_assoc;         // Associativity of per-core Page Walk Cache.
    bool couple_ptw_to_dcache;          // Whether PTW reads 8-byte PTEs through data cache.
    unsigned int ptw_dcache_line_size;  // Line size of internal coupled PTW data cache.
    uint64_t ptw_dcache_size;           // Total byte size of internal coupled PTW dcache.
    unsigned int ptw_dcache_assoc;      // Associativity of internal coupled PTW dcache.
};

/** Creates an instance of a TLB simulator. */
analysis_tool_t *
tlb_simulator_create(const tlb_simulator_knobs_t &knobs);

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _TLB_SIMULATOR_CREATE_H_ */
