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

/* page_table.cpp: Implementation of radix_table_node_t, radix_page_table_t,
 * and mmu_address_space_manager_t.
 */

#include "mmu/page_table.h" // Include class declarations.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

radix_table_node_t::radix_table_node_t(ptw_level_t level, addr_t table_phys_addr)
    // Store the radix tree level (L4..L1).
    : level_(level)
    // Store the 4 KB-aligned physical base address of this page table node.
    , table_phys_addr_(table_phys_addr)
{
}

radix_page_table_t::radix_page_table_t(memref_pid_t pid,
                                       physical_frame_allocator_t *frame_allocator)
    // Store the process ID for this address space.
    : pid_(pid)
    // Store the pointer to the shared physical frame allocator.
    , frame_allocator_(frame_allocator)
    // Allocate the 4 KB root Level 4 (PML4 / CR3) table frame on construction.
    , root_node_(new radix_table_node_t(ptw_level_t::PTW_LEVEL_4_PML4E,
                                        frame_allocator->allocate_page_table_frame()))
    // Initialize the demand page fault counter to zero.
    , page_fault_count_(0)
{
}

radix_table_node_t *
radix_page_table_t::ensure_child_node(radix_table_node_t *parent_node, size_t index)
{
    // Retrieve the existing child pointer at `index` in `parent_node`.
    radix_table_node_t *child = parent_node->get_child(index);
    // If the child node is already allocated, ensure its directory entry is marked present.
    if (child != nullptr) {
        page_table_entry_t &dir_entry = parent_node->get_entry(index);
        dir_entry.set_present(true); // Ensure directory entry P bit is set.
        return child;                // Return the existing child node.
    }
    // Determine the child's radix level (one level below parent_node).
    int child_lvl_int = static_cast<int>(parent_node->get_level()) - 1;
    ptw_level_t child_level = static_cast<ptw_level_t>(child_lvl_int);
    // Allocate a new 4 KB physical frame for the child page table node.
    addr_t child_table_paddr = frame_allocator_->allocate_page_table_frame();
    // Construct the new child radix_table_node_t.
    std::unique_ptr<radix_table_node_t> new_child(
        new radix_table_node_t(child_level, child_table_paddr));
    // Grab the raw pointer before transferring ownership into parent_node.
    radix_table_node_t *child_ptr = new_child.get();
    // Populate the parent directory entry to point to the child table's physical frame.
    page_table_entry_t &dir_entry = parent_node->get_entry(index);
    dir_entry.set_present(true);                // Mark directory entry present.
    dir_entry.set_writable(true);               // Allow writes through directory entry.
    dir_entry.set_user_accessible(true);        // Allow user-space access.
    dir_entry.set_accessed(true);               // Set accessed bit on directory entry.
    dir_entry.set_huge_page(false);             // Intermediate directory is not a leaf.
    dir_entry.set_phys_addr(child_table_paddr); // Store child table physical address.
    // Attach the child unique_ptr to parent_node at `index`.
    parent_node->set_child(index, std::move(new_child));
    // Return the newly created child node pointer.
    return child_ptr;
}

addr_t
radix_page_table_t::resolve_leaf_physical_frame(
    addr_t vaddr, page_size_kind_t kind,
    const std::unordered_map<addr_t, addr_t> *external_v2p)
{
    // Align the virtual address to the boundary of the target page kind.
    addr_t aligned_vbase = page_size_helper_t::align_to_page(vaddr, kind);
    // Also compute the 4 KB-aligned virtual address for looking up in external_v2p.
    addr_t vpage_4kb =
        page_size_helper_t::align_to_page(vaddr, page_size_kind_t::PAGE_SIZE_4KB);
    // Check if an external virtual-to-physical map was provided.
    if (external_v2p != nullptr && !external_v2p->empty()) {
        // First look up the exact 4 KB virtual page in external_v2p.
        auto it = external_v2p->find(vpage_4kb);
        if (it != external_v2p->end()) {
            // Align the mapped physical frame to `kind` (for 2MB/1GB huge pages).
            return page_size_helper_t::align_to_page(it->second, kind);
        }
        // If `kind` is a huge page (2 MB or 1 GB), also check the huge page's head VPN.
        auto it_head = external_v2p->find(aligned_vbase);
        if (it_head != external_v2p->end()) {
            return page_size_helper_t::align_to_page(it_head->second, kind);
        }
    }
    // No external mapping found; allocate a synthetic physical frame of `kind`.
    return frame_allocator_->allocate_data_frame(kind);
}

bool
radix_page_table_t::map_page(addr_t vaddr, addr_t paddr, page_size_kind_t kind,
                             bool writable)
{
    // Align both virtual and physical addresses to the requested page granularity.
    addr_t aligned_vaddr = page_size_helper_t::align_to_page(vaddr, kind);
    addr_t aligned_paddr = page_size_helper_t::align_to_page(paddr, kind);
    // Determine the terminal level where the leaf entry resides (L3, L2, or L1).
    ptw_level_t terminal_level = page_size_helper_t::get_terminal_level(kind);
    // Start at the Level 4 (PML4) root node.
    radix_table_node_t *cur_node = root_node_.get();
    // Descend from Level 4 down to `terminal_level`.
    for (int lvl = static_cast<int>(ptw_level_t::PTW_LEVEL_4_PML4E);
         lvl >= static_cast<int>(terminal_level); --lvl) {
        // Convert loop integer to ptw_level_t enum.
        ptw_level_t cur_level = static_cast<ptw_level_t>(lvl);
        // Extract the 9-bit index at `cur_level` from `aligned_vaddr`.
        size_t idx = page_size_helper_t::extract_level_index(aligned_vaddr, cur_level);
        // Check if we have reached the terminal leaf level for this page granularity.
        if (cur_level == terminal_level) {
            // Configure the leaf PTE / PDE / PDPTE entry at `idx`.
            page_table_entry_t &leaf = cur_node->get_entry(idx);
            leaf.set_present(true);
            leaf.set_writable(writable);
            leaf.set_user_accessible(true);
            leaf.set_accessed(true);
            // Set the hardware Page Size (PS) bit if this is a 2 MB or 1 GB huge page.
            leaf.set_huge_page(kind != page_size_kind_t::PAGE_SIZE_4KB);
            leaf.set_phys_addr(aligned_paddr);
            return true;
        }
        // Otherwise, ensure the next-level child page table node exists and descend.
        cur_node = ensure_child_node(cur_node, idx);
    }
    return false;
}

bool
radix_page_table_t::unmap_page(addr_t vaddr, page_size_kind_t *unmapped_kind_out)
{
    // Start at the Level 4 (PML4) root node.
    radix_table_node_t *cur_node = root_node_.get();
    // Walk down from Level 4 to Level 1 looking for the present leaf entry.
    for (int lvl = static_cast<int>(ptw_level_t::PTW_LEVEL_4_PML4E);
         lvl >= static_cast<int>(ptw_level_t::PTW_LEVEL_1_PTE); --lvl) {
        if (cur_node == nullptr) {
            return false; // Path terminated before finding a present leaf.
        }
        ptw_level_t cur_level = static_cast<ptw_level_t>(lvl);
        size_t idx = page_size_helper_t::extract_level_index(vaddr, cur_level);
        page_table_entry_t &entry = cur_node->get_entry(idx);
        if (!entry.is_present()) {
            return false; // Entry is not present.
        }
        // Check if this entry is a leaf (either Level 1 PTE, or L2/L3 with PS=1).
        if (cur_level == ptw_level_t::PTW_LEVEL_1_PTE || entry.is_huge_page()) {
            // Determine the granularity of the leaf being unmapped.
            page_size_kind_t kind = page_size_kind_t::PAGE_SIZE_4KB;
            if (cur_level == ptw_level_t::PTW_LEVEL_2_PDE) {
                kind = page_size_kind_t::PAGE_SIZE_2MB;
            } else if (cur_level == ptw_level_t::PTW_LEVEL_3_PDPTE) {
                kind = page_size_kind_t::PAGE_SIZE_1GB;
            }
            if (unmapped_kind_out != nullptr) {
                *unmapped_kind_out = kind;
            }
            // Clear the leaf PTE and return true.
            entry.clear();
            return true;
        }
        // Advance to the next child table.
        cur_node = cur_node->get_child(idx);
    }
    return false;
}

bool
radix_page_table_t::walk_and_populate(
    addr_t vaddr, bool is_write, const std::unordered_map<addr_t, addr_t> *external_v2p,
    const huge_page_detector_t *huge_detector, ptw_result_t *result_out)
{
    // Validate the output pointer.
    if (result_out == nullptr) {
        return false;
    }
    // Preserve configured sector size before resetting output structure.
    size_t req_sector_size = result_out->sector_size;
    // Reset the output walk result fields.
    *result_out = ptw_result_t();
    // Restore preserved sector size.
    result_out->sector_size = req_sector_size;
    result_out->pid = pid_;
    result_out->virt_addr = vaddr;
    // Query the huge page detector (if provided) for the page granularity of `vaddr`.
    page_size_kind_t target_kind = page_size_kind_t::PAGE_SIZE_4KB;
    if (huge_detector != nullptr) {
        target_kind = huge_detector->get_page_size_kind(pid_, vaddr);
    }
    // Determine the terminal radix level for `target_kind` (L3, L2, or L1).
    ptw_level_t terminal_level = page_size_helper_t::get_terminal_level(target_kind);
    // Start walking from the Level 4 (PML4) root node.
    radix_table_node_t *cur_node = root_node_.get();
    bool faulted = false;
    // Descend from Level 4 down to `terminal_level`.
    for (int lvl = static_cast<int>(ptw_level_t::PTW_LEVEL_4_PML4E);
         lvl >= static_cast<int>(terminal_level); --lvl) {
        ptw_level_t cur_level = static_cast<ptw_level_t>(lvl);
        size_t idx = page_size_helper_t::extract_level_index(vaddr, cur_level);
        page_table_entry_t &entry = cur_node->get_entry(idx);
        // Check if an existing huge-page leaf is already mapped at L3 or L2 even if
        // target_kind was 4KB (e.g., mapped earlier via map_page()).
        bool existing_huge_leaf =
            entry.is_present() && entry.is_huge_page() &&
            (cur_level == ptw_level_t::PTW_LEVEL_3_PDPTE ||
             cur_level == ptw_level_t::PTW_LEVEL_2_PDE);
        // Check if we have reached the leaf level for this walk.
        if (cur_level == terminal_level || existing_huge_leaf) {
            // Determine the effective page size kind of this leaf.
            page_size_kind_t leaf_kind = target_kind;
            if (existing_huge_leaf) {
                leaf_kind = (cur_level == ptw_level_t::PTW_LEVEL_3_PDPTE)
                    ? page_size_kind_t::PAGE_SIZE_1GB
                    : page_size_kind_t::PAGE_SIZE_2MB;
            }
            // If the leaf entry is not yet present, handle a first-touch demand fault.
            if (!entry.is_present()) {
                faulted = true;      // Mark that this walk incurred a demand page fault.
                ++page_fault_count_; // Increment per-process demand fault count.
                addr_t leaf_frame =
                    resolve_leaf_physical_frame(vaddr, leaf_kind, external_v2p);
                entry.set_present(true);
                entry.set_writable(true);
                entry.set_user_accessible(true);
                entry.set_huge_page(leaf_kind != page_size_kind_t::PAGE_SIZE_4KB);
                entry.set_phys_addr(leaf_frame);
            }
            // Update hardware Accessed (A) and Dirty (D) bits on the leaf entry.
            entry.set_accessed(true);
            if (is_write) {
                entry.set_dirty(true);
            }
            // Record this terminal step in result_out->steps.
            ptw_step_record_t step;
            step.level = cur_level;
            step.table_base_paddr = cur_node->get_table_phys_addr();
            step.entry_index = idx;
            step.entry_paddr = cur_node->get_entry_phys_addr(idx);
            step.next_paddr = entry.get_phys_addr();
            step.pwc_hit = false;
            step.is_leaf = true;
            result_out->steps.push_back(step);
            // Compute the final translated physical address (frame base + page offset).
            addr_t page_offset =
                page_size_helper_t::extract_page_offset(vaddr, leaf_kind);
            result_out->page_size_kind = leaf_kind;
            result_out->page_size_bytes =
                page_size_helper_t::get_page_size_bytes(leaf_kind);
            result_out->page_base_vaddr =
                page_size_helper_t::align_to_page(vaddr, leaf_kind);
            result_out->page_base_paddr = entry.get_phys_addr();
            result_out->phys_addr = entry.get_phys_addr() + page_offset;
            result_out->is_page_fault = faulted;
            result_out->success = true;

            // When sector mode is active (sector_size > 1) and terminal leaf is 4 KB:
            // Fetch/populate all `sector_size` PTEs from the leaf node that share `entry_offset / sector_size`.
            if (result_out->sector_size > 1 && leaf_kind == page_size_kind_t::PAGE_SIZE_4KB) {
                // Compute base index in the leaf table: entries with identical entry_offset / sector_size.
                size_t base_idx = (idx / result_out->sector_size) * result_out->sector_size;
                // Compute sector-aligned virtual base address.
                addr_t vaddr_base = (vaddr & ~((static_cast<addr_t>(result_out->sector_size) * 4096ULL) - 1ULL));
                // Iterate across all sub-entries within the sector (2, 4, or 8).
                for (size_t s = 0; s < result_out->sector_size; ++s) {
                    size_t cur_idx = base_idx + s;                                     // Target leaf entry index.
                    addr_t cur_vaddr = vaddr_base + static_cast<addr_t>(s) * 4096ULL;  // Virtual address of sub-entry.
                    page_table_entry_t &pte = cur_node->get_entry(cur_idx);             // Reference leaf PTE.
                    bool is_demand_entry = (cur_idx == idx);                            // Check if this sub-entry is the demand page.
                    bool has_valid_pfn = false;                                         // Track if sub-entry has a valid PFN.

                    // Check whether this entry already has a valid physical mapping.
                    if (pte.is_present()) {                                             // Check if PTE was already populated.
                        has_valid_pfn = true;                                           // Already-present PTE has valid PFN.
                    } else if (is_demand_entry) {                                       // Check if this is the demand entry.
                        has_valid_pfn = true;                                           // Demand page is always materialized on miss.
                    } else if (external_v2p != nullptr && !external_v2p->empty()) {     // Check if external pagemap map is provided.
                        // Sibling entry: only valid if explicitly present in the external pagemap (Bit 63 == 1).
                        addr_t sib_vpage_4kb = page_size_helper_t::align_to_page(cur_vaddr, page_size_kind_t::PAGE_SIZE_4KB); // Align to 4 KB.
                        auto it_v2p = external_v2p->find(sib_vpage_4kb);                 // Lookup sibling page in pagemap table.
                        if (it_v2p != external_v2p->end()) {                            // Sibling page was valid in pagemap!
                            has_valid_pfn = true;                                       // Mark sibling PFN valid.
                        } else {                                                        // Sibling was unmapped / invalid in pagemap!
                            has_valid_pfn = false;                                      // Mark sibling sub-entry invalid.
                        }
                    } else {                                                            // No external pagemap (synthetic mode).
                        has_valid_pfn = true;                                           // Eagerly synthesize sibling frame.
                    }

                    // If the entry has a valid PFN and is not yet mapped in the page table, allocate its frame.
                    if (has_valid_pfn && !pte.is_present()) {                           // Check if frame allocation needed.
                        addr_t sib_frame = resolve_leaf_physical_frame(cur_vaddr, page_size_kind_t::PAGE_SIZE_4KB, external_v2p); // Resolve frame.
                        pte.set_present(true);                                          // Mark sibling PTE present.
                        pte.set_writable(true);                                         // Mark sibling writable.
                        pte.set_user_accessible(true);                                  // Mark user accessible.
                        pte.set_huge_page(false);                                       // Standard 4 KB page.
                        pte.set_phys_addr(sib_frame);                                   // Store mapped frame address.
                    }

                    // Construct sector entry record.
                    ptw_sector_entry_t s_ent;                                            // Instantiate sector entry record.
                    s_ent.valid = has_valid_pfn;                                        // Reflect valid/invalid PFN status.
                    s_ent.vaddr = cur_vaddr;                                            // Virtual page address.
                    s_ent.paddr = has_valid_pfn ? pte.get_phys_addr() : 0;              // Physical frame address (or 0 if invalid).
                    s_ent.ppn = has_valid_pfn ? (pte.get_phys_addr() >> 12) : 0;        // PPN (or 0 if invalid).
                    s_ent.page_size_kind = page_size_kind_t::PAGE_SIZE_4KB;             // 4 KB page kind.
                    s_ent.dirty = has_valid_pfn ? pte.is_dirty() : false;               // Propagate dirty status if valid.
                    result_out->sector_entries.push_back(s_ent);                        // Append sector entry.
                }
            } else if (result_out->sector_size > 1) {
                // For huge page leaf (2 MB or 1 GB), all sector sub-entries share the huge frame.
                addr_t vaddr_base = (vaddr & ~((static_cast<addr_t>(result_out->sector_size) * 4096ULL) - 1ULL));
                for (size_t s = 0; s < result_out->sector_size; ++s) {
                    ptw_sector_entry_t s_ent;
                    s_ent.valid = true;                                                 // Mark valid.
                    s_ent.vaddr = vaddr_base + static_cast<addr_t>(s) * 4096ULL;        // Virtual page address.
                    s_ent.paddr = entry.get_phys_addr() + static_cast<addr_t>(s) * 4096ULL; // Linear offset frame.
                    s_ent.ppn = s_ent.paddr >> 12;                                      // Physical page number.
                    s_ent.page_size_kind = leaf_kind;                                   // Huge page granularity.
                    s_ent.dirty = entry.is_dirty();                                     // Propagate dirty status.
                    result_out->sector_entries.push_back(s_ent);                        // Append sector entry.
                }
            }
            return true;
        }
        // Intermediate level: check if the child page table node already exists.
        if (cur_node->get_child(idx) == nullptr) {
            faulted = true; // Allocating a missing intermediate page table node.
        }
        radix_table_node_t *child_node = ensure_child_node(cur_node, idx);
        // Record this intermediate directory step in result_out->steps.
        ptw_step_record_t step;
        step.level = cur_level;
        step.table_base_paddr = cur_node->get_table_phys_addr();
        step.entry_index = idx;
        step.entry_paddr = cur_node->get_entry_phys_addr(idx);
        step.next_paddr = child_node->get_table_phys_addr();
        step.pwc_hit = false;
        step.is_leaf = false;
        result_out->steps.push_back(step);
        // Advance `cur_node` to the child table node for the next iteration.
        cur_node = child_node;
    }
    return false;
}

bool
radix_page_table_t::lookup_non_faulting(addr_t vaddr, addr_t *phys_addr_out,
                                        page_size_kind_t *kind_out) const
{
    // Start at the Level 4 (PML4) root node.
    const radix_table_node_t *cur_node = root_node_.get();
    for (int lvl = static_cast<int>(ptw_level_t::PTW_LEVEL_4_PML4E);
         lvl >= static_cast<int>(ptw_level_t::PTW_LEVEL_1_PTE); --lvl) {
        if (cur_node == nullptr) {
            return false; // Missing intermediate table.
        }
        ptw_level_t cur_level = static_cast<ptw_level_t>(lvl);
        size_t idx = page_size_helper_t::extract_level_index(vaddr, cur_level);
        const page_table_entry_t &entry = cur_node->get_entry(idx);
        if (!entry.is_present()) {
            return false; // Entry not present.
        }
        // Check if this entry is a leaf.
        if (cur_level == ptw_level_t::PTW_LEVEL_1_PTE || entry.is_huge_page()) {
            page_size_kind_t kind = page_size_kind_t::PAGE_SIZE_4KB;
            if (cur_level == ptw_level_t::PTW_LEVEL_2_PDE) {
                kind = page_size_kind_t::PAGE_SIZE_2MB;
            } else if (cur_level == ptw_level_t::PTW_LEVEL_3_PDPTE) {
                kind = page_size_kind_t::PAGE_SIZE_1GB;
            }
            if (kind_out != nullptr) {
                *kind_out = kind;
            }
            if (phys_addr_out != nullptr) {
                addr_t offset = page_size_helper_t::extract_page_offset(vaddr, kind);
                *phys_addr_out = entry.get_phys_addr() + offset;
            }
            return true;
        }
        cur_node = cur_node->get_child(idx);
    }
    return false;
}

mmu_address_space_manager_t::mmu_address_space_manager_t(addr_t pt_phys_base,
                                                         addr_t data_phys_base)
    // Initialize the shared physical frame allocator with the specified base regions.
    : frame_allocator_(pt_phys_base, data_phys_base)
    // Initialize the shared huge page detector.
    , huge_page_detector_(2)
{
}

radix_page_table_t *
mmu_address_space_manager_t::get_or_create_page_table(memref_pid_t pid)
{
    // Look up whether a radix_page_table_t already exists for `pid`.
    auto it = page_tables_.find(pid);
    if (it != page_tables_.end()) {
        return it->second.get(); // Return existing per-PID page table.
    }
    // Create a new radix_page_table_t for `pid`.
    std::unique_ptr<radix_page_table_t> pt(
        new radix_page_table_t(pid, &frame_allocator_));
    radix_page_table_t *pt_ptr = pt.get();
    page_tables_[pid] = std::move(pt);
    return pt_ptr;
}

void
mmu_address_space_manager_t::register_external_v2p_mapping(memref_pid_t pid, addr_t vaddr,
                                                           addr_t paddr)
{
    // Align both addresses to 4 KB boundaries.
    addr_t vpage_4kb =
        page_size_helper_t::align_to_page(vaddr, page_size_kind_t::PAGE_SIZE_4KB);
    addr_t ppage_4kb =
        page_size_helper_t::align_to_page(paddr, page_size_kind_t::PAGE_SIZE_4KB);
    // Record in the PID's external v2p map (and also PID 0 global map if pid != 0).
    external_v2p_[pid][vpage_4kb] = ppage_4kb;
    if (pid != 0) {
        external_v2p_[0][vpage_4kb] = ppage_4kb;
    }
    // Reserve the physical frame in the allocator so synthetic frames never collide.
    frame_allocator_.reserve_external_frame(ppage_4kb, page_size_kind_t::PAGE_SIZE_4KB);
    // Feed the mapping to the huge page detector to detect contiguous 2 MB huge pages.
    huge_page_detector_.observe_v2p_mapping(pid, vpage_4kb, ppage_4kb);
    if (pid != 0) {
        huge_page_detector_.observe_v2p_mapping(0, vpage_4kb, ppage_4kb);
    }
}

void
mmu_address_space_manager_t::load_external_v2p_map(
    memref_pid_t pid, const std::unordered_map<addr_t, addr_t> &v2p_map)
{
    // Register every 4 KB virtual->physical mapping from `v2p_map`.
    for (const auto &kv : v2p_map) {
        register_external_v2p_mapping(pid, kv.first, kv.second);
    }
}

const std::unordered_map<addr_t, addr_t> *
mmu_address_space_manager_t::get_external_v2p_map(memref_pid_t pid) const
{
    // Check for a PID-specific v2p map first.
    auto it = external_v2p_.find(pid);
    if (it != external_v2p_.end() && !it->second.empty()) {
        return &it->second;
    }
    // Fall back to the global (PID 0) v2p map if present.
    auto it_global = external_v2p_.find(0);
    if (it_global != external_v2p_.end() && !it_global->second.empty()) {
        return &it_global->second;
    }
    return nullptr;
}

} // namespace drmemtrace
} // namespace dynamorio
