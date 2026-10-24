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

void
page_table_walker_t::issue_pte_cache_read(const memref_t &origin_memref,
                                          addr_t pte_phys_addr)
{
    // If no coupled CPU data cache is attached, do nothing.
    if (ptw_data_cache_ == nullptr) {
        return;
    }
    // Snapshot the data cache's hit counter prior to issuing the 8-byte PTE read.
    int64_t hits_before = 0;
    caching_device_stats_t *cache_stats = ptw_data_cache_->get_stats();
    if (cache_stats != nullptr) {
        hits_before = cache_stats->get_metric(metric_name_t::HITS);
    }
    // Construct a synthetic 8-byte physical TRACE_TYPE_READ for the hardware PTE.
    memref_t pte_memref = {};
    pte_memref.data.type = TRACE_TYPE_READ;
    pte_memref.data.pid = origin_memref.data.pid;
    pte_memref.data.tid = origin_memref.data.tid;
    pte_memref.data.addr = pte_phys_addr;
    pte_memref.data.size = mmu_constants_t::PTE_SIZE_BYTES;
    pte_memref.data.pc =
        type_is_instr(origin_memref.instr.type) ? origin_memref.instr.addr
                                                : origin_memref.data.pc;
    // Dispatch the 8-byte physical PTE read into the coupled data cache hierarchy.
    ptw_data_cache_->request(pte_memref);
    // Check whether the PTE read hit or missed in the coupled data cache.
    if (cache_stats != nullptr) {
        int64_t hits_after = cache_stats->get_metric(metric_name_t::HITS);
        stats_.record_ptw_cache_access(hits_after > hits_before);
    }
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
    // Now execute the actual 8-byte PTE memory reads for all steps after deepest_pwc_hit_idx.
    int memory_reads = 0;
    for (size_t i = static_cast<size_t>(deepest_pwc_hit_idx + 1);
         i < local_result.steps.size(); ++i) {
        const ptw_step_record_t &step = local_result.steps[i];
        // Issue an 8-byte physical memory read for `step.entry_paddr` into the coupled
        // data cache (if attached) and increment the PTW memory read counter.
        issue_pte_cache_read(memref, step.entry_paddr);
        ++memory_reads;
        // If this step is an intermediate directory, insert it into the PWC for future walks.
        if (!step.is_leaf && pwc_.is_enabled()) {
            pwc_.insert(pid, step.level, vaddr, step.next_paddr);
        }
    }
    // Store the final number of 8-byte memory reads performed during this walk.
    local_result.num_memory_reads = memory_reads;
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
