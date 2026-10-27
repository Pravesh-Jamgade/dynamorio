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
// Speculative wrong-path execution engine.
#include "../simulator/cpu/wrong_path.h"
// ChampSim and CloudSuite trace reader adapter.
#include "../reader/champsim_reader.h"
// Miss Status Holding Register (MSHR) table.
#include "../simulator/cpu/mshr.h"
// Cache Read, Write, and Prefetch queues.
#include "../simulator/cpu/cache_queues.h"
// INI configuration parser.
#include "../simulator/cpu/ini_parser.h"
// Complete Out-of-Order CPU and 3-level cache simulator.
#include "../simulator/cpu/cpu_simulator.h"
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
    // Verify per-level cache hit breakdown for cold walk: all 4 levels went to DRAM.
    assert(stats->get_ptw_cache_hits_dram() == 4); // DRAM serviced all 4 cold PTE reads.
    assert(stats->get_ptw_cache_hits_l1d() == 0);  // No L1D hits during initial cold walk.
    assert(stats->get_level_cache_hits(ptw_level_t::PTW_LEVEL_4_PML4E,
                                       ptw_cache_hit_level_t::HIT_DRAM) == 1); // L4 missed to DRAM.
    assert(stats->get_level_cache_hits(ptw_level_t::PTW_LEVEL_3_PDPTE,
                                       ptw_cache_hit_level_t::HIT_DRAM) == 1); // L3 missed to DRAM.
    assert(stats->get_level_cache_hits(ptw_level_t::PTW_LEVEL_2_PDE,
                                       ptw_cache_hit_level_t::HIT_DRAM) == 1); // L2 missed to DRAM.
    assert(stats->get_level_cache_hits(ptw_level_t::PTW_LEVEL_1_PTE,
                                       ptw_cache_hit_level_t::HIT_DRAM) == 1); // L1 missed to DRAM.

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
    // Verify per-level cache hit breakdown for warm walk: all 4 levels hit in L1D cache.
    assert(stats->get_ptw_cache_hits_l1d() == 4);  // 4 total L1D hits after warm walk.
    assert(stats->get_ptw_cache_hits_dram() == 4); // Still 4 DRAM accesses total.
    assert(stats->get_level_cache_hits(ptw_level_t::PTW_LEVEL_4_PML4E,
                                       ptw_cache_hit_level_t::HIT_L1D) == 1);  // L4 hit in L1D.
    assert(stats->get_level_cache_hits(ptw_level_t::PTW_LEVEL_3_PDPTE,
                                       ptw_cache_hit_level_t::HIT_L1D) == 1);  // L3 hit in L1D.
    assert(stats->get_level_cache_hits(ptw_level_t::PTW_LEVEL_2_PDE,
                                       ptw_cache_hit_level_t::HIT_L1D) == 1);  // L2 hit in L1D.
    assert(stats->get_level_cache_hits(ptw_level_t::PTW_LEVEL_1_PTE,
                                       ptw_cache_hit_level_t::HIT_L1D) == 1);  // L1 hit in L1D.
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
        knobs.TLB_L1I_entries = 4;
        knobs.TLB_L1I_assoc = 4;
        knobs.TLB_L1D_entries = 4;
        knobs.TLB_L1D_assoc = 4;
        knobs.TLB_L2_entries = 8;
        knobs.TLB_L2_assoc = 4;
        knobs.TLB_replace_policy = policy;
        knobs.enable_page_table_walk = true;
        knobs.enable_huge_pages = false;

        tlb_simulator_t sim(knobs);
        const memref_pid_t pid = 4242;

        // Issue 5 distinct instruction fetches followed by immediate repeat accesses on core 0 (tid=22222).
        // First access per page must miss in L1 ITLB, and repeat access must hit in L1 ITLB.
        for (int i = 0; i < 5; ++i) {
            // Compute unique 4KB aligned virtual instruction address.
            addr_t ipc = 0x0000000000400000ULL + static_cast<addr_t>(i) * 4096ULL;
            // First fetch triggers compulsory ITLB miss and page table walk on core 0.
            assert(sim.process_memref(generate_mem_ref(ipc, ipc, pid, 22222, TRACE_TYPE_INSTR)));
            // Second fetch triggers L1 ITLB hit on core 0.
            assert(sim.process_memref(generate_mem_ref(ipc, ipc, pid, 22222, TRACE_TYPE_INSTR)));
        }

        // Access 10 distinct 4KB pages to exercise L1 and L2 TLB eviction under `policy`.
        for (int i = 0; i < 10; ++i) {
            addr_t vaddr = 0x00007f8800000000ULL + static_cast<addr_t>(i) * 4096ULL;
            assert(sim.process_memref(generate_mem_ref(vaddr, 0x700000, pid)));
        }

        // Verify L1 ITLB recorded hits and misses accurately.
        const tlb_stats_t *itlb_stats = sim.get_tlb_stats(0, 1, /*is_instr=*/true);
        // Verify ITLB stats pointer is valid.
        assert(itlb_stats != nullptr);
        // Verify 5 ITLB hits from repeated accesses.
        assert(itlb_stats->get_metric(metric_name_t::HITS) == 5);
        // Verify 5 ITLB misses from first accesses.
        assert(itlb_stats->get_metric(metric_name_t::MISSES) == 5);

        const mmu_stats_t *stats = sim.get_mmu_stats(0);
        assert(stats != nullptr);
        // 5 instruction page walks + 10 data page walks = 15 total walks.
        assert(stats->get_total_ptw_walks() == 15);

        // Perform a TLB shootdown on the most recently accessed page (i = 9).
        addr_t last_vaddr = 0x00007f8800000000ULL + 9ULL * 4096ULL;
        sim.perform_tlb_shootdown(pid, last_vaddr);
        assert(stats->get_tlb_shootdowns() == 1);

        // Accessing `last_vaddr` again must miss in both L1 and L2 TLBs and trigger an
        // 16th page table walk!
        assert(sim.process_memref(generate_mem_ref(last_vaddr, 0x700008, pid)));
        assert(stats->get_total_ptw_walks() == 16);

        // Also verify print_results() executes cleanly with zero errors.
        assert(sim.print_results());
    }
}

// Test 6: Performance Model, Total Instructions, Total Cycles, CPI Stalls, and IPC.
static void
unit_test_performance_model_cpi_ipc()
{
    // 1. Direct mathematical verification of performance_model_t.
    performance_params_t params;
    params.cpi_base = 0.5;
    params.l2_tlb_hit_latency = 10;
    params.pwc_hit_latency = 4;
    params.dram_latency = 100;
    params.dcache_hit_latency = 4;
    params.dcache_miss_latency = 100;

    performance_model_t model(params);

    // Mock stats:
    // L1 TLB with 50 misses.
    tlb_stats_t mock_l1_stats(4096);
    memref_t dummy_ref = {};
    dummy_ref.data.type = TRACE_TYPE_READ;
    dummy_ref.data.size = 8;
    for (int i = 0; i < 50; ++i) {
        mock_l1_stats.access(dummy_ref, /*hit=*/false, nullptr);
    }
    // Coupled data cache with 5 misses.
    cache_stats_t mock_dcache_stats(64);
    for (int i = 0; i < 5; ++i) {
        mock_dcache_stats.access(dummy_ref, /*hit=*/false, nullptr);
    }

    // MMU stats with 1 walk of 4 levels, 0 PWC hits, 4 memory reads to DRAM.
    mmu_stats_t mock_mmu_stats;
    ptw_result_t mock_ptw;
    mock_ptw.success = true;
    mock_ptw.page_size_kind = page_size_kind_t::PAGE_SIZE_4KB;
    mock_ptw.is_page_fault = true;
    mock_ptw.num_memory_reads = 4;
    for (int lvl = 4; lvl >= 1; --lvl) {
        ptw_step_record_t step;
        step.level = static_cast<ptw_level_t>(lvl);
        step.pwc_hit = false;
        mock_ptw.steps.push_back(step);
    }
    mock_mmu_stats.record_walk(mock_ptw);

    // Evaluate 1000 instructions.
    const int64_t instrs = 1000;
    performance_metrics_t m =
        model.evaluate(instrs, &mock_l1_stats, nullptr, nullptr,
                       &mock_mmu_stats, &mock_dcache_stats);

    // Assert exact values:
    // Base cycles = round(1000 * 0.5) = 500.
    assert(m.total_instructions == 1000);
    assert(m.base_cycles == 500);
    // Cache stall cycles = 5 misses * 100 = 500.
    assert(m.cache_stall_cycles == 500);
    // TLB stall cycles = (50 L1 misses * 10) + (4 PTW reads * 100) = 500 + 400 = 900.
    assert(m.tlb_stall_cycles == 900);
    // Total cycles = 500 + 500 + 900 = 1900.
    assert(m.total_cycles == 1900);
    // CPI base = 0.5.
    assert(m.cpi_base == 0.5);
    // CPI cache stalls = 500 / 1000 = 0.5.
    assert(std::abs(m.cpi_cache_stalls - 0.5) < 1e-6);
    // CPI TLB stalls = 900 / 1000 = 0.9.
    assert(std::abs(m.cpi_tlb_stalls - 0.9) < 1e-6);
    // Total CPI = 0.5 + 0.5 + 0.9 = 1.9.
    assert(std::abs(m.cpi_total - 1.9) < 1e-6);
    // IPC = 1000 / 1900 = 0.5263157...
    assert(std::abs(m.ipc - (1000.0 / 1900.0)) < 1e-6);

    // 2. End-to-end integration test with tlb_simulator_t.
    tlb_simulator_knobs_t knobs;
    knobs.num_cores = 1;
    knobs.page_size = 4096;
    knobs.enable_page_table_walk = true;
    knobs.enable_performance_model = true;
    knobs.cpi_base = 0.5;
    knobs.l2_tlb_hit_latency = 9;
    knobs.dram_latency = 150;

    tlb_simulator_t sim(knobs);
    const memref_pid_t pid = 8888;

    // Issue 20 instruction fetch memrefs and 10 data read memrefs.
    for (int i = 0; i < 20; ++i) {
        addr_t pc = 0x400000ULL + static_cast<addr_t>(i) * 4ULL;
        assert(sim.process_memref(generate_mem_ref(pc, pc, pid, 1, TRACE_TYPE_INSTR)));
    }
    for (int i = 0; i < 10; ++i) {
        addr_t addr = 0x7fff00000000ULL + static_cast<addr_t>(i) * 4096ULL;
        assert(sim.process_memref(generate_mem_ref(addr, 0x400010, pid, 1, TRACE_TYPE_READ)));
    }

    // Query core 0's computed performance metrics.
    performance_metrics_t core_m = sim.get_core_performance_metrics(0);
    // Exactly 20 instructions must be recorded.
    assert(core_m.total_instructions == 20);
    // Total cycles must be strictly greater than ideal base cycles (20 * 0.5 = 10).
    assert(core_m.total_cycles > 10);
    // TLB stall cycles must be positive due to cold L1/L2 misses and page table walks.
    assert(core_m.tlb_stall_cycles > 0);
    // Total CPI must be strictly greater than cpi_base.
    assert(core_m.cpi_total > knobs.cpi_base);
    // IPC must equal total_instructions / total_cycles.
    assert(std::abs(core_m.ipc - (20.0 / static_cast<double>(core_m.total_cycles))) < 1e-6);

    // Verify print_results prints the performance & timing model cleanly.
    assert(sim.print_results());
}

// Test 7: Three-Level Data Cache Hierarchy (L1D -> L2 -> L3/LLC) and PTW Accesses.
static void
unit_test_three_level_cache()
{
    three_level_cache_knobs_t knobs;
    knobs.num_cores = 1;
    knobs.line_size = 64;
    knobs.l1d_size = 4096;      // 4 KB L1D
    knobs.l1d_assoc = 2;
    knobs.l2_size = 16384;      // 16 KB L2
    knobs.l2_assoc = 4;
    knobs.l3_size = 65536;      // 64 KB L3
    knobs.l3_assoc = 8;
    knobs.l1d_latency = 4;
    knobs.l2_latency = 12;
    knobs.l3_latency = 35;
    knobs.dram_latency = 150;

    three_level_cache_t cache(knobs);

    // 1. Cold access to 0x1000 misses L1D, L2, L3 and hits DRAM!
    memref_t ref1 = generate_mem_ref(0x1000, 0x400000);
    cache_access_result_t res1 = cache.request(0, ref1);
    assert(res1.level == cache_hit_level_t::HIT_DRAM);
    assert(res1.latency_cycles == 4 + 12 + 35 + 150);

    // 2. Immediate re-access to 0x1000 must HIT in L1D!
    cache_access_result_t res2 = cache.request(0, ref1);
    assert(res2.level == cache_hit_level_t::HIT_L1D);
    assert(res2.latency_cycles == 4);

    // 3. Hardware Page Table Walker 8-byte PTE read to 0x2000 (cold -> DRAM).
    cache_access_result_t ptw_res1 = cache.ptw_request(0, 0x2000);
    assert(ptw_res1.level == cache_hit_level_t::HIT_DRAM);

    // 4. Second PTW access to 0x2008 (same 64B cache line as 0x2000) must HIT in L1D!
    cache_access_result_t ptw_res2 = cache.ptw_request(0, 0x2008);
    assert(ptw_res2.level == cache_hit_level_t::HIT_L1D);
    assert(ptw_res2.latency_cycles == 4);

    // 5. Instruction fetch to 0x3000 (cold -> DRAM fill into L1I).
    memref_t ifetch1 = generate_mem_ref(0x3000, 0x3000, 1, 1, TRACE_TYPE_INSTR);
    cache_access_result_t ires1 = cache.request_instruction(0, ifetch1);
    assert(ires1.level == cache_hit_level_t::HIT_DRAM);
    assert(ires1.latency_cycles == knobs.l1i_latency + knobs.l2_latency + knobs.l3_latency + knobs.dram_latency);

    // 6. Immediate instruction fetch re-access to 0x3000 must HIT in L1I!
    cache_access_result_t ires2 = cache.request_instruction(0, ifetch1);
    assert(ires2.level == cache_hit_level_t::HIT_L1I);
    assert(ires2.latency_cycles == knobs.l1i_latency);
    assert(cache.get_l1i_cache(0) != nullptr);
}

// Test 8: Branch Target Buffer (BTB), Branch Predictor, and Return Address Stack (RAS).
static void
unit_test_btb_and_branch_predictor()
{
    branch_predictor_t bp(/*btb_entries=*/128, /*bimodal_entries=*/256, /*ras_depth=*/16);

    // 1. Conditional branch at PC 0x401000 with target 0x401050.
    addr_t br_pc = 0x401000;
    addr_t target = 0x401050;

    // First execution: bimodal starts at Weakly Taken (2), BTB is cold -> predicts taken, target 0.
    branch_prediction_t pred1 = bp.update(br_pc, cpu_op_type_t::OP_BRANCH_COND,
                                          /*actual_taken=*/true, target);
    assert(pred1.actual_taken == true);

    // Second execution of same branch taken: BTB should now predict target 0x401050!
    branch_prediction_t pred2 = bp.update(br_pc, cpu_op_type_t::OP_BRANCH_COND,
                                          /*actual_taken=*/true, target);
    assert(pred2.predicted_taken == true);
    assert(pred2.predicted_target == target);
    assert(!pred2.is_mispredicted());

    // 2. Function Call and Return Address Stack (RAS).
    addr_t call_pc = 0x402000;
    addr_t call_target = 0x500000;
    addr_t ret_pc = 0x500040;
    addr_t expected_ret = call_pc + 5; // Call instruction length 5 bytes.

    // Encounter CALL: pushes expected_ret to RAS.
    bp.update(call_pc, cpu_op_type_t::OP_CALL, /*actual_taken=*/true,
              call_target, /*return_addr=*/expected_ret);

    // Encounter RETURN: pops from RAS.
    branch_prediction_t ret_pred = bp.predict(ret_pc, cpu_op_type_t::OP_RETURN);
    assert(ret_pred.predicted_target == expected_ret);
}

// Test 9: Reorder Buffer (ROB) and Register Dependency Scoreboard.
static void
unit_test_rob_and_reg_deps()
{
    // Configure a small 4-entry ROB and scoreboard.
    rob_t rob(4);
    register_scoreboard_t scoreboard(32);

    // Instruction 1: Produces R1 at cycle 10.
    dyn_instruction_t i1;
    i1.seq_id = 1;
    i1.pc = 0x400000;
    i1.dest_reg = 1;
    assert(rob.dispatch(i1));
    scoreboard.allocate_destination(i1);

    // Instruction 2: Consumes R1, produces R2.
    dyn_instruction_t i2;
    i2.seq_id = 2;
    i2.pc = 0x400004;
    i2.src_regs.push_back(1);
    i2.dest_reg = 2;
    assert(rob.dispatch(i2));
    scoreboard.allocate_destination(i2);

    // At cycle 5, R1 is NOT ready.
    uint64_t issue_cycle = 0;
    assert(!scoreboard.check_sources_ready(i2, 5, issue_cycle));
    assert(issue_cycle == UINT64_MAX); // Waiting on unready producer.

    // At cycle 10, i1 completes writeback.
    scoreboard.mark_writeback(i1, 10);
    assert(rob.mark_ready(1, 10));

    // Now at cycle 10, i2's source R1 is ready!
    assert(scoreboard.check_sources_ready(i2, 10, issue_cycle));
    assert(issue_cycle == 10);

    // Commit at cycle 10: i1 retires, but i2 cannot retire (not yet ready).
    std::vector<dyn_instruction_t> committed;
    size_t count = rob.commit(/*commit_width=*/2, 10, committed);
    assert(count == 1);
    assert(committed[0].seq_id == 1);
    assert(rob.size() == 1); // i2 remains in ROB.

    // Mark i2 ready at cycle 12 and commit.
    assert(rob.mark_ready(2, 12));
    committed.clear();
    count = rob.commit(/*commit_width=*/2, 12, committed);
    assert(count == 1);
    assert(committed[0].seq_id == 2);
    assert(rob.empty());
}

// Test 10: Load-Store Queue (LSQ) and Store-to-Load Forwarding.
static void
unit_test_lsq_and_forwarding()
{
    lsq_t lsq(16, 16);

    // Store instruction at address 0x7fff0000 with 8 bytes.
    dyn_instruction_t st;
    st.seq_id = 10;
    st.op_type = cpu_op_type_t::OP_STORE;
    st.mem_addr = 0x7fff0000;
    st.mem_size = 8;
    assert(lsq.dispatch_store(st));

    // Load instruction to the SAME address 0x7fff0000 with 8 bytes.
    dyn_instruction_t ld;
    ld.seq_id = 11;
    ld.op_type = cpu_op_type_t::OP_LOAD;
    ld.mem_addr = 0x7fff0000;
    ld.mem_size = 8;
    assert(lsq.dispatch_load(ld));

    // Probe store forwarding: must hit!
    uint64_t fwd_id = 0;
    assert(lsq.check_store_forwarding(ld.seq_id, ld.mem_addr, ld.mem_size, fwd_id));
    assert(fwd_id == 10);
    assert(lsq.store_forwardings() == 1);

    // Load to a different address 0x7fff1000: must NOT forward!
    uint64_t fwd_id2 = 0;
    assert(!lsq.check_store_forwarding(12, 0x7fff1000, 8, fwd_id2));
}

// Test 11: Full Out-of-Order CPU & 3-Level Cache Simulator End-to-End.
static void
unit_test_ooo_cpu_simulator()
{
    cpu_simulator_knobs_t knobs;
    knobs.num_cores = 1;
    knobs.fetch_width = 4;
    knobs.commit_width = 4;
    knobs.rob_capacity = 32;
    knobs.lq_capacity = 16;
    knobs.sq_capacity = 16;
    knobs.l1d_size = 4096;
    knobs.l2_size = 16384;
    knobs.l3_size = 65536;

    cpu_simulator_t sim(knobs);

    // Feed a sequence of instructions, stores, and loads.
    const memref_pid_t pid = 5555;
    for (int i = 0; i < 20; ++i) {
        addr_t pc = 0x400000 + i * 4;
        assert(sim.process_memref(generate_mem_ref(pc, pc, pid, 1, TRACE_TYPE_INSTR)));
        // Every 4th instruction do a store followed by a load.
        if (i % 4 == 0) {
            addr_t addr = 0x7fff2000 + i * 8;
            assert(sim.process_memref(generate_mem_ref(addr, pc, pid, 1, TRACE_TYPE_WRITE)));
            assert(sim.process_memref(generate_mem_ref(addr, pc, pid, 1, TRACE_TYPE_READ)));
        }
    }

    cpu_core_t *core = sim.get_core(0);
    assert(core != nullptr);
    core->flush_and_drain_pipeline();

    // Verify all instructions retired from the ROB.
    assert(core->total_instructions_retired() > 20);
    assert(core->get_current_cycle() > 20);
    assert(core->get_ipc() > 0.0);
    assert(core->get_cpi() > 0.0);

    // Verify ITLB received instruction fetches and recorded hits/misses.
    tlb_t *itlb = sim.get_itlb(0);                                                  // Retrieve core 0 ITLB.
    assert(itlb != nullptr);                                                         // Verify non-null ITLB.
    assert(itlb->get_stats() != nullptr);                                            // Verify valid ITLB stats.
    assert(itlb->get_stats()->get_metric(metric_name_t::MISSES) >= 1);               // At least 1 compulsory miss on 0x400000.
    assert(itlb->get_stats()->get_metric(metric_name_t::HITS) >= 19);                // Remaining 19 instruction fetches hit!

    // Verify DTLB received data memory references and recorded hits/misses.
    tlb_t *dtlb = sim.get_dtlb(0);                                                  // Retrieve core 0 DTLB.
    assert(dtlb != nullptr);                                                         // Verify non-null DTLB.
    assert(dtlb->get_stats() != nullptr);                                            // Verify valid DTLB stats.
    assert(dtlb->get_stats()->get_metric(metric_name_t::MISSES) >= 1);               // At least 1 compulsory miss on data page.
    assert(dtlb->get_stats()->get_metric(metric_name_t::HITS) >= 1);                 // Hits on repeated data page accesses.

    // Verify unified L2 TLB received requests on L1 misses.
    tlb_t *l2_tlb = sim.get_l2_tlb(0);                                              // Retrieve core 0 L2 TLB.
    assert(l2_tlb != nullptr);                                                       // Verify non-null L2 TLB.
    assert(l2_tlb->get_stats() != nullptr);                                          // Verify valid L2 TLB stats.
    assert(l2_tlb->get_stats()->get_metric(metric_name_t::MISSES) >= 1);             // L2 compulsory misses.

    // Verify results print cleanly.
    assert(sim.print_results());
}

// Test 12: Speculative Wrong-Path Execution, Cache Pollution, and Pipeline Squash.
static void
unit_test_wrong_path_speculation()
{
    // 1. Standalone test of speculative wrong-path engine.
    wrong_path_knobs_t wp_knobs;
    // Enable wrong-path speculation.
    wp_knobs.enable_wrong_path = true;
    // Set max speculative fetch depth to 16 instructions.
    wp_knobs.max_speculative_depth = 16;
    // Set load probability to 50% to ensure memory traffic.
    wp_knobs.load_probability = 0.50;
    // Set store probability to 25% to verify store buffering.
    wp_knobs.store_probability = 0.25;
    // Disable inner branches for deterministic linear instruction stream.
    wp_knobs.branch_probability = 0.0;

    // Instantiate speculative wrong-path engine.
    speculative_wrong_path_engine_t wp_engine(wp_knobs);
    // Verify engine is enabled.
    assert(wp_engine.is_enabled());

    // Misprediction setup: ground truth branch is Taken to 0x500000, but predictor predicted Not-Taken.
    branch_prediction_t pred;
    // Set prediction to Not-Taken.
    pred.predicted_taken = false;
    // Set predicted target to 0 (fallthrough).
    pred.predicted_target = 0;
    // Set ground truth to Taken.
    pred.actual_taken = true;
    // Set ground truth target to 0x500000.
    pred.actual_target = 0x500000;
    // Set misprediction flags.
    pred.direction_mispredicted = true;
    pred.target_mispredicted = true;
    // Confirm prediction is a misprediction.
    assert(pred.is_mispredicted());

    // In-out sequence ID counter.
    uint64_t next_seq_id = 100;
    // Generate speculative wrong-path stream along the fallthrough path.
    std::vector<dyn_instruction_t> stream = wp_engine.generate_wrong_path_stream(
        /*branch_seq_id=*/99, /*branch_pc=*/0x400000, pred, /*fetch_width=*/4,
        /*penalty_cycles=*/10, /*pid=*/111, /*tid=*/222, next_seq_id);

    // Verify wrong-path instructions were generated.
    assert(!stream.empty());
    // Verify fetch count respects max speculative depth.
    assert(stream.size() <= 16);
    // Fallthrough address must be 0x400004 since predictor predicted Not-Taken.
    assert(stream[0].pc == 0x400004);
    // Verify instruction is flagged as wrong-path speculative.
    assert(stream[0].is_wrong_path == true);
    // Verify PID was recorded.
    assert(stream[0].pid == 111);
    // Verify TID was recorded.
    assert(stream[0].tid == 222);

    // Verify statistics tracking.
    const wrong_path_stats_t &stats = wp_engine.get_stats();
    // Verify misprediction counter incremented.
    assert(stats.mispredictions_handled == 1);
    // Verify instructions fetched matches stream size.
    assert(stats.instructions_fetched == static_cast<int64_t>(stream.size()));

    // 2. Test speculative cache pollution via wrong-path load execution.
    three_level_cache_knobs_t cache_knobs;
    // Configure single-core cache hierarchy.
    cache_knobs.num_cores = 1;
    // L1D size: 4 KB.
    cache_knobs.l1d_size = 4096;
    // L2 size: 16 KB.
    cache_knobs.l2_size = 16384;
    // L3 size: 65 KB.
    cache_knobs.l3_size = 65536;
    // Instantiate 3-level data cache.
    three_level_cache_t cache(cache_knobs);

    // Create a speculative load instruction on the wrong path.
    dyn_instruction_t spec_load;
    // Assign sequence ID.
    spec_load.seq_id = 101;
    // Operation type is Load.
    spec_load.op_type = cpu_op_type_t::OP_LOAD;
    // PC on wrong path.
    spec_load.pc = 0x400008;
    // Address accessed speculatively.
    spec_load.mem_addr = 0x7fffbeef0000ULL;
    // Load size: 8 bytes.
    spec_load.mem_size = 8;
    // Mark as wrong path.
    spec_load.is_wrong_path = true;

    // Simulate speculative load access into the cache hierarchy.
    wp_engine.simulate_speculative_load(/*core_id=*/0, spec_load, &cache, /*dtlb=*/nullptr);
    // Verify speculative cache accesses incremented.
    assert(wp_engine.get_stats().cache_accesses == 1);
    // Verify speculative cache misses incremented.
    assert(wp_engine.get_stats().cache_misses == 1);

    // Now execute a normal right-path demand load to the EXACT same address:
    // Because the speculative wrong-path load fetched the line into L1D,
    // this demand load MUST HIT in L1D! This proves microarchitectural cache pollution!
    memref_t demand_load = {};
    // Operation type is Read.
    demand_load.data.type = TRACE_TYPE_READ;
    // Address matches speculative load.
    demand_load.data.addr = 0x7fffbeef0000ULL;
    // Access size: 8 bytes.
    demand_load.data.size = 8;
    // Process ID.
    demand_load.data.pid = 111;
    // Thread ID.
    demand_load.data.tid = 222;
    // Request address from cache hierarchy.
    cache_access_result_t res = cache.request(0, demand_load);
    // Verify hit in L1D cache due to speculative pre-population!
    assert(res.level == cache_hit_level_t::HIT_L1D);

    // Test speculative instruction fetch pollution in L1I cache:
    dyn_instruction_t spec_fetch_instr;
    // Set sequence ID.
    spec_fetch_instr.seq_id = 102;
    // PC on wrong path.
    spec_fetch_instr.pc = 0x409000;
    // Process ID.
    spec_fetch_instr.pid = 111;
    // Thread ID.
    spec_fetch_instr.tid = 222;
    // Mark as wrong path.
    spec_fetch_instr.is_wrong_path = true;

    // Simulate speculative instruction fetch into L1I cache.
    wp_engine.simulate_speculative_fetch(/*core_id=*/0, spec_fetch_instr, &cache, /*itlb=*/nullptr);
    // Verify speculative L1I accesses incremented.
    assert(wp_engine.get_stats().icache_accesses == 1);
    // Verify speculative L1I misses incremented.
    assert(wp_engine.get_stats().icache_misses == 1);

    // Right-path demand instruction fetch to 0x409000 will now HIT in L1I!
    memref_t demand_fetch = generate_mem_ref(0x409000, 0x409000, 111, 222, TRACE_TYPE_INSTR);
    // Request instruction from cache hierarchy.
    cache_access_result_t ifetch_res = cache.request_instruction(0, demand_fetch);
    // Verify hit in L1I cache due to speculative instruction pre-population!
    assert(ifetch_res.level == cache_hit_level_t::HIT_L1I);

    // 3. Test squash accounting.
    wp_engine.record_squash(stream.size());
    // Verify squashed instruction counter updated.
    assert(wp_engine.get_stats().instructions_squashed == static_cast<int64_t>(stream.size()));

    // 4. Test end-to-end integration with cpu_simulator_t.
    cpu_simulator_knobs_t sim_knobs;
    // Single core simulator.
    sim_knobs.num_cores = 1;
    // Enable wrong-path speculation.
    sim_knobs.enable_wrong_path = true;
    // Max speculative depth: 8.
    sim_knobs.max_speculative_depth = 8;
    // Branch misprediction penalty: 10 cycles.
    sim_knobs.branch_mispredict_penalty = 10;
    // Instantiate simulator.
    cpu_simulator_t sim(sim_knobs);

    // Issue sequential instructions.
    sim.process_memref(generate_mem_ref(0x400000, 0x400000, 1, 1, TRACE_TYPE_INSTR));
    sim.process_memref(generate_mem_ref(0x400004, 0x400004, 1, 1, TRACE_TYPE_INSTR));
    // Now trigger a backwards branch jump: from 0x400004 to 0x400000.
    // Predictor has cold state, so this triggers a misprediction flush!
    sim.process_memref(generate_mem_ref(0x400000, 0x400000, 1, 1, TRACE_TYPE_INSTR));

    // Retrieve core 0 pointer.
    cpu_core_t *core = sim.get_core(0);
    // Verify pointer is non-null.
    assert(core != nullptr);
    // Drain remaining in-flight instructions.
    core->flush_and_drain_pipeline();

    // Verify that the wrong path engine inside the core handled the misprediction and squashed instructions.
    const wrong_path_stats_t &core_wp_stats = core->get_wrong_path_engine().get_stats();
    // Must have handled at least 1 misprediction.
    assert(core_wp_stats.mispredictions_handled >= 1);
    // Must have fetched speculative instructions.
    assert(core_wp_stats.instructions_fetched > 0);
    // Must have squashed speculative instructions upon resolution.
    assert(core_wp_stats.instructions_squashed > 0);

    // Verify ROB and LSQ have drained and are completely clean.
    assert(core->get_rob().empty());
    // Verify Load Queue is empty.
    assert(core->get_lsq().lq_size() == 0);
    // Verify Store Queue is empty.
    assert(core->get_lsq().sq_size() == 0);
}

// Test 13: ChampSim Standard (64-byte) and CloudSuite (96-byte) Trace Reader.
static void
unit_test_champsim_and_cloudsuite_reader()
{
    // 1. Test Standard 64-byte ChampSim record reading and unpacking.
    std::string std_trace_path = "/tmp/test_std_champsim.trace";
    // Open binary output file.
    FILE *f_std = fopen(std_trace_path.c_str(), "wb");
    // Verify file created.
    assert(f_std != nullptr);

    // Create a 64-byte instruction record:
    champsim_instr_t std_instr = {};
    // Program counter.
    std_instr.ip = 0x401000;
    // Not a branch.
    std_instr.is_branch = 0;
    // Not taken.
    std_instr.branch_taken = 0;
    // Destination register.
    std_instr.destination_registers[0] = 1;
    // Source register.
    std_instr.source_registers[0] = 2;
    // 1 store address.
    std_instr.destination_memory[0] = 0x7fff0010;
    // 1 load address.
    std_instr.source_memory[0] = 0x7fff0020;
    // Write 64 bytes to file.
    size_t written = fwrite(&std_instr, 1, sizeof(std_instr), f_std);
    // Verify 64 bytes written.
    assert(written == 64);
    // Close file.
    fclose(f_std);

    // Read standard trace via champsim_trace_reader_t.
    champsim_trace_reader_t std_reader(std_trace_path, champsim_trace_format_t::FORMAT_STANDARD);
    // Verify open succeeds.
    assert(std_reader.open());
    // Verify format is standard.
    assert(!std_reader.is_cloudsuite());

    // Unpack memrefs: must yield 1 instruction fetch, 1 load, and 1 store.
    memref_t m1 = {}, m2 = {}, m3 = {}, m4 = {};
    // First reference: instruction fetch.
    assert(std_reader.get_next_memref(m1));
    // Verify type is instruction fetch and PC matches.
    assert(m1.instr.type == TRACE_TYPE_INSTR && m1.instr.addr == 0x401000);
    // Second reference: data read.
    assert(std_reader.get_next_memref(m2));
    // Verify type is read and address matches.
    assert(m2.data.type == TRACE_TYPE_READ && m2.data.addr == 0x7fff0020);
    // Third reference: data write.
    assert(std_reader.get_next_memref(m3));
    // Verify type is write and address matches.
    assert(m3.data.type == TRACE_TYPE_WRITE && m3.data.addr == 0x7fff0010);
    // Fourth reference: EOF.
    assert(!std_reader.get_next_memref(m4));
    // Close reader.
    std_reader.close();
    // Clean up temporary file.
    remove(std_trace_path.c_str());

    // 2. Test CloudSuite 96-byte ChampSim record reading and unpacking.
    std::string cs_trace_path = "/tmp/test_cloudsuite.trace";
    // Open binary output file.
    FILE *f_cs = fopen(cs_trace_path.c_str(), "wb");
    // Verify file created.
    assert(f_cs != nullptr);

    // Create a 96-byte CloudSuite instruction record:
    cloudsuite_instr_t cs_instr = {};
    // Program counter.
    cs_instr.ip = 0x402000;
    // Is branch.
    cs_instr.is_branch = 1;
    // Branch taken.
    cs_instr.branch_taken = 1;
    // Destination register.
    cs_instr.destination_registers[0] = 3;
    // Source register.
    cs_instr.source_registers[0] = 4;
    // 4 stores in CloudSuite format:
    cs_instr.destination_memory[0] = 0x7fff1000;
    cs_instr.destination_memory[1] = 0x7fff1008;
    cs_instr.destination_memory[2] = 0x7fff1010;
    cs_instr.destination_memory[3] = 0x7fff1018;
    // 4 loads in CloudSuite format:
    cs_instr.source_memory[0] = 0x7fff2000;
    cs_instr.source_memory[1] = 0x7fff2008;
    cs_instr.source_memory[2] = 0x7fff2010;
    cs_instr.source_memory[3] = 0x7fff2018;
    // Address Space ID 42 (PID).
    cs_instr.asid[0] = 42;
    // Thread ID 1.
    cs_instr.asid[1] = 1;
    // Write 96 bytes to file.
    written = fwrite(&cs_instr, 1, sizeof(cs_instr), f_cs);
    // Verify 96 bytes written.
    assert(written == 96);
    // Close file.
    fclose(f_cs);

    // Read CloudSuite trace via champsim_trace_reader_t.
    champsim_trace_reader_t cs_reader(cs_trace_path, champsim_trace_format_t::FORMAT_CLOUDSUITE);
    // Verify open succeeds.
    assert(cs_reader.open());
    // Verify format is CloudSuite.
    assert(cs_reader.is_cloudsuite());

    // Unpack memrefs: must yield 1 instruction fetch (with PID 42, TID 1), 4 loads, 4 stores = 9 memrefs!
    std::vector<memref_t> cs_refs;
    memref_t ref;
    // Drain all memrefs.
    while (cs_reader.get_next_memref(ref)) {
        cs_refs.push_back(ref);
    }
    // Verify count is 9.
    assert(cs_refs.size() == 9);
    // Verify instruction fetch has PID 42 and TID 1.
    assert(cs_refs[0].instr.type == TRACE_TYPE_INSTR && cs_refs[0].instr.addr == 0x402000);
    assert(cs_refs[0].instr.pid == 42 && cs_refs[0].instr.tid == 1);
    // Verify 4 data reads.
    for (int i = 1; i <= 4; ++i) {
        assert(cs_refs[i].data.type == TRACE_TYPE_READ);
        assert(cs_refs[i].data.pid == 42);
    }
    // Verify 4 data writes.
    for (int i = 5; i <= 8; ++i) {
        assert(cs_refs[i].data.type == TRACE_TYPE_WRITE);
        assert(cs_refs[i].data.pid == 42);
    }
    // Close reader.
    cs_reader.close();
    // Clean up temporary file.
    remove(cs_trace_path.c_str());

    // 3. Test real trace playback from disk if Google trace exists.
    std::string google_trace = "/media/pravesh/Storage/ALL_TRACES/nutch_phase0_core1.trace.xz";
    FILE *check_f = fopen(google_trace.c_str(), "rb");
    if (check_f != nullptr) {
        fclose(check_f);

        // Test flag parsing helper:
        assert(champsim_trace_reader_t::parse_format_string("default") == champsim_trace_format_t::FORMAT_STANDARD);
        assert(champsim_trace_reader_t::parse_format_string("standard") == champsim_trace_format_t::FORMAT_STANDARD);
        assert(champsim_trace_reader_t::parse_format_string("64") == champsim_trace_format_t::FORMAT_STANDARD);
        assert(champsim_trace_reader_t::parse_format_string("cloudsuite") == champsim_trace_format_t::FORMAT_CLOUDSUITE);
        assert(champsim_trace_reader_t::parse_format_string("cs") == champsim_trace_format_t::FORMAT_CLOUDSUITE);
        assert(champsim_trace_reader_t::parse_format_string("96") == champsim_trace_format_t::FORMAT_CLOUDSUITE);
        assert(champsim_trace_reader_t::parse_format_string("auto", /*is_cloudsuite_flag=*/true) == champsim_trace_format_t::FORMAT_CLOUDSUITE);

        // Verify explicit format flag passed to constructor (CloudSuite format):
        champsim_trace_reader_t flag_reader(google_trace, champsim_trace_format_t::FORMAT_CLOUDSUITE);
        assert(flag_reader.open());
        assert(flag_reader.is_cloudsuite());
        flag_reader.close();

        // Create ChampSim trace reader with auto-detection fallback.
        champsim_trace_reader_t real_reader(google_trace);
        // Verify open succeeds.
        assert(real_reader.open());
        // Configure single core simulator.
        cpu_simulator_knobs_t knobs;
        knobs.num_cores = 1;
        cpu_simulator_t sim(knobs);
        // Simulate 1000 instructions from real Google trace.
        uint64_t simulated = real_reader.run_simulation(sim, /*max_instructions=*/1000, /*print_interval=*/0);
        // Verify simulated count matches 1000.
        assert(simulated == 1000);
        // Verify core retired instructions.
        assert(sim.get_core(0)->total_instructions_retired() > 0);
    }
}

// Test 14: Miss Status Holding Registers (MSHR), Cache Queues (RQ/WQ/PQ), and Nested INI Parser.
static void
unit_test_mshr_queues_and_nested_ini()
{
    // 1. Test MSHR Table: allocation, line matching, secondary coalescing, and capacity stalls.
    mshr_table_t mshr(2, 64);
    // Verify initial capacity is 2.
    assert(mshr.capacity() == 2);
    // Verify initial occupancy is 0.
    assert(mshr.occupancy() == 0);
    // Verify MSHR is not full.
    assert(!mshr.is_full());
    // Allocate first entry for cache line 0x1000 with 10 cycle fill latency.
    mshr_entry_t *e1 = mshr.allocate(0x1000, 0, 10, cpu_op_type_t::OP_LOAD, 101);
    // Verify allocation returned non-null pointer.
    assert(e1 != nullptr);
    // Verify occupancy incremented to 1.
    assert(mshr.occupancy() == 1);
    // Verify MSHR is not yet full.
    assert(!mshr.is_full());
    // Lookup with different byte offset in same 64-byte line (0x1028).
    mshr_entry_t *found1 = mshr.lookup(0x1028);
    // Verify lookup matches allocated entry.
    assert(found1 == e1);
    // Coalesce secondary miss to line 0x1000.
    mshr.coalesce_secondary(found1, 102);
    // Verify statistics recorded secondary merge.
    assert(mshr.get_stats().secondary_merges == 1);
    // Verify secondary ID recorded in entry.
    assert(found1->secondary_ids.size() == 1);
    // Verify sequence ID matches.
    assert(found1->secondary_ids[0] == 102);

    // Allocate second entry for line 0x2000 with 15 cycle latency.
    mshr_entry_t *e2 = mshr.allocate(0x2000, 0, 15, cpu_op_type_t::OP_LOAD, 103);
    // Verify allocation returned valid pointer.
    assert(e2 != nullptr);
    // Verify occupancy reached capacity (2).
    assert(mshr.occupancy() == 2);
    // Verify table reports full.
    assert(mshr.is_full());
    // Verify structural stall calculation reports positive delay.
    assert(mshr.get_cycles_until_next_available(0) > 0);
    // Advance simulation time to cycle 5: neither entry is ready yet.
    mshr.retire_completed_fills(5);
    // Verify occupancy remains 2.
    assert(mshr.occupancy() == 2);
    // Advance to cycle 10: e1 (ready at cycle 10) must retire!
    mshr.retire_completed_fills(10);
    // Verify occupancy dropped to 1.
    assert(mshr.occupancy() == 1);
    // Verify table is no longer full.
    assert(!mshr.is_full());
    // Verify line 0x1000 is no longer in MSHR.
    assert(mshr.lookup(0x1000) == nullptr);
    // Verify line 0x2000 is still active.
    assert(mshr.lookup(0x2000) == e2);

    // 2. Test Cache Queues: Read Queue (RQ), Write Queue (WQ), Prefetch Queue (PQ).
    // Test Read Queue FIFO behavior.
    read_queue_t rq(2, 64);
    // Verify Read Queue capacity.
    assert(rq.capacity() == 2);
    // Verify Read Queue initial size.
    assert(rq.size() == 0);
    // Prepare first read request.
    cache_queue_entry_t r1 = {};
    // Set target address.
    r1.addr = 0x4000;
    // Set operation type to load.
    r1.op_type = cpu_op_type_t::OP_LOAD;
    // Enqueue first read request.
    assert(rq.enqueue(r1, 1));
    // Verify size is 1.
    assert(rq.size() == 1);
    // Prepare second read request.
    cache_queue_entry_t r2 = {};
    // Set target address.
    r2.addr = 0x5000;
    // Set operation type to load.
    r2.op_type = cpu_op_type_t::OP_LOAD;
    // Enqueue second read request.
    assert(rq.enqueue(r2, 2));
    // Verify queue is now full.
    assert(rq.is_full());
    // Third enqueue must fail due to capacity.
    cache_queue_entry_t r3 = {};
    // Set target address.
    r3.addr = 0x6000;
    // Attempt enqueue on full queue.
    assert(!rq.enqueue(r3, 3));
    // Dequeue first request.
    cache_queue_entry_t deq_r;
    // Verify dequeue succeeds.
    assert(rq.dequeue(deq_r));
    // Verify FIFO ordering: first dequeued address is 0x4000.
    assert(deq_r.addr == 0x4000);
    // Dequeue second request.
    assert(rq.dequeue(deq_r));
    // Verify second dequeued address is 0x5000.
    assert(deq_r.addr == 0x5000);
    // Verify queue is now empty.
    assert(rq.is_empty());

    // Test Write Queue RAW hazard detection and write-coalescing.
    write_queue_t wq(4, 64);
    // Prepare first store request.
    cache_queue_entry_t w1 = {};
    // Set address.
    w1.addr = 0x7000;
    // Set size to 8 bytes.
    w1.size = 8;
    // Set operation type to store.
    w1.op_type = cpu_op_type_t::OP_STORE;
    // Enqueue store into write queue.
    assert(wq.enqueue(w1, 1));
    // Verify write queue size is 1.
    assert(wq.size() == 1);
    // Verify RAW hazard detection for exact byte address.
    assert(wq.has_raw_hazard(0x7000));
    // Verify RAW hazard detection for offset within 8-byte range.
    assert(wq.has_raw_hazard(0x7004));
    // Verify non-conflicting address has no RAW hazard.
    assert(!wq.has_raw_hazard(0x8000));
    // Test write-coalescing: store to 0x7020 is within the same 64-byte line (0x7000-0x703F).
    cache_queue_entry_t w2 = {};
    // Set offset address.
    w2.addr = 0x7020;
    // Set size.
    w2.size = 8;
    // Set operation type to store.
    w2.op_type = cpu_op_type_t::OP_STORE;
    // Enqueue second store to same cache line.
    assert(wq.enqueue(w2, 2));
    // Verify queue size did not increase due to write-coalescing!
    assert(wq.size() == 1);
    // Verify coalesced writes counter incremented.
    assert(wq.coalesced_writes() == 1);

    // Test Prefetch Queue (PQ): drops silently when full.
    prefetch_queue_t pq(1, 64);
    // Prepare first prefetch request.
    cache_queue_entry_t p1 = {};
    // Set address.
    p1.addr = 0x9000;
    // Set operation type to prefetch.
    p1.op_type = cpu_op_type_t::OP_PREFETCH;
    // Enqueue first prefetch.
    assert(pq.enqueue(p1, 1));
    // Verify queue is now full.
    assert(pq.is_full());
    // Prepare second prefetch request.
    cache_queue_entry_t p2 = {};
    // Set address.
    p2.addr = 0x9040;
    // Set operation type to prefetch.
    p2.op_type = cpu_op_type_t::OP_PREFETCH;
    // Overfilling prefetch queue must drop cleanly without error.
    assert(!pq.enqueue(p2, 2));
    // Verify dropped counter incremented.
    assert(pq.dropped_prefetches() == 1);

    // 3. Test INI Configuration Parser and Nested Overwrite.
    std::string parent_ini = "/tmp/unit_test_parent.ini";
    // Define child INI path.
    std::string child_ini = "/tmp/unit_test_child.ini";
    // Open parent INI file stream for writing.
    std::ofstream pf(parent_ini.c_str());
    // Write CPU section.
    pf << "[cpu]\n";
    // Write num_cores setting.
    pf << "num_cores = 1\n";
    // Write rob_capacity setting.
    pf << "rob_capacity = 256\n";
    // Write lq_capacity setting.
    pf << "lq_capacity = 72\n";
    // Write L1D cache section.
    pf << "[l1d]\n";
    // Write size setting with byte multiplier.
    pf << "size = 32KB\n";
    // Write mshr_capacity setting.
    pf << "mshr_capacity = 16\n";
    // Close parent file.
    pf.close();

    // Open child INI file stream for writing.
    std::ofstream cf(child_ini.c_str());
    // Write include directive pointing to parent INI file.
    cf << "include = /tmp/unit_test_parent.ini\n";
    // Write CPU section.
    cf << "[cpu]\n";
    // Overwrite parent ROB capacity setting from 256 to 512!
    cf << "rob_capacity = 512\n";
    // Write L1D cache section.
    cf << "[l1d]\n";
    // Overwrite parent MSHR capacity setting from 16 to 32!
    cf << "mshr_capacity = 32\n";
    // Close child file.
    cf.close();

    // Instantiate INI configuration parser.
    ini_config_t config;
    // Load and resolve child INI with nested include.
    assert(config.load(child_ini));

    // Verify parsed values in configuration map.
    // Verify num_cores inherited from parent.
    assert(config.get_uint64("cpu", "num_cores") == 1);
    // Verify lq_capacity inherited from parent.
    assert(config.get_uint64("cpu", "lq_capacity") == 72);
    // Verify rob_capacity overwritten by child to 512!
    assert(config.get_uint64("cpu", "rob_capacity") == 512);
    // Verify mshr_capacity overwritten by child to 32!
    assert(config.get_uint64("l1d", "mshr_capacity") == 32);
    // Verify 32KB byte size parsed correctly to 32768.
    assert(config.get_bytesize("l1d", "size") == 32768);

    // Apply configuration to simulator knobs.
    cpu_simulator_knobs_t knobs;
    // Apply parsed values.
    config.apply_to_knobs(knobs);
    // Verify knobs num_cores matches 1.
    assert(knobs.num_cores == 1);
    // Verify knobs rob_capacity matches overwritten 512.
    assert(knobs.rob_capacity == 512);
    // Verify knobs lq_capacity matches inherited 72.
    assert(knobs.lq_capacity == 72);
    // Verify knobs l1d_size matches 32768.
    assert(knobs.l1d_size == 32768);
    // Verify knobs l1d_mshr_capacity matches overwritten 32.
    assert(knobs.l1d_mshr_capacity == 32);

    // Clean up temporary parent file.
    remove(parent_ini.c_str());
    // Clean up temporary child file.
    remove(child_ini.c_str());
}

// Test 15: Sectored L2 TLB Geometry (32x6, 64x6, 128x6), Leaf Block Co-Allocation & INI Configuration.
static void
unit_test_sector_l2_tlb()
{
    // 1. Verify Sector Geometry Calculations: total PTE entries remains constant at 1536.
    const unsigned int baseline_entries = 1536;                                     // Total PTE capacity.
    const unsigned int assoc = 6;                                                   // 6-way associativity.

    // Baseline (sector=1): 256 sets x 6 ways = 1536 entries.
    unsigned int sets_s1 = baseline_entries / (1 * assoc);                          // 1536 / 6 = 256 sets.
    assert(sets_s1 == 256);                                                         // Verify 256x6 baseline.

    // Sector=2: 128 sets x 6 ways (768 sector blocks x 2 PTEs = 1536 entries).
    unsigned int sets_s2 = baseline_entries / (2 * assoc);                          // 1536 / 12 = 128 sets.
    assert(sets_s2 == 128);                                                         // Verify 128x6.

    // Sector=4: 64 sets x 6 ways (384 sector blocks x 4 PTEs = 1536 entries).
    unsigned int sets_s4 = baseline_entries / (4 * assoc);                          // 1536 / 24 = 64 sets.
    assert(sets_s4 == 64);                                                          // Verify 64x6.

    // Sector=8: 32 sets x 6 ways (192 sector blocks x 8 PTEs = 1536 entries).
    unsigned int sets_s8 = baseline_entries / (8 * assoc);                          // 1536 / 48 = 32 sets.
    assert(sets_s8 == 32);                                                          // Verify 32x6.

    // 2. Functional Verification of Sector=8: Leaf PTE block pre-allocation (0..7 sharing entry_offset / 8).
    cpu_simulator_knobs_t knobs;                                                    // Instantiate configuration knobs.
    knobs.num_cores = 1;                                                            // Single CPU core.
    knobs.enable_wrong_path = false;                                                // Disable speculative wrong-path for deterministic test.
    knobs.l2_tlb_entries = 1536;                                                    // 1536 total PTE capacity.
    knobs.l2_tlb_assoc = 6;                                                         // 6-way associativity.
    knobs.l2_tlb_sector_size = 8;                                                   // Sector size = 8 (32 sets x 6 ways).

    cpu_simulator_t sim(knobs);                                                     // Instantiate O3 CPU simulator.
    const memref_pid_t pid = 7777;                                                  // Test process identifier.

    // Access page 0 (vaddr = 0x400000, leaf entry_offset = 0).
    // This is a cold miss: triggers 1 page table walk and places all 8 PTEs (0..7) into the sector block.
    assert(sim.process_memref(generate_mem_ref(0x400000, 0x400000, pid, 1, TRACE_TYPE_INSTR))); // Access page 0.

    // Query core 0's L2 TLB.
    tlb_t *l2_tlb = sim.get_l2_tlb(0);                                              // Retrieve L2 TLB pointer.
    assert(l2_tlb != nullptr);                                                       // Verify non-null.
    assert(l2_tlb->get_sector_size() == 8);                                         // Verify sector size is 8.
    // L2 TLB must record 1 compulsory miss on page 0.
    assert(l2_tlb->get_stats()->get_metric(metric_name_t::MISSES) == 1);            // Verify 1 miss.

    // Access pages 1 through 7 (vaddrs 0x401000 through 0x407000).
    // These share entry_offset / 8 == 0 with page 0 and were co-allocated into the same sector!
    for (int p = 1; p < 8; ++p) {                                                   // Iterate sibling pages 1..7.
        addr_t vaddr = 0x400000ULL + static_cast<addr_t>(p) * 4096ULL;              // Compute sibling page address.
        assert(sim.process_memref(generate_mem_ref(vaddr, vaddr, pid, 1, TRACE_TYPE_INSTR))); // Access sibling page.
    }

    // All sibling accesses 1..7 must HIT in the L2 TLB without page table walks!
    assert(l2_tlb->get_stats()->get_metric(metric_name_t::HITS) == 7);              // Verify exactly 7 L2 TLB hits!
    assert(l2_tlb->get_stats()->get_metric(metric_name_t::MISSES) == 1);            // Miss count remains 1!

    // Access page 8 (vaddr = 0x408000, leaf entry_offset = 8, entry_offset / 8 == 1).
    // This belongs to the next sector: triggers a 2nd miss and co-allocates pages 8..15.
    assert(sim.process_memref(generate_mem_ref(0x408000, 0x408000, pid, 1, TRACE_TYPE_INSTR))); // Access page 8.
    assert(l2_tlb->get_stats()->get_metric(metric_name_t::MISSES) == 2);            // Miss count becomes 2.

    // Access page 9 (vaddr = 0x409000, leaf entry_offset = 9, entry_offset / 8 == 1).
    // This is a sibling in sector 1: must HIT!
    assert(sim.process_memref(generate_mem_ref(0x409000, 0x409000, pid, 1, TRACE_TYPE_INSTR))); // Access page 9.
    assert(l2_tlb->get_stats()->get_metric(metric_name_t::HITS) == 8);              // Hit count becomes 8!
    assert(l2_tlb->get_stats()->get_metric(metric_name_t::MISSES) == 2);            // Miss count remains 2!

    // 3. Test Invalid PTE handling in Sectored TLB using an external pagemap (v2p) map.
    // In this scenario:
    // - Sector base is 0x610000 (pages 0..7).
    // - In external_v2p: page 0 (0x610000) and page 1 (0x611000) have valid PFNs.
    // - Sibling page 2 (0x612000) is NOT in external_v2p (unmapped / invalid in pagemap).
    std::unordered_map<addr_t, addr_t> sample_v2p;                                  // Instantiate pagemap table.
    sample_v2p[0x610000] = 0x1055000;                                               // Page 0 valid PFN 0x1055 (4KB page).
    sample_v2p[0x611000] = 0x2099000;                                               // Page 1 valid PFN 0x2099 (4KB page).
    // 0x612000 is intentionally omitted to simulate an unmapped/invalid PTE!
    sim.get_address_space_manager()->load_external_v2p_map(pid, sample_v2p);        // Load pagemap into MMU.

    int64_t misses_before = l2_tlb->get_stats()->get_metric(metric_name_t::MISSES); // Initial misses.
    int64_t hits_before = l2_tlb->get_stats()->get_metric(metric_name_t::HITS);     // Initial hits.

    // Access page 0 (0x610000): compulsory miss. Walker installs sector 0x610000:
    // Sub-entry 0 (0x610000) is VALID (demand page).
    // Sub-entry 1 (0x611000) is VALID (found in pagemap).
    // Sub-entry 2 (0x612000) is INVALID (absent from pagemap).
    assert(sim.process_memref(generate_mem_ref(0x610000, 0x610000, pid, 1, TRACE_TYPE_INSTR))); // Access page 0.
    assert(l2_tlb->get_stats()->get_metric(metric_name_t::MISSES) == misses_before + 1); // Recorded 1 miss.

    // Access page 1 (0x611000): sibling was valid in pagemap, must HIT in L2 TLB!
    assert(sim.process_memref(generate_mem_ref(0x611000, 0x611000, pid, 1, TRACE_TYPE_INSTR))); // Access sibling 1.
    assert(l2_tlb->get_stats()->get_metric(metric_name_t::HITS) == hits_before + 1);    // Recorded 1 hit!

    // Access page 2 (0x612000): sibling was absent from pagemap (invalid PTE)!
    // Must MISS in L2 TLB despite sector tag already being cached!
    assert(sim.process_memref(generate_mem_ref(0x612000, 0x612000, pid, 1, TRACE_TYPE_INSTR))); // Access invalid sibling 2.
    assert(l2_tlb->get_stats()->get_metric(metric_name_t::MISSES) == misses_before + 2); // Recorded 2nd miss!

    // 4. Test Loading configs/o3_sector.ini via ini_config_t.
    ini_config_t sector_config;                                                     // Instantiate INI parser.
    assert(sector_config.load("configs/o3_sector.ini"));                            // Load and parse o3_sector.ini.
    cpu_simulator_knobs_t parsed_knobs;                                             // Simulator knobs destination.
    sector_config.apply_to_knobs(parsed_knobs);                                     // Apply configuration.

    // Verify parsed sector parameters match specification.
    assert(parsed_knobs.l2_tlb_sector_size == 8);                                   // Verify sector_size = 8.
    assert(parsed_knobs.l2_tlb_entries == 1536);                                    // Verify entries = 1536.
    assert(parsed_knobs.l2_tlb_assoc == 6);                                         // Verify assoc = 6.

    // 5. Verify results print cleanly.
    assert(sim.print_results());                                                    // Verify results print cleanly.
}

static void
unit_test_cold_capacity_compulsory_and_reuse_histogram()
{
    // 1. Test Cold, Compulsory, and Capacity Misses on Caches using cache_stats_t.
    // Configure cache stats with 64-byte lines.
    cache_stats_t c_stats(64);
    // Explicitly configure capacity to 4 blocks (256 bytes).
    c_stats.set_device_capacity(4);
    // Enable power-of-2 reuse distance histogram.
    c_stats.enable_reuse_distance_histogram(true);

    // Build synthetic memory reference template for instruction reads.
    memref_t ref = {};
    ref.data.type = TRACE_TYPE_READ;
    ref.data.size = 8;
    ref.data.pid = 1;
    ref.data.tid = 1;

    // Access 4 distinct cache lines (0x1000, 0x1040, 0x1080, 0x10c0) for the first time.
    for (int i = 0; i < 4; ++i) {
        ref.data.addr = 0x1000ULL + static_cast<addr_t>(i * 64);
        c_stats.access(ref, /*hit=*/false, nullptr);
    }
    // Verify all 4 accesses are classified as cold and compulsory misses.
    assert(c_stats.get_metric(metric_name_t::MISSES) == 4);
    assert(c_stats.get_metric(metric_name_t::COMPULSORY_MISSES) == 4);
    assert(c_stats.get_metric(metric_name_t::COLD_MISSES) == 4);
    assert(c_stats.get_metric(metric_name_t::CAPACITY_MISSES) == 0);

    // Access a 5th distinct cache line (0x1100).
    ref.data.addr = 0x1100ULL;
    c_stats.access(ref, /*hit=*/false, nullptr);
    assert(c_stats.get_metric(metric_name_t::MISSES) == 5);
    assert(c_stats.get_metric(metric_name_t::COMPULSORY_MISSES) == 5);
    assert(c_stats.get_metric(metric_name_t::COLD_MISSES) == 5);
    assert(c_stats.get_metric(metric_name_t::CAPACITY_MISSES) == 0);

    // Re-access the first line (0x1000) as a MISS.
    // Between line 0x1000's first reference and now, 4 distinct blocks (0x1040, 0x1080, 0x10c0, 0x1100)
    // were accessed. Since reuse distance 4 >= device capacity 4, this is a CAPACITY MISS!
    ref.data.addr = 0x1000ULL;
    c_stats.access(ref, /*hit=*/false, nullptr);
    assert(c_stats.get_metric(metric_name_t::MISSES) == 6);
    assert(c_stats.get_metric(metric_name_t::COMPULSORY_MISSES) == 5); // Compulsory misses remains 5.
    assert(c_stats.get_metric(metric_name_t::COLD_MISSES) == 5);       // Cold misses remains 5.
    assert(c_stats.get_metric(metric_name_t::CAPACITY_MISSES) == 1);   // Exactly 1 capacity miss recorded!

    // Re-access line 0x1000 immediately as a HIT: reuse distance = 0.
    c_stats.access(ref, /*hit=*/true, nullptr);
    assert(c_stats.get_metric(metric_name_t::HITS) == 1);

    // 2. Verify LLC Reuse Distance Histogram Power-of-2 Binning.
    const auto &hist = c_stats.get_reuse_histogram();
    const auto &labels = caching_device_stats_t::get_reuse_histogram_labels();
    // Labels vector must contain exactly 28 bins: 0, 1, 2, 4, ..., 16777216, >16777216, inf.
    assert(labels.size() == 28);
    assert(hist.size() == 28);
    assert(labels[0] == "0");
    assert(labels[1] == "1");
    assert(labels[2] == "2");
    assert(labels[3] == "4");
    assert(labels[27] == "inf");

    // All 5 initial first-time references (0x1000..0x10c0, 0x1100) have distance = inf.
    assert(hist[27] == 5); // 5 compulsory accesses in inf bin.
    // Re-accessing 0x1000 had reuse distance = 4: falls into bin 3 (label "4").
    assert(hist[3] == 1);  // 1 event in bin 4.
    // Immediate repeat hit on 0x1000 had reuse distance = 0: falls into bin 0 (label "0").
    assert(hist[0] == 1);  // 1 event in bin 0.

    // 3. Test TLB Cold, Compulsory, and Capacity Misses using tlb_stats_t.
    tlb_stats_t t_stats(4096);
    // Configure TLB capacity to 2 page entries.
    t_stats.set_device_capacity(2);

    memref_t tlb_ref = {};
    tlb_ref.data.type = TRACE_TYPE_READ;
    tlb_ref.data.size = 8;
    tlb_ref.data.pid = 1;
    tlb_ref.data.tid = 1;

    // Access page 0 (0x200000) and page 1 (0x201000).
    tlb_ref.data.addr = 0x200000ULL;
    t_stats.access(tlb_ref, /*hit=*/false, nullptr);
    tlb_ref.data.addr = 0x201000ULL;
    t_stats.access(tlb_ref, /*hit=*/false, nullptr);

    assert(t_stats.get_metric(metric_name_t::MISSES) == 2);
    assert(t_stats.get_metric(metric_name_t::COLD_MISSES) == 2);
    assert(t_stats.get_metric(metric_name_t::COMPULSORY_MISSES) == 2);
    assert(t_stats.get_metric(metric_name_t::CAPACITY_MISSES) == 0);

    // Access page 2 (0x202000): 3rd unique page.
    tlb_ref.data.addr = 0x202000ULL;
    t_stats.access(tlb_ref, /*hit=*/false, nullptr);
    assert(t_stats.get_metric(metric_name_t::MISSES) == 3);
    assert(t_stats.get_metric(metric_name_t::COLD_MISSES) == 3);
    assert(t_stats.get_metric(metric_name_t::COMPULSORY_MISSES) == 3);
    assert(t_stats.get_metric(metric_name_t::CAPACITY_MISSES) == 0);

    // Re-access page 0 (0x200000) as a MISS: reuse distance = 2 (pages 1 and 2 in between).
    // Capacity is 2: reuse distance 2 >= capacity 2 -> CAPACITY MISS!
    tlb_ref.data.addr = 0x200000ULL;
    t_stats.access(tlb_ref, /*hit=*/false, nullptr);
    assert(t_stats.get_metric(metric_name_t::MISSES) == 4);
    assert(t_stats.get_metric(metric_name_t::COLD_MISSES) == 3);       // Cold misses remains 3.
    assert(t_stats.get_metric(metric_name_t::COMPULSORY_MISSES) == 3); // Compulsory remains 3.
    assert(t_stats.get_metric(metric_name_t::CAPACITY_MISSES) == 1);   // Exactly 1 capacity miss recorded!
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
    // Run performance model (instructions, cycles, CPI_cache_stalls, CPI_tlb_stalls, IPC) unit test.
    unit_test_performance_model_cpi_ipc();
    // Run 3-level data cache hierarchy unit test.
    unit_test_three_level_cache();
    // Run BTB, branch predictor, and RAS unit test.
    unit_test_btb_and_branch_predictor();
    // Run ROB and Register Dependency Scoreboard unit test.
    unit_test_rob_and_reg_deps();
    // Run LSQ and Store-to-Load forwarding unit test.
    unit_test_lsq_and_forwarding();
    // Run full Out-of-Order CPU Core and 3-level cache simulator unit test.
    unit_test_ooo_cpu_simulator();
    // Run Speculative Wrong-Path Execution, Cache Pollution, and Pipeline Squash unit test.
    unit_test_wrong_path_speculation();
    // Run ChampSim Standard (64-byte) and CloudSuite (96-byte) Trace Reader unit test.
    unit_test_champsim_and_cloudsuite_reader();
    // Run MSHR, Cache Queues, and Nested INI Parser unit test.
    unit_test_mshr_queues_and_nested_ini();
    // Run Sectored L2 TLB (32x6, 64x6, 128x6), Leaf Block Co-Allocation & INI Configuration unit test.
    unit_test_sector_l2_tlb();
    // Run Cold, Capacity, Compulsory Misses and LLC Reuse Distance Histogram unit test.
    unit_test_cold_capacity_compulsory_and_reuse_histogram();
}

} // namespace drmemtrace
} // namespace dynamorio
