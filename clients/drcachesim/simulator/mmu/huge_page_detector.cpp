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

/* huge_page_detector.cpp: Implementation of huge_page_detector_t.
 */

#include "mmu/huge_page_detector.h" // Include class declaration.

#ifdef LINUX
#    include <fcntl.h>     // Provide open() and O_RDONLY on Linux.
#    include <sys/types.h> // Provide off64_t and standard POSIX types.
#    include <unistd.h>    // Provide pread() and close() on Linux.
#endif

#include <sstream> // Provide std::istringstream for parsing /proc/<pid>/smaps lines.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

huge_page_detector_t::huge_page_detector_t(size_t min_contiguous_subpages_for_heuristic)
    // Ensure at least 1 subpage is required for heuristic detection.
    : min_contiguous_subpages_(min_contiguous_subpages_for_heuristic == 0
                                   ? 1
                                   : min_contiguous_subpages_for_heuristic)
    // Initialize /proc/kpageflags file descriptor to -1 (closed).
    , kpageflags_fd_(-1)
{
}

huge_page_detector_t::~huge_page_detector_t()
{
#ifdef LINUX
    // Close the /proc/kpageflags file descriptor if it was opened.
    if (kpageflags_fd_ >= 0) {
        close(kpageflags_fd_); // Close Linux kernel page flags descriptor.
        kpageflags_fd_ = -1;   // Reset descriptor to -1.
    }
#endif
}

void
huge_page_detector_t::register_huge_page_region(memref_pid_t pid, addr_t start_vaddr,
                                                addr_t end_vaddr, page_size_kind_t kind)
{
    // Ignore empty or inverted intervals.
    if (end_vaddr <= start_vaddr) {
        return; // Nothing to register when end <= start.
    }
    // Check if the caller is registering 1 GB giant pages.
    if (kind == page_size_kind_t::PAGE_SIZE_1GB) {
        // Align the start address down to a 1 GB boundary.
        addr_t cur = page_size_helper_t::align_to_page(
            start_vaddr, page_size_kind_t::PAGE_SIZE_1GB);
        // Step by 1 GB across the interval [start_vaddr, end_vaddr).
        while (cur < end_vaddr) {
            // Insert the 1 GB-aligned virtual base address for this PID.
            huge_1gb_pages_[pid].insert(cur);
            // Advance to the next 1 GB boundary.
            cur += static_cast<addr_t>(mmu_constants_t::PAGE_SIZE_1GB_BYTES);
        }
        return; // Completed 1 GB registration.
    }
    // Check if the caller is registering 2 MB huge pages.
    if (kind == page_size_kind_t::PAGE_SIZE_2MB) {
        // Align the start address down to a 2 MB boundary.
        addr_t cur = page_size_helper_t::align_to_page(
            start_vaddr, page_size_kind_t::PAGE_SIZE_2MB);
        // Step by 2 MB across the interval [start_vaddr, end_vaddr).
        while (cur < end_vaddr) {
            // Insert the 2 MB-aligned virtual base address for this PID.
            huge_2mb_pages_[pid].insert(cur);
            // Advance to the next 2 MB boundary.
            cur += static_cast<addr_t>(mmu_constants_t::PAGE_SIZE_2MB_BYTES);
        }
    }
}

page_size_kind_t
huge_page_detector_t::get_page_size_kind(memref_pid_t pid, addr_t vaddr) const
{
    // Compute the 1 GB-aligned virtual base address for `vaddr`.
    addr_t base_1gb =
        page_size_helper_t::align_to_page(vaddr, page_size_kind_t::PAGE_SIZE_1GB);
    // Check whether this specific PID has `base_1gb` registered as a 1 GB page.
    auto it_1gb_pid = huge_1gb_pages_.find(pid);
    if (it_1gb_pid != huge_1gb_pages_.end() &&
        it_1gb_pid->second.find(base_1gb) != it_1gb_pid->second.end()) {
        return page_size_kind_t::PAGE_SIZE_1GB; // Matched PID-specific 1 GB page.
    }
    // Check whether wildcard PID 0 has `base_1gb` registered as a 1 GB page.
    if (pid != 0) {
        auto it_1gb_any = huge_1gb_pages_.find(0);
        if (it_1gb_any != huge_1gb_pages_.end() &&
            it_1gb_any->second.find(base_1gb) != it_1gb_any->second.end()) {
            return page_size_kind_t::PAGE_SIZE_1GB; // Matched wildcard 1 GB page.
        }
    }
    // Compute the 2 MB-aligned virtual base address for `vaddr`.
    addr_t base_2mb =
        page_size_helper_t::align_to_page(vaddr, page_size_kind_t::PAGE_SIZE_2MB);
    // Check whether this specific PID has `base_2mb` registered as a 2 MB huge page.
    auto it_2mb_pid = huge_2mb_pages_.find(pid);
    if (it_2mb_pid != huge_2mb_pages_.end() &&
        it_2mb_pid->second.find(base_2mb) != it_2mb_pid->second.end()) {
        return page_size_kind_t::PAGE_SIZE_2MB; // Matched PID-specific 2 MB page.
    }
    // Check whether wildcard PID 0 has `base_2mb` registered as a 2 MB huge page.
    if (pid != 0) {
        auto it_2mb_any = huge_2mb_pages_.find(0);
        if (it_2mb_any != huge_2mb_pages_.end() &&
            it_2mb_any->second.find(base_2mb) != it_2mb_any->second.end()) {
            return page_size_kind_t::PAGE_SIZE_2MB; // Matched wildcard 2 MB page.
        }
    }
    // Default to standard 4 KB base page when not inside any registered huge page.
    return page_size_kind_t::PAGE_SIZE_4KB;
}

bool
huge_page_detector_t::observe_v2p_mapping(memref_pid_t pid, addr_t vaddr, addr_t paddr)
{
    // Align both virtual and physical addresses to 4 KB base page boundaries.
    addr_t vpage_4kb =
        page_size_helper_t::align_to_page(vaddr, page_size_kind_t::PAGE_SIZE_4KB);
    addr_t ppage_4kb =
        page_size_helper_t::align_to_page(paddr, page_size_kind_t::PAGE_SIZE_4KB);
    // Extract the 2 MB-aligned virtual base and physical base.
    addr_t vbase_2mb =
        page_size_helper_t::align_to_page(vpage_4kb, page_size_kind_t::PAGE_SIZE_2MB);
    addr_t pbase_2mb =
        page_size_helper_t::align_to_page(ppage_4kb, page_size_kind_t::PAGE_SIZE_2MB);
    // Extract the intra-2MB byte offsets of the virtual and physical 4 KB pages.
    addr_t voff_2mb = page_size_helper_t::extract_page_offset(
        vpage_4kb, page_size_kind_t::PAGE_SIZE_2MB);
    addr_t poff_2mb = page_size_helper_t::extract_page_offset(
        ppage_4kb, page_size_kind_t::PAGE_SIZE_2MB);
    // Look up or create the candidate tracker for (pid, vbase_2mb).
    candidate_2mb_t &cand = candidates_2mb_[pid][vbase_2mb];
    // For a valid 2 MB compound page, the intra-2MB offset of VPN and PPN must match.
    if (voff_2mb != poff_2mb) {
        // Mark this 2 MB virtual slice as conflicted (not a single 2 MB huge page).
        cand.has_conflict = true;
        // If it was previously promoted, demote it.
        huge_2mb_pages_[pid].erase(vbase_2mb);
        return false; // Not a 2 MB huge page.
    }
    // If this is the first 4 KB subpage observed in this 2 MB virtual slice, record pbase.
    if (cand.matched_vpns.empty() && !cand.has_conflict) {
        cand.phys_base_2mb = pbase_2mb; // Record expected 2 MB physical frame base.
    } else if (cand.phys_base_2mb != pbase_2mb) {
        // Subpages within the same 2 MB virtual slice mapped to different 2 MB frames.
        cand.has_conflict = true;
        huge_2mb_pages_[pid].erase(vbase_2mb);
        return false; // Conflicting physical bases.
    }
    // If a prior subpage already conflicted, do not promote.
    if (cand.has_conflict) {
        return false; // Already marked non-huge.
    }
    // Record this 4 KB virtual page as verified within the 2 MB slice.
    cand.matched_vpns.insert(vpage_4kb);
    // Check if the number of verified contiguous-aligned subpages meets the threshold.
    if (cand.matched_vpns.size() >= min_contiguous_subpages_) {
        // Insert vbase_2mb into the active 2 MB huge-page set for this PID.
        auto res = huge_2mb_pages_[pid].insert(vbase_2mb);
        // Return true if this call newly promoted the 2 MB slice.
        return res.second;
    }
    // Threshold not yet reached; return false.
    return false;
}

size_t
huge_page_detector_t::analyze_v2p_map(
    memref_pid_t pid, const std::unordered_map<addr_t, addr_t> &v2p_map)
{
    // Count how many 2 MB huge pages are newly promoted during this bulk scan.
    size_t promoted_count = 0;
    // Iterate through every (virtual_page -> physical_frame) pair in the map.
    for (const auto &kv : v2p_map) {
        // Feed each 4 KB mapping into observe_v2p_mapping().
        if (observe_v2p_mapping(pid, kv.first, kv.second)) {
            ++promoted_count; // Increment count on new 2 MB promotion.
        }
    }
    // Return total number of 2 MB huge pages promoted.
    return promoted_count;
}

size_t
huge_page_detector_t::parse_proc_smaps(memref_pid_t pid, std::istream &smaps_stream)
{
    // Track how many huge-page regions are registered from the smaps stream.
    size_t registered = 0;
    // Track the current VMA's start and end virtual addresses.
    addr_t cur_vma_start = 0;
    addr_t cur_vma_end = 0;
    bool in_vma = false;
    // Read the smaps stream line by line.
    std::string line;
    while (std::getline(smaps_stream, line)) {
        // Skip empty lines.
        if (line.empty()) {
            continue;
        }
        // Check if this line is a VMA header of the form "00400000-00600000 rw-p ...".
        size_t dash_pos = line.find('-');
        size_t space_pos = line.find(' ');
        if (dash_pos != std::string::npos && space_pos != std::string::npos &&
            dash_pos < space_pos) {
            // Attempt to parse the hex start and end addresses around the hyphen.
            std::string start_hex = line.substr(0, dash_pos);
            std::string end_hex = line.substr(dash_pos + 1, space_pos - dash_pos - 1);
            char *endptr1 = nullptr;
            char *endptr2 = nullptr;
            unsigned long long s_val = std::strtoull(start_hex.c_str(), &endptr1, 16);
            unsigned long long e_val = std::strtoull(end_hex.c_str(), &endptr2, 16);
            // Verify both substrings parsed completely as hexadecimal numbers.
            if (endptr1 != start_hex.c_str() && *endptr1 == '\0' &&
                endptr2 != end_hex.c_str() && *endptr2 == '\0') {
                cur_vma_start = static_cast<addr_t>(s_val); // Save VMA start address.
                cur_vma_end = static_cast<addr_t>(e_val);   // Save VMA end address.
                in_vma = true;                              // Mark VMA context active.
                continue;                                   // Advance to attribute lines.
            }
        }
        // If we are inside a VMA block, inspect relevant huge-page attribute keys.
        if (!in_vma) {
            continue;
        }
        // Parse "Key: <value> kB" lines using a string stream.
        std::istringstream iss(line);
        std::string key;
        uint64_t kb_val = 0;
        if (!(iss >> key >> kb_val)) {
            continue; // Line does not match "Key: <number>".
        }
        // Check for explicit hugetlbfs kernel/MMU page size (2048 kB or 1048576 kB).
        if (key == "KernelPageSize:" || key == "MMUPageSize:") {
            if (kb_val == 1048576ULL) {
                // Register the entire VMA as 1 GB giant pages.
                register_huge_page_region(pid, cur_vma_start, cur_vma_end,
                                          page_size_kind_t::PAGE_SIZE_1GB);
                ++registered;
            } else if (kb_val == 2048ULL) {
                // Register the entire VMA as 2 MB huge pages.
                register_huge_page_region(pid, cur_vma_start, cur_vma_end,
                                          page_size_kind_t::PAGE_SIZE_2MB);
                ++registered;
            }
        } else if (key == "AnonHugePages:" || key == "ShmemPmdMapped:" ||
                   key == "FilePmdMapped:") {
            // If THP PMD mappings are present (>= 2048 kB), register the 2 MB aligned
            // sub-region of this VMA up to kb_val * 1024 bytes.
            if (kb_val >= 2048ULL) {
                // Align cur_vma_start up to the first 2 MB boundary inside the VMA.
                uint64_t page_2mb = mmu_constants_t::PAGE_SIZE_2MB_BYTES;
                addr_t aligned_start = static_cast<addr_t>(
                    (cur_vma_start + page_2mb - 1ULL) & ~(page_2mb - 1ULL));
                // Compute the end of the THP-backed portion within the VMA.
                addr_t thp_end = aligned_start + static_cast<addr_t>(kb_val * 1024ULL);
                if (thp_end > cur_vma_end) {
                    thp_end = cur_vma_end; // Clamp to VMA end boundary.
                }
                if (thp_end > aligned_start) {
                    register_huge_page_region(pid, aligned_start, thp_end,
                                              page_size_kind_t::PAGE_SIZE_2MB);
                    ++registered;
                }
            }
        }
    }
    // Return count of huge-page VMA entries registered.
    return registered;
}

bool
huge_page_detector_t::open_kpageflags()
{
#ifdef LINUX
    // If already open, return true immediately.
    if (kpageflags_fd_ >= 0) {
        return true;
    }
    // Attempt to open /proc/kpageflags in read-only mode.
    kpageflags_fd_ = open("/proc/kpageflags", O_RDONLY);
    // Return true if open succeeded.
    return (kpageflags_fd_ >= 0);
#else
    // Not supported on non-Linux platforms.
    return false;
#endif
}

bool
huge_page_detector_t::query_kpageflags_pfn(uint64_t pfn, bool *is_huge_out) const
{
    // Validate output pointer.
    if (is_huge_out == nullptr) {
        return false;
    }
    // Default output to false.
    *is_huge_out = false;
#ifdef LINUX
    // Ensure /proc/kpageflags descriptor is open.
    if (kpageflags_fd_ < 0) {
        return false;
    }
    // Each PFN has an 8-byte (64-bit) flags entry in /proc/kpageflags.
    off64_t offset = static_cast<off64_t>(pfn * sizeof(uint64_t));
    uint64_t flags = 0;
    // Use pread64 to read the 8-byte entry atomically at `offset` without mutating pos.
    ssize_t bytes_read = pread(kpageflags_fd_, &flags, sizeof(flags), offset);
    if (bytes_read != static_cast<ssize_t>(sizeof(flags))) {
        return false; // Failed to read 8 bytes from /proc/kpageflags.
    }
    // Check if any huge/compound page bit (THP, HUGE, COMPOUND_HEAD/TAIL) is set.
    *is_huge_out = ((flags & kpageflags_bits_t::KPF_ANY_HUGE_MASK) != 0);
    return true;
#else
    (void)pfn; // Suppress unused parameter warning on non-Linux builds.
    return false;
#endif
}

void
huge_page_detector_t::unregister_huge_page(memref_pid_t pid, addr_t vaddr)
{
    // Remove from 2 MB set if present.
    addr_t base_2mb =
        page_size_helper_t::align_to_page(vaddr, page_size_kind_t::PAGE_SIZE_2MB);
    auto it_2mb = huge_2mb_pages_.find(pid);
    if (it_2mb != huge_2mb_pages_.end()) {
        it_2mb->second.erase(base_2mb);
    }
    // Remove from 1 GB set if present.
    addr_t base_1gb =
        page_size_helper_t::align_to_page(vaddr, page_size_kind_t::PAGE_SIZE_1GB);
    auto it_1gb = huge_1gb_pages_.find(pid);
    if (it_1gb != huge_1gb_pages_.end()) {
        it_1gb->second.erase(base_1gb);
    }
}

size_t
huge_page_detector_t::get_num_2mb_regions() const
{
    // Sum the number of 2 MB base entries across all tracked PIDs.
    size_t total = 0;
    for (const auto &kv : huge_2mb_pages_) {
        total += kv.second.size();
    }
    return total;
}

size_t
huge_page_detector_t::get_num_1gb_regions() const
{
    // Sum the number of 1 GB base entries across all tracked PIDs.
    size_t total = 0;
    for (const auto &kv : huge_1gb_pages_) {
        total += kv.second.size();
    }
    return total;
}

} // namespace drmemtrace
} // namespace dynamorio
