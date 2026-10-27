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

/* page_table_walker.cpp: Implementation of page_table_walker_t.
 */

#include "mmu/page_table_walker.h" // Include class declaration.

#include "caching_device_stats.h" // Provide metric_name_t::HITS for coupled cache checks.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

page_table_walker_t::page_table_walker_t(int core_id,
                                         mmu_address_space_manager_t *address_space_mgr,
                                         unsigned int pwc_entries, unsigned int pwc_assoc,
                                         caching_device_t *ptw_data_cache)
    // Store the zero-based CPU core identifier.
    : core_id_(core_id)
    // Store the shared multi-process radix page table manager pointer.
    , address_space_mgr_(address_space_mgr)
    // Initialize the per-core Page Walk Cache with the specified entries and assoc.
    , pwc_(pwc_entries, pwc_assoc)
    // Initialize the per-core MMU statistics collector.
    , stats_()
    // Store the optional coupled CPU data cache pointer.
    , ptw_data_cache_(ptw_data_cache)
{
}

ptw_cache_hit_level_t
page_table_walker_t::issue_pte_cache_read(const memref_t &origin_memref,
                                          addr_t pte_phys_addr,
                                          ptw_level_t ptw_level)
{
    // If no coupled CPU data cache is attached, record as DRAM and return.
    if (ptw_data_cache_ == nullptr) {
        stats_.record_ptw_cache_access(ptw_level, ptw_cache_hit_level_t::HIT_DRAM); // Record DRAM read.
        return ptw_cache_hit_level_t::HIT_DRAM; // Return DRAM hit level.
    }
    // Traverse the cache hierarchy upward starting from the coupled L1D cache.
    caching_device_t *c1 = ptw_data_cache_;                             // L1 Data cache.
    caching_device_t *c2 = (c1 != nullptr) ? c1->get_parent() : nullptr; // L2 Unified cache.
    caching_device_t *c3 = (c2 != nullptr) ? c2->get_parent() : nullptr; // L3 Shared cache.
    // Snapshot the cache hit counters prior to issuing the 8-byte PTE read.
    int64_t c1_hits_before = 0; // Prior L1D hit count.
    int64_t c2_hits_before = 0; // Prior L2 hit count.
    int64_t c3_hits_before = 0; // Prior L3 hit count.
    if (c1 != nullptr && c1->get_stats() != nullptr) {
        c1_hits_before = c1->get_stats()->get_metric(metric_name_t::HITS); // Read L1D hits.
    }
    if (c2 != nullptr && c2->get_stats() != nullptr) {
        c2_hits_before = c2->get_stats()->get_metric(metric_name_t::HITS); // Read L2 hits.
    }
    if (c3 != nullptr && c3->get_stats() != nullptr) {
        c3_hits_before = c3->get_stats()->get_metric(metric_name_t::HITS); // Read L3 hits.
    }
    // Construct a synthetic physical TRACE_TYPE_READ for the hardware PTE (or full sector of PTEs).
    memref_t pte_memref = {};                           // Zero-initialize memref struct.
    pte_memref.data.type = TRACE_TYPE_READ;             // Set read type for PTE lookup.
    pte_memref.data.pid = origin_memref.data.pid;       // Propagate process identifier.
    pte_memref.data.tid = origin_memref.data.tid;       // Propagate thread identifier.

    // If reading leaf PTE and sector mode is active, read the full sector block (e.g. 64B for 8 PTEs).
    size_t access_size = mmu_constants_t::PTE_SIZE_BYTES;                                 // Default 8 bytes.
    addr_t target_paddr = pte_phys_addr;                                                  // Target physical address.
    if (ptw_level == ptw_level_t::PTW_LEVEL_1_PTE && sector_size_ > 1) {                  // Check sector mode.
        access_size = sector_size_ * mmu_constants_t::PTE_SIZE_BYTES;                     // E.g., 8 * 8 = 64 bytes.
        target_paddr = pte_phys_addr & ~static_cast<addr_t>(access_size - 1);             // Align to sector boundary.
    }
    pte_memref.data.addr = target_paddr;                                                  // Set physical address.
    pte_memref.data.size = access_size;                                                   // Set physical access width.
    pte_memref.data.pc =
        type_is_instr(origin_memref.instr.type) ? origin_memref.instr.addr
                                                : origin_memref.data.pc; // Source PC.
    // Dispatch the physical PTE read into the coupled data cache hierarchy.
    ptw_data_cache_->request(pte_memref);
    // Snapshot hit counters after issuing the PTE read.
    int64_t c1_hits_after = 0; // Post L1D hit count.
    int64_t c2_hits_after = 0; // Post L2 hit count.
    int64_t c3_hits_after = 0; // Post L3 hit count.
    if (c1 != nullptr && c1->get_stats() != nullptr) {
        c1_hits_after = c1->get_stats()->get_metric(metric_name_t::HITS); // Read L1D hits.
    }
    if (c2 != nullptr && c2->get_stats() != nullptr) {
        c2_hits_after = c2->get_stats()->get_metric(metric_name_t::HITS); // Read L2 hits.
    }
    if (c3 != nullptr && c3->get_stats() != nullptr) {
        c3_hits_after = c3->get_stats()->get_metric(metric_name_t::HITS); // Read L3 hits.
    }
    // Determine the cache hierarchy level where the 8-byte PTE read was satisfied.
    ptw_cache_hit_level_t hit_level = ptw_cache_hit_level_t::HIT_DRAM; // Default to DRAM.
    if (c1_hits_after > c1_hits_before) {
        hit_level = ptw_cache_hit_level_t::HIT_L1D; // Satisfied by L1D cache hit.
    } else if (c2_hits_after > c2_hits_before) {
        hit_level = ptw_cache_hit_level_t::HIT_L2;  // Satisfied by L2 cache hit.
    } else if (c3_hits_after > c3_hits_before) {
        hit_level = ptw_cache_hit_level_t::HIT_L3;  // Satisfied by L3 cache hit.
    } else {
        hit_level = ptw_cache_hit_level_t::HIT_DRAM; // Missed caches; serviced by DRAM.
    }
    // Record the outcome in per-core MMU stats.
    stats_.record_ptw_cache_access(ptw_level, hit_level); // Record per-level cache hit.
    return hit_level;                                     // Return resolved hit level.
}

bool
page_table_walker_t::walk(const memref_t &memref, addr_t vaddr, ptw_result_t *result_out)
{
    // Validate that the shared address space manager is available.
    if (address_space_mgr_ == nullptr) {
        return false;
    }
    // Extract the process ID and whether the access is a write.
    memref_pid_t pid = memref.data.pid;
    bool is_write = (memref.data.type == TRACE_TYPE_WRITE);
    // Retrieve or create the per-process 4-level radix page table.
    radix_page_table_t *pt = address_space_mgr_->get_or_create_page_table(pid);
    // Retrieve any external virtual-to-physical mapping table for this process.
    const std::unordered_map<addr_t, addr_t> *ext_v2p =
        address_space_mgr_->get_external_v2p_map(pid);
    // Walk and populate the architectural radix page table to get all steps [L4..leaf].
    ptw_result_t local_result;
    // Propagate configured sector size into walk request.
    local_result.sector_size = sector_size_;
    if (!pt->walk_and_populate(vaddr, is_write, ext_v2p,
                               &address_space_mgr_->get_huge_page_detector(),
                               &local_result)) {
        return false;
    }
    // Probe the Page Walk Cache (PWC) from the deepest intermediate directory level
    // upward (i.e., from steps.size() - 2 down to 0) to find the deepest cached table.
    // Note: steps.back() is always the terminal leaf (L1 PTE, L2 PDE with PS=1, or
    // L3 PDPTE with PS=1), which is cached in the TLB rather than the PWC.
    int deepest_pwc_hit_idx = -1;
    if (pwc_.is_enabled() && local_result.steps.size() >= 2) {
        // Iterate from the lowest intermediate directory level up to Level 4 (PML4E).
        for (int i = static_cast<int>(local_result.steps.size()) - 2; i >= 0; --i) {
            ptw_level_t lvl = local_result.steps[i].level;
            addr_t cached_next_paddr = 0;
            // Query the PWC for (pid, lvl, vaddr).
            if (pwc_.lookup(pid, lvl, vaddr, &cached_next_paddr)) {
                // Record the PWC hit at `lvl`.
                stats_.record_pwc_access(lvl, /*hit=*/true);
                deepest_pwc_hit_idx = i;
                local_result.pwc_hit_level = lvl;
                // Mark this step and all higher ancestor levels [0..i] as bypassed by PWC.
                for (int j = 0; j <= i; ++j) {
                    local_result.steps[j].pwc_hit = true;
                }
                break; // Stop probing once the deepest intermediate PWC hit is found.
            } else {
                // Record the PWC miss at `lvl`.
                stats_.record_pwc_access(lvl, /*hit=*/false);
            }
        }
    }
    // Calculate total walk latency in processor cycles.
    uint64_t walk_latency = 0; // Accumulated latency in cycles.
    if (deepest_pwc_hit_idx >= 0) { // If higher levels were satisfied by PWC:
        walk_latency += pwc_hit_latency_; // Charge PWC directory lookup latency.
    }
    // Now execute the actual 8-byte PTE memory reads for all steps after deepest_pwc_hit_idx.
    int memory_reads = 0;
    for (size_t i = static_cast<size_t>(deepest_pwc_hit_idx + 1);
         i < local_result.steps.size(); ++i) {
        ptw_step_record_t &step = local_result.steps[i]; // Mutable reference to walk step.
        // Issue an 8-byte physical memory read for `step.entry_paddr` into the coupled
        // data cache (if attached), determine cache hit level, and record the outcome.
        step.cache_hit_level =
            issue_pte_cache_read(memref, step.entry_paddr, step.level); // Issue read & save hit level.
        ++memory_reads; // Increment count of memory reads issued in this walk.
        // Accumulate memory read latency based on the cache level that serviced the PTE read.
        if (step.cache_hit_level == ptw_cache_hit_level_t::HIT_L1D) { // Hit in L1D:
            walk_latency += l1d_hit_latency_; // Add L1D latency.
        } else if (step.cache_hit_level == ptw_cache_hit_level_t::HIT_L2) { // Hit in L2:
            walk_latency += l2_hit_latency_; // Add L2 latency.
        } else if (step.cache_hit_level == ptw_cache_hit_level_t::HIT_L3) { // Hit in L3:
            walk_latency += l3_hit_latency_; // Add L3 latency.
        } else { // Serviced by main memory DRAM:
            walk_latency += dram_latency_; // Add DRAM latency.
        }
        // If this step is an intermediate directory, insert it into the PWC for future walks.
        if (!step.is_leaf && pwc_.is_enabled()) {
            pwc_.insert(pid, step.level, vaddr, step.next_paddr);
        }
    }
    // Fallback minimum walk latency if no steps were traversed.
    if (walk_latency == 0) { // Check zero walk latency:
        walk_latency = dram_latency_; // Default to DRAM latency.
    }
    // Store the final number of 8-byte memory reads performed during this walk.
    local_result.num_memory_reads = memory_reads;
    local_result.total_latency_cycles = walk_latency; // Store total latency in walk result.
    last_walk_latency_ = walk_latency; // Store last walk duration on the walker instance.
    // Record the completed walk in the core's MMU statistics collector.
    stats_.record_walk(local_result);
    // Copy local_result to `result_out` if the caller provided an output pointer.
    if (result_out != nullptr) {
        *result_out = local_result;
    }
    return true;
}

void
page_table_walker_t::handle_shootdown(memref_pid_t pid, addr_t vaddr)
{
    // Invalidate matching intermediate entries in this core's Page Walk Cache.
    pwc_.invalidate_vaddr(pid, vaddr);
    // Increment the core's shootdown counter.
    stats_.record_shootdown();
}

void
page_table_walker_t::flush_pid(memref_pid_t pid)
{
    // Flush all PWC entries belonging to `pid` on this core.
    pwc_.invalidate_pid(pid);
    // Record the flush as a shootdown event.
    stats_.record_shootdown();
}

} // namespace drmemtrace
} // namespace dynamorio
