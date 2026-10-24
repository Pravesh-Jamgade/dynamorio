/* **********************************************************
 * Copyright (c) 2024-2026 Google, LLC  All rights reserved.
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
 * ARE DISCLAIMED. IN NO EVENT SHALL GOOGLE, LLC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

// Standard assertion macro.
#include <cassert>
// Standard integer types.
#include <cstdint>
// Standard file stream definitions.
#include <fstream>
// Standard I/O streams.
#include <iostream>
// Standard string stream definitions.
#include <sstream>
// Standard string class.
#include <string>
// Standard hash set container.
#include <unordered_set>
// Standard dynamic array container.
#include <vector>

// Unit test declaration header.
#include "tlb_simulator_unit_test.h"
// TLB simulator implementation header.
#include "../simulator/tlb_simulator.h"
// LRU data cache for external coupled PTW testing.
#include "../simulator/cache_lru.h"
// Cache statistics for external coupled PTW testing.
#include "../simulator/cache_stats.h"
// Memory reference structure definition.
#include "../common/memref.h"
// Trace entry type definitions.
#include "trace_entry.h"

// Open the top-level DynamoRIO namespace.
namespace dynamorio {
// Open the drmemtrace component namespace.
namespace drmemtrace {

// Mock subclass of tlb_simulator_t used to inspect processed addresses and internal state.
class tlb_simulator_mock_t : public tlb_simulator_t {
public:
    // Construct the mock simulator with the provided configuration knobs.
    tlb_simulator_mock_t(const tlb_simulator_knobs_t &knobs)
        // Forward knobs to the base tlb_simulator_t constructor.
        : tlb_simulator_t(knobs) {};

    // Intercept process_memref to record every virtual or physical address simulated.
    bool
    process_memref(const memref_t &memref) override
    {
        // Process memref like tlb_simulator does.
        bool result = tlb_simulator_t::process_memref(memref);
        // Save address used by tlb_simulator, if any.
        if (!type_has_address(memref.data.type)) {
            // Non-address records do not contribute to the address set.
            return result;
        }
        // Default to pointing at the incoming virtual memref.
        const memref_t *simref = &memref;
        // Temporary storage in case virtual-to-physical translation is active.
        memref_t phys_memref;
        // Convert to physical address when use_physical is enabled.
        if (knobs_.use_physical) {
            // Translate virtual memref to physical memref using v2p map.
            phys_memref = memref2phys(memref);
            // Point simref at the translated physical memref.
            simref = &phys_memref;
        }
        // Record the simulated address in our verification set.
        addresses.insert(simref->data.addr);
        // Return the result of tlb_simulator.
        return result;
    };

    // Helper to toggle use_physical on both tlb_simulator_t and base simulator_t.
    void
    set_knob_use_physical(bool set)
    {
        // Set tlb_simulator_t knob.
        knobs_.use_physical = set;
        // Set simulator_t knob.
        knob_use_physical_ = set;
    };

    // Set of unique addresses observed by process_memref.
    std::unordered_set<addr_t> addresses;
};

// Helper function to synthesize an 8-byte data read memref_t for a given address and PC.
static memref_t
generate_mem_ref(const addr_t addr, const addr_t pc, const memref_pid_t pid = 11111,
                 const memref_tid_t tid = 22222,
                 const trace_type_t type = TRACE_TYPE_READ)
{
    // Zero-initialize the memref_t union.
    memref_t memref = {};
    // Set the trace operation type (read, write, or instruction fetch).
    memref.data.type = type;
    // Set the simulated process identifier.
    memref.data.pid = pid;
    // Set the simulated thread identifier.
    memref.data.tid = tid;
    // Set the target virtual address.
    memref.data.addr = addr;
    // Set the access width to 8 bytes.
    memref.data.size = 8;
    // Set the instruction program counter.
    memref.data.pc = pc;
    // Return the populated memref_t.
    return memref;
}

// Checks that the addresses the tlb simulator uses (in memrefs) are the same we expect
// in addresses.
static std::string
check_addresses(const std::vector<memref_t> &memrefs,
                const std::unordered_set<addr_t> &addresses,
                const std::string &v2p_file_path = "")
{
    // Use default TLB simulator knobs.
    tlb_simulator_knobs_t knobs;
    // Instantiate the mock TLB simulator.
    tlb_simulator_mock_t tlb_simulator_mock(knobs);

    // If a v2p.textproto file path was supplied, load it and enable physical mode.
    if (!v2p_file_path.empty()) {
        // Enable physical addressing mode.
        tlb_simulator_mock.set_knob_use_physical(true);
        // Open the v2p file stream.
        std::ifstream fin;
        // Open the specified file path.
        fin.open(v2p_file_path);
        // Verify that the file opened cleanly.
        if (!fin.is_open())
            return "Failed to open the v2p file '" + v2p_file_path + "'\n";
        // Populate the simulator's v2p map from the file stream.
        std::string error_str = tlb_simulator_mock.create_v2p_from_file(fin);
        // Close the file stream.
        fin.close();
        // Propagate any v2p parsing error.
        if (!error_str.empty())
            return "ERROR: v2p_reader failed with: " + error_str + "\n";
    }

    // Feed all test memory references into the simulator.
    for (const memref_t &memref : memrefs)
        tlb_simulator_mock.process_memref(memref);

    // Verify that the count of observed addresses matches expectations.
    if (addresses.size() != tlb_simulator_mock.addresses.size()) {
        return "ERROR: size mismatch. Expected " + std::to_string(addresses.size()) +
            " got " + std::to_string(tlb_simulator_mock.addresses.size()) + ".\n";
    }

    // Verify that every expected address was observed.
    for (const addr_t addr : addresses) {
        if (tlb_simulator_mock.addresses.count(addr) == 0)
            return "ERROR: address " + std::to_string(addr) + " not found.\n";
    }

    // Return empty string on success.
    return "";
}

// Legacy test checking virtual and v2p physical address handling.
static void
tlb_simulator_check_addresses(const std::string &testdir)
{
    // virtual_addresses and physical_addresses are taken from an X64 trace, hence we
    // enable this test only on 64-bit hosts.
#if defined(X86_64) || defined(AARCH64)
    // Path to the test v2p.textproto file.
    const std::string v2p_file_path =
        testdir + "/drmemtrace.threadsig.aarch64.raw/v2p.textproto";
    // Expected virtual addresses.
    const std::unordered_set<addr_t> virtual_addresses = { 0x0000fffffb73da60,
                                                           0x00000000004a7a78,
                                                           0x00000000004a5f20 };
    // Expected translated physical addresses from v2p.textproto.
    const std::unordered_set<addr_t> physical_addresses = { 0x000000000133da60,
                                                            0x00000000002a7a78,
                                                            0x00000000002a5f20 };
    // Build the vector of input memrefs.
    std::vector<memref_t> memrefs;
    // Dummy instruction PC counter.
    addr_t pc = 0;
    // Populate one memref per virtual address.
    for (const addr_t &addr : virtual_addresses) {
        memrefs.push_back(generate_mem_ref(addr, pc));
        pc += 8;
    }

    // Check that the addresses the tlb_simulator operates with are the same virtual
    // addresses we created the memrefs with.
    std::string error_str = check_addresses(memrefs, virtual_addresses);
    if (!error_str.empty()) {
        std::cerr << "ERROR: tlb_simulator_unit_test with virtual_addresses failed with: "
                  << error_str << "\n";
        exit(1);
    }

    // Check that the addresses the tlb_simulator operates with are the same physical
    // addresses we expect.
    error_str = check_addresses(memrefs, physical_addresses, v2p_file_path);
    if (!error_str.empty()) {
        std::cerr
            << "ERROR: tlb_simulator_unit_test with physical_addresses failed with: "
            << error_str << "\n";
        exit(1);
    }
#endif
}

// Test 1: 4-Level Radix Page Table Walk (PML4 -> PDPT -> PD -> PT) & Page Walk Cache (PWC).
static void
unit_test_radix_ptw_and_pwc()
{
    // Configure a 1-core TLB simulator with realistic PTW and PWC enabled.
    tlb_simulator_knobs_t knobs;
    // Single simulated core.
    knobs.num_cores = 1;
    // 4KB base page size.
    knobs.page_size = 4096;
    // Enable 4-level radix page table walk on L2 TLB misses.
    knobs.enable_page_table_walk = true;
    // Disable huge pages for this pure 4KB walk test.
    knobs.enable_huge_pages = false;
    // Enable a 32-entry, 4-way associative Page Walk Cache.
    knobs.TLB_pwc_entries = 32;
    knobs.TLB_pwc_assoc = 4;
    // Use LRU replacement in the TLBs.
    knobs.TLB_replace_policy = "LRU";

    // Create the TLB simulator instance.
    tlb_simulator_t sim(knobs);

    // First access: virtual address 0x00007fff00001000 (4KB page #1 in a 2MB region).
    // Because the PWC and TLBs are cold, this L2 TLB miss must walk all 4 levels:
    // L4 (PML4E) -> L3 (PDPTE) -> L2 (PDE) -> L1 (PTE), incurring 4 PTW memory reads
    // and 1 demand page fault.
    const addr_t vaddr1 = 0x00007fff00001000ULL;
    // Process the first memory read.
    bool ok = sim.process_memref(generate_mem_ref(vaddr1, 0x400000));
    // Verify process_memref succeeded.
    assert(ok);

    // Verify that the VPN -> PPN translation is installed in core 0's TLB hierarchy.
    addr_t ppn1 = 0;
    page_size_kind_t kind1 = page_size_kind_t::PAGE_SIZE_2MB;
    assert(sim.lookup_tlb_entry(0, 11111, vaddr1, /*is_instr=*/false, &ppn1, &kind1));
    assert(ppn1 != 0);
    assert(kind1 == page_size_kind_t::PAGE_SIZE_4KB);

    // Fetch core 0's MMU statistics after the first walk.
    const mmu_stats_t *stats = sim.get_mmu_stats(0);
    // Ensure the stats pointer is valid.
    assert(stats != nullptr);
    // Exactly 1 total page table walk must have occurred.
    assert(stats->get_total_ptw_walks() == 1);
    // It must be classified as a 4KB page walk.
    assert(stats->get_walks_4kb() == 1);
    // Exactly 1 demand page fault must have occurred to allocate the leaf PFN.
    assert(stats->get_page_faults() == 1);
    // All 4 radix levels (L4, L3, L2, L1) must have been read from memory.
    assert(stats->get_ptw_reads_at_level(ptw_level_t::PTW_LEVEL_4_PML4E) == 1);
    assert(stats->get_ptw_reads_at_level(ptw_level_t::PTW_LEVEL_3_PDPTE) == 1);
    assert(stats->get_ptw_reads_at_level(ptw_level_t::PTW_LEVEL_2_PDE) == 1);
    assert(stats->get_ptw_reads_at_level(ptw_level_t::PTW_LEVEL_1_PTE) == 1);
    assert(stats->get_total_ptw_memory_reads() == 4);
    // The PWC was cold, so 0 PWC hits on the first walk.
    assert(stats->get_pwc_hits() == 0);

    // Re-accessing the exact same 4KB page must hit in the L1 D-TLB without a walk.
    ok = sim.process_memref(generate_mem_ref(vaddr1 + 0x128, 0x400008));
    assert(ok);
    // Page table walk count must remain 1.
    assert(stats->get_total_ptw_walks() == 1);

    // Second distinct 4KB page in the SAME 2MB aligned region: 0x00007fff00002000.
    // It shares L4 (PML4E), L3 (PDPTE), and L2 (PDE) entries with vaddr1!
    // Therefore, the Page Walk Cache must hit at L2 (PDE), skipping L4, L3, and L2
    // memory reads and issuing only 1 memory read at L1 (PTE)!
    const addr_t vaddr2 = 0x00007fff00002000ULL;
    ok = sim.process_memref(generate_mem_ref(vaddr2, 0x400010));
    assert(ok);

    // Total walks must now be 2.
    assert(stats->get_total_ptw_walks() == 2);
    // Demand page faults must now be 2 (one for vaddr1, one for vaddr2).
    assert(stats->get_page_faults() == 2);
    // PWC hits must now be 1 (hit at L2 PDE).
    assert(stats->get_pwc_hits() == 1);
    assert(stats->get_pwc_hits_at_level(ptw_level_t::PTW_LEVEL_2_PDE) == 1);
    // Upper levels L4, L3, L2 must NOT have issued additional memory reads (still 1 each).
    assert(stats->get_ptw_reads_at_level(ptw_level_t::PTW_LEVEL_4_PML4E) == 1);
    assert(stats->get_ptw_reads_at_level(ptw_level_t::PTW_LEVEL_3_PDPTE) == 1);
    assert(stats->get_ptw_reads_at_level(ptw_level_t::PTW_LEVEL_2_PDE) == 1);
    // Only L1 (PTE) issued a second memory read (now 2).
    assert(stats->get_ptw_reads_at_level(ptw_level_t::PTW_LEVEL_1_PTE) == 2);
    // Total memory reads across the 2 walks is 4 + 1 = 5 (instead of 8 without PWC).
    assert(stats->get_total_ptw_memory_reads() == 5);
}

// Test 2: Huge Page Detection (2MB & 1GB), smaps Parsing, and Shortened Walks.
static void
unit_test_huge_pages_and_shortened_walks()
{
    // Configure a 1-core TLB simulator with both PTW and Huge Pages enabled.
    tlb_simulator_knobs_t knobs;
    knobs.num_cores = 1;
    knobs.page_size = 4096;
    knobs.enable_page_table_walk = true;
    knobs.enable_huge_pages = true;
    knobs.TLB_pwc_entries = 32;
    knobs.TLB_pwc_assoc = 4;
    knobs.TLB_replace_policy = "LRU";

    // Instantiate the TLB simulator.
    tlb_simulator_t sim(knobs);
    // Target test process ID.
    const memref_pid_t pid = 11111;

    // Retrieve the shared MMU address space manager and its huge-page detector.
    mmu_address_space_manager_t *mgr = sim.get_address_space_manager();
    assert(mgr != nullptr);
    huge_page_detector_t &detector = mgr->get_huge_page_detector();

    // 1. Register a 2MB huge-page region via synthetic /proc/<pid>/smaps stream parsing:
    //    Range [0x00007f0000200000, 0x00007f0000400000) backed by AnonHugePages (THP).
    std::istringstream smaps_input(
        "7f0000200000-7f0000400000 rw-p 00000000 00:00 0 \n"
        "Size:               2048 kB\n"
        "KernelPageSize:        4 kB\n"
        "MMUPageSize:           4 kB\n"
        "AnonHugePages:      2048 kB\n");
    // Parse the synthetic smaps stream for PID 11111.
    size_t registered = detector.parse_proc_smaps(pid, smaps_input);
    // Verify that 1 huge-page VMA was extracted.
    assert(registered == 1);

    // 2. Register an explicit 1GB giant-page region at [0x00007fc000000000, 0x00007fc040000000).
    const addr_t giant_base = 0x00007fc000000000ULL;
    detector.register_huge_page_region(pid, giant_base,
                                       giant_base + mmu_constants_t::PAGE_SIZE_1GB_BYTES,
                                       page_size_kind_t::PAGE_SIZE_1GB);

    // Access the first 4KB sub-page of the 2MB THP region (0x00007f0000200000).
    // Because it is a 2MB page, the PTW stops at L2 (PDE): only 3 levels walked (L4, L3, L2)!
    const addr_t thp_base = 0x00007f0000200000ULL;
    assert(sim.process_memref(generate_mem_ref(thp_base, 0x500000, pid)));

    // Inspect MMU statistics.
    const mmu_stats_t *stats = sim.get_mmu_stats(0);
    assert(stats != nullptr);
    // 1 walk of a 2MB huge page must be recorded.
    assert(stats->get_walks_2mb() == 1);
    assert(stats->get_walks_4kb() == 0);
    // Only 3 memory reads (L4, L3, L2) — L1 PTE was never touched!
    assert(stats->get_total_ptw_memory_reads() == 3);
    assert(stats->get_ptw_reads_at_level(ptw_level_t::PTW_LEVEL_1_PTE) == 0);

    // Now access 5 different 4KB sub-pages scattered across the same 2MB huge page
    // (e.g., +4KB, +64KB, +512KB, +1MB, +2044KB).
    // Because the TLB entry covers the entire 2MB region, ALL of them must HIT in the
    // L1 D-TLB and trigger ZERO additional page table walks!
    for (addr_t offset : { 0x1000ULL, 0x10000ULL, 0x80000ULL, 0x100000ULL, 0x1ff000ULL }) {
        assert(sim.process_memref(generate_mem_ref(thp_base + offset, 0x500008, pid)));
    }
    // Verify that NO additional page table walks occurred!
    assert(stats->get_total_ptw_walks() == 1);
    assert(stats->get_walks_2mb() == 1);

    // Now access the 1GB giant page region at giant_base + 0x12345000 (an interior 4KB offset).
    // Because it is a 1GB page, the PTW stops at L3 (PDPTE): at most 2 levels (L4, L3).
    assert(sim.process_memref(generate_mem_ref(giant_base + 0x12345000ULL, 0x500010, pid)));
    // Verify that 1 1GB page table walk was recorded.
    assert(stats->get_walks_1gb() == 1);

    // Accessing another interior address 512MB away inside the same 1GB page must hit in the TLB!
    assert(sim.process_memref(generate_mem_ref(giant_base + 0x20000000ULL, 0x500018, pid)));
    // Still only 1 1GB walk!
    assert(stats->get_walks_1gb() == 1);
}

// Test 3: Contiguous PFN Alignment Heuristic from v2p mappings.
static void
unit_test_v2p_huge_page_heuristic()
{
    // Verify huge_page_detector_t's contiguous physical frame heuristic directly.
    huge_page_detector_t detector(/*min_contiguous_subpages_for_heuristic=*/2);
    // Target PID.
    const memref_pid_t pid = 9999;
    // Choose a 2MB-aligned virtual base: 0x0000555500200000.
    const addr_t vbase = 0x0000555500200000ULL;
    // Choose a 2MB-aligned physical base: 0x0000000040200000.
    const addr_t pbase = 0x0000000040200000ULL;

    // Feed two 4KB page mappings within this 2MB window that share the exact same
    // 2MB physical base and identical intra-2MB page offsets.
    bool promoted1 = detector.observe_v2p_mapping(pid, vbase + 0x00000, pbase + 0x00000);
    assert(!promoted1);
    bool promoted2 = detector.observe_v2p_mapping(pid, vbase + 0x15000, pbase + 0x15000);
    assert(promoted2);

    // The heuristic must now classify any virtual address inside [vbase, vbase + 2MB)
    // as PAGE_SIZE_2MB!
    assert(detector.get_page_size_kind(pid, vbase + 0x99000) ==
           page_size_kind_t::PAGE_SIZE_2MB);
    assert(detector.get_num_2mb_regions() == 1);
}

// Test 4: Coupled PTW-to-Data-Cache Simulation (Internal & External L1 Data Cache).
static void
unit_test_coupled_ptw_data_cache()
{
    // Configure a 1-core TLB simulator with PTW and coupled L1 data cache enabled,
    // and disable the Page Walk Cache (TLB_pwc_entries = 0) so every walk issues all
    // 4 physical PTE reads to the coupled L1 data cache.
    tlb_simulator_knobs_t knobs;
    knobs.num_cores = 1;
    knobs.page_size = 4096;
    knobs.enable_page_table_walk = true;
    knobs.enable_huge_pages = false;
    // Disable PWC so we can observe all 4 PTE reads hitting/missing in the data cache.
    knobs.TLB_pwc_entries = 0;
    knobs.couple_ptw_to_dcache = true;
    knobs.ptw_dcache_size = 32 * 1024;
    knobs.ptw_dcache_assoc = 8;
    knobs.ptw_dcache_line_size = 64;

    // Create the simulator.
    tlb_simulator_t sim(knobs);

    // Attach an external cache_lru_t via set_core_ptw_data_cache() to also verify
    // external cache hierarchy coupling.
    cache_lru_t external_dcache("ext_ptw_dcache");
    cache_stats_t external_stats(/*block_size=*/64);
    bool cache_ok = external_dcache.init(/*associativity=*/8, /*block_size=*/64,
                                         /*total_size=*/32768, /*parent=*/nullptr,
                                         &external_stats);
    assert(cache_ok);
    sim.set_core_ptw_data_cache(0, &external_dcache);

    // First walk on vaddr1 = 0x00007fff10000000:
    // Reads 4 distinct 4KB page-table frames (PML4, PDPT, PD, PT).
    // All 4 PTE cache lines are cold in the data cache -> 4 data-cache misses, 0 hits.
    const addr_t vaddr1 = 0x00007fff10000000ULL;
    assert(sim.process_memref(generate_mem_ref(vaddr1, 0x600000)));

    const mmu_stats_t *stats = sim.get_mmu_stats(0);
    assert(stats != nullptr);
    assert(stats->get_ptw_cache_misses() == 4);
    assert(stats->get_ptw_cache_hits() == 0);

    // Second walk on adjacent 4KB page vaddr2 = 0x00007fff10001000:
    // Because PWC is disabled, the PTW again reads PML4E, PDPTE, PDE, and PTE from the
    // coupled data cache.
    // - PML4E, PDPTE, and PDE are the EXACT same 8-byte entries as vaddr1 -> 3 hits!
    // - The L1 PTE is at index 1 (offset 8 bytes) in the SAME 64-byte cache line as
    //   index 0 (offset 0 bytes) fetched by vaddr1 -> 4th hit!
    // Thus all 4 PTE reads on the second walk MUST HIT in the coupled L1 data cache!
    const addr_t vaddr2 = 0x00007fff10001000ULL;
    assert(sim.process_memref(generate_mem_ref(vaddr2, 0x600008)));

    assert(stats->get_ptw_cache_misses() == 4);
    assert(stats->get_ptw_cache_hits() == 4);
}

// Test 5: TLB Replacement Policies (LRU, LFU, FIFO) and Cross-Core TLB Shootdowns.
static void
unit_test_replacement_policies_and_shootdowns()
{
    // Verify that LFU, LRU, and FIFO replacement policies all construct and run cleanly,
    // and that perform_tlb_shootdown() invalidates translations across cores.
    for (const std::string &policy : { std::string("LFU"), std::string("LRU"),
                                       std::string("FIFO") }) {
        tlb_simulator_knobs_t knobs;
        knobs.num_cores = 2;
        knobs.page_size = 4096;
        knobs.TLB_L1D_entries = 4;
        knobs.TLB_L1D_assoc = 4;
        knobs.TLB_L2_entries = 8;
        knobs.TLB_L2_assoc = 4;
        knobs.TLB_replace_policy = policy;
        knobs.enable_page_table_walk = true;
        knobs.enable_huge_pages = false;

        tlb_simulator_t sim(knobs);
        const memref_pid_t pid = 4242;

        // Access 10 distinct 4KB pages to exercise L1 and L2 TLB eviction under `policy`.
        for (int i = 0; i < 10; ++i) {
            addr_t vaddr = 0x00007f8800000000ULL + static_cast<addr_t>(i) * 4096ULL;
            assert(sim.process_memref(generate_mem_ref(vaddr, 0x700000, pid)));
        }

        const mmu_stats_t *stats = sim.get_mmu_stats(0);
        assert(stats != nullptr);
        assert(stats->get_total_ptw_walks() == 10);

        // Perform a TLB shootdown on the most recently accessed page (i = 9).
        addr_t last_vaddr = 0x00007f8800000000ULL + 9ULL * 4096ULL;
        sim.perform_tlb_shootdown(pid, last_vaddr);
        assert(stats->get_tlb_shootdowns() == 1);

        // Accessing `last_vaddr` again must miss in both L1 and L2 TLBs and trigger an
        // 11th page table walk!
        assert(sim.process_memref(generate_mem_ref(last_vaddr, 0x700008, pid)));
        assert(stats->get_total_ptw_walks() == 11);

        // Also verify print_results() executes cleanly with zero errors.
        assert(sim.print_results());
    }
}

// Entry point for all TLB simulator unit tests.
void
unit_test_tlb_simulator(const std::string &testdir)
{
    // Run legacy virtual/physical address verification test.
    tlb_simulator_check_addresses(testdir);
    // Run 4-level radix PTW and Page Walk Cache unit test.
    unit_test_radix_ptw_and_pwc();
    // Run 2MB/1GB huge page detection, smaps parser, and shortened walk unit test.
    unit_test_huge_pages_and_shortened_walks();
    // Run v2p contiguous PFN huge-page heuristic unit test.
    unit_test_v2p_huge_page_heuristic();
    // Run coupled PTW-to-data-cache simulation unit test.
    unit_test_coupled_ptw_data_cache();
    // Run replacement policies (LFU, LRU, FIFO) and TLB shootdown unit test.
    unit_test_replacement_policies_and_shootdowns();
}

} // namespace drmemtrace
} // namespace dynamorio
