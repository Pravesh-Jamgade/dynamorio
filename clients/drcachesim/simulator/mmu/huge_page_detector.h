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

/* huge_page_detector.h: Detects whether a virtual address (pid, vaddr) is
 * backed by a 2 MB or 1 GB huge page using:
 *   1. Linux /proc/kpageflags (KPF_THP, KPF_HUGE, KPF_COMPOUND_HEAD/TAIL),
 *   2. Linux /proc/<pid>/smaps VMA huge-page annotations,
 *   3. Contiguous PFN alignment analysis on v2p mappings, and
 *   4. Explicit huge-page region registration.
 */

#ifndef _DRMEMTRACE_HUGE_PAGE_DETECTOR_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_HUGE_PAGE_DETECTOR_H_ 1 // Define inclusion guard macro.

#include <istream>       // Provide std::istream for parsing /proc/pid/smaps streams.
#include <stddef.h>      // Provide size_t definition.
#include <stdint.h>      // Provide fixed-width integer types.
#include <string>        // Provide std::string.
#include <unordered_map> // Provide std::unordered_map for per-PID lookup tables.
#include <unordered_set> // Provide std::unordered_set for fast 2MB/1GB base lookups.

#include "mmu/mmu_types.h" // Provide page_size_kind_t and mmu_constants_t.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

/**
 * @brief Linux /proc/kpageflags bit masks for compound/huge page detection.
 *
 * Reference: Linux kernel Documentation/admin-guide/mm/pagemap.rst.
 */
struct kpageflags_bits_t {
    // Bit 15: Page is the head of a compound page allocation.
    static constexpr uint64_t KPF_COMPOUND_HEAD_BIT = (1ULL << 15);
    // Bit 16: Page is a tail sub-page of a compound page allocation.
    static constexpr uint64_t KPF_COMPOUND_TAIL_BIT = (1ULL << 16);
    // Bit 17: Page is an explicit hugetlbfs huge page.
    static constexpr uint64_t KPF_HUGE_BIT = (1ULL << 17);
    // Bit 22: Page is a Transparent Huge Page (THP).
    static constexpr uint64_t KPF_THP_BIT = (1ULL << 22);
    // Combined mask indicating any huge or compound page backing.
    static constexpr uint64_t KPF_ANY_HUGE_MASK =
        KPF_COMPOUND_HEAD_BIT | KPF_COMPOUND_TAIL_BIT | KPF_HUGE_BIT | KPF_THP_BIT;
};

/**
 * @brief Object-oriented detector that determines the hardware page granularity
 *        (4 KB, 2 MB, or 1 GB) backing a process virtual address.
 */
class huge_page_detector_t {
public:
    /**
     * @brief Constructs a huge page detector.
     *
     * @param min_contiguous_subpages_for_heuristic Minimum number of 4 KB sub-pages
     *        within a 2 MB region that must satisfy VPN_offset == PPN_offset and share
     *        the same 2 MB physical base to infer a 2 MB huge page from v2p mappings.
     */
    explicit huge_page_detector_t(size_t min_contiguous_subpages_for_heuristic = 2);

    /**
     * @brief Destructor that closes /proc/kpageflags if opened.
     */
    virtual ~huge_page_detector_t();

    /**
     * @brief Explicitly marks a virtual address range [start_vaddr, end_vaddr) for a
     *        given process as backed by 2 MB or 1 GB huge pages.
     *
     * @param pid         Process ID (use 0 as a wildcard matching any PID if desired).
     * @param start_vaddr Start virtual address of the region.
     * @param end_vaddr   Exclusive end virtual address of the region.
     * @param kind        Huge page granularity (PAGE_SIZE_2MB or PAGE_SIZE_1GB).
     */
    void
    register_huge_page_region(memref_pid_t pid, addr_t start_vaddr, addr_t end_vaddr,
                              page_size_kind_t kind);

    /**
     * @brief Queries the page size kind (4 KB, 2 MB, or 1 GB) for a (pid, vaddr) pair.
     *
     * Checks 1 GB regions first, then 2 MB regions (both PID-specific and wildcard
     * PID 0), and returns PAGE_SIZE_4KB if no huge page covers the address.
     *
     * @param pid   Process ID of the memory access.
     * @param vaddr Virtual address of the memory access.
     * @return The resolved page_size_kind_t.
     */
    page_size_kind_t
    get_page_size_kind(memref_pid_t pid, addr_t vaddr) const;

    /**
     * @brief Observes a single 4 KB (virtual -> physical) mapping and applies the
     *        contiguous PFN alignment heuristic incrementally.
     *
     * When at least `min_contiguous_subpages_` distinct 4 KB pages within the same
     * 2 MB virtual slice map to the same 2 MB physical slice with identical intra-2MB
     * page offsets ((vaddr & 0x1fffff) == (paddr & 0x1fffff)), the 2 MB virtual slice
     * is automatically promoted to PAGE_SIZE_2MB.
     *
     * @param pid   Process ID (or 0 for global v2p map).
     * @param vaddr Virtual page address.
     * @param paddr Physical frame address.
     * @return True if this observation caused a 2 MB huge page promotion.
     */
    bool
    observe_v2p_mapping(memref_pid_t pid, addr_t vaddr, addr_t paddr);

    /**
     * @brief Bulk-analyzes a complete virtual-to-physical map (e.g., from v2p_reader_t)
     *        to detect 2 MB huge pages via the contiguous PFN heuristic.
     *
     * @param pid     Process ID (or 0 for global v2p map).
     * @param v2p_map Map of 4 KB virtual page start addresses to physical frame addresses.
     * @return Number of 2 MB huge pages detected and registered.
     */
    size_t
    analyze_v2p_map(memref_pid_t pid, const std::unordered_map<addr_t, addr_t> &v2p_map);

    /**
     * @brief Parses a Linux /proc/<pid>/smaps stream to detect VMAs backed by
     *        hugetlbfs (2 MB / 1 GB) or Transparent Huge Pages (AnonHugePages,
     *        ShmemPmdMapped, FilePmdMapped).
     *
     * @param pid          Process ID to associate with detected huge-page VMAs.
     * @param smaps_stream Input stream containing /proc/<pid>/smaps text.
     * @return Number of huge pages (2 MB or 1 GB) registered from the stream.
     */
    size_t
    parse_proc_smaps(memref_pid_t pid, std::istream &smaps_stream);

    /**
     * @brief Attempts to open /proc/kpageflags on Linux for live PFN huge-page queries.
     *
     * @return True if /proc/kpageflags was successfully opened (requires CAP_SYS_ADMIN).
     */
    bool
    open_kpageflags();

    /**
     * @brief Queries /proc/kpageflags for a given 4 KB Physical Frame Number (PFN).
     *
     * @param pfn         The 4 KB physical frame number (paddr >> 12).
     * @param is_huge_out Output boolean set to true if KPF_THP, KPF_HUGE, or
     *                    KPF_COMPOUND_HEAD/TAIL is set in /proc/kpageflags.
     * @return True if /proc/kpageflags was successfully read for `pfn`.
     */
    bool
    query_kpageflags_pfn(uint64_t pfn, bool *is_huge_out) const;

    /**
     * @brief Removes a huge-page registration (e.g., on munmap or huge-page split).
     *
     * @param pid   Process ID.
     * @param vaddr Virtual address inside the huge page to unregister.
     */
    void
    unregister_huge_page(memref_pid_t pid, addr_t vaddr);

    /**
     * @brief Returns the total number of 2 MB huge-page regions currently registered.
     */
    size_t
    get_num_2mb_regions() const;

    /**
     * @brief Returns the total number of 1 GB huge-page regions currently registered.
     */
    size_t
    get_num_1gb_regions() const;

private:
    // Tracks candidate 2 MB virtual slices during incremental v2p heuristic analysis.
    struct candidate_2mb_t {
        addr_t phys_base_2mb = 0;                 // Expected 2 MB-aligned physical base.
        bool has_conflict = false;                // True if any 4 KB subpage mismatched.
        std::unordered_set<addr_t> matched_vpns;  // Set of aligned 4 KB VPNs verified.
    };

    // Minimum matching 4 KB subpages needed to promote a 2 MB candidate.
    size_t min_contiguous_subpages_;
    // Per-PID set of 2 MB-aligned virtual base addresses backed by 2 MB huge pages.
    std::unordered_map<memref_pid_t, std::unordered_set<addr_t>> huge_2mb_pages_;
    // Per-PID set of 1 GB-aligned virtual base addresses backed by 1 GB huge pages.
    std::unordered_map<memref_pid_t, std::unordered_set<addr_t>> huge_1gb_pages_;
    // Per-PID map from 2 MB-aligned virtual base to candidate tracking state.
    std::unordered_map<memref_pid_t, std::unordered_map<addr_t, candidate_2mb_t>>
        candidates_2mb_;
    // File descriptor for /proc/kpageflags on Linux (-1 when closed/unavailable).
    int kpageflags_fd_;
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_HUGE_PAGE_DETECTOR_H_ */
