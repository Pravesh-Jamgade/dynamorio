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

/* tlb_simulator.cpp: Implementation of tlb_simulator_t and tlb_simulator_create().
 */

#include "tlb_simulator.h" // Include class declaration.

#include <stddef.h> // Provide NULL and size_t definitions.
#include <fstream>  // Provide std::ifstream for opening v2p_file.
#include <iostream> // Provide std::cerr.
#include <string>   // Provide std::string.
#include <vector>   // Provide std::vector.

#include "analysis_tool.h"        // Provide analysis_tool_t interface.
#include "cache_lru.h"            // Provide cache_lru_t for coupled PTW data caches.
#include "cache_stats.h"          // Provide cache_stats_t for coupled PTW data caches.
#include "caching_device_stats.h" // Provide caching_device_stats_t.
#include "memref.h"               // Provide memref_t.
#include "options.h"              // Provide REPLACE_POLICY_* constants.
#include "simulator.h"            // Provide base simulator_t methods.
#include "tlb.h"                  // Provide tlb_t, tlb_lru_t, and tlb_fifo_t.
#include "tlb_simulator_create.h" // Provide tlb_simulator_knobs_t.
#include "tlb_stats.h"            // Provide tlb_stats_t.
#include "trace_entry.h"          // Provide trace_type_t constants.
#include "utils.h"                // Provide ERRMSG macro and helper functions.

namespace dynamorio {  // Outer DynamoRIO namespace.
namespace drmemtrace { // DrMemtrace simulation namespace.

analysis_tool_t *
tlb_simulator_create(const tlb_simulator_knobs_t &knobs)
{
    // If no external v2p file path was specified, construct and return the simulator.
    if (knobs.v2p_file.empty())
        return new tlb_simulator_t(knobs);

    // Open the specified v2p.textproto file.
    std::ifstream fin;
    fin.open(knobs.v2p_file);
    if (!fin.is_open()) {
        ERRMSG("Failed to open the v2p file '%s'\n", knobs.v2p_file.c_str());
        return nullptr;
    }

    // Construct the TLB simulator and populate its virtual-to-physical map from `fin`.
    tlb_simulator_t *sim = new tlb_simulator_t(knobs);
    std::string error_str = sim->create_v2p_from_file(fin);
    fin.close();

    // If parsing v2p.textproto failed, clean up and return nullptr.
    if (!error_str.empty()) {
        delete sim;
        ERRMSG("ERROR: v2p_reader failed with: %s\n", error_str.c_str());
        return nullptr;
    }

    // Return the initialized TLB simulator instance.
    return sim;
}

tlb_simulator_t::tlb_simulator_t(const tlb_simulator_knobs_t &knobs)
    // Initialize the base simulator_t with core count, skip/warmup/sim refs, and flags.
    : simulator_t(knobs.num_cores, knobs.skip_refs, knobs.warmup_refs,
                  knobs.warmup_fraction, knobs.sim_refs, knobs.cpu_scheduling,
                  knobs.use_physical, knobs.verbose)
    // Store the TLB simulator configuration knobs.
    , knobs_(knobs)
    // Allocate the shared multi-process 4-level radix page table & frame manager.
    , address_space_mgr_(new mmu_address_space_manager_t())
    // Mark warmed up immediately if neither warmup_refs nor warmup_fraction is enabled.
    , is_warmed_up_(knobs.warmup_refs == 0 && knobs.warmup_fraction == 0.0)
{
    // Allocate per-core pointer arrays for L1I, L1D, and L2 TLBs.
    itlbs_ = new tlb_t *[knobs_.num_cores];
    dtlbs_ = new tlb_t *[knobs_.num_cores];
    lltlbs_ = new tlb_t *[knobs_.num_cores];
    for (unsigned int i = 0; i < knobs_.num_cores; i++) {
        itlbs_[i] = NULL;
        dtlbs_[i] = NULL;
        lltlbs_[i] = NULL;
    }
    // Resize per-core vectors for hardware page table walkers and coupled PTW caches.
    walkers_.resize(knobs_.num_cores);
    internal_ptw_dcaches_.resize(knobs_.num_cores);

    // Check whether warmup statistics tracking should be enabled on tlb_stats_t.
    bool warmup_enabled = (knobs_.warmup_refs > 0 || knobs_.warmup_fraction > 0.0);

    // Instantiate and initialize L1I, L1D, L2 TLBs, walkers, and coupled caches per core.
    for (unsigned int i = 0; i < knobs_.num_cores; i++) {
        // Create the core's L1 instruction TLB using the configured replacement policy.
        itlbs_[i] = create_tlb(knobs_.TLB_replace_policy);
        if (itlbs_[i] == NULL) {
            error_string_ = "Failed to create itlbs_";
            success_ = false;
            return;
        }
        // Create the core's L1 data TLB using the configured replacement policy.
        dtlbs_[i] = create_tlb(knobs_.TLB_replace_policy);
        if (dtlbs_[i] == NULL) {
            error_string_ = "Failed to create dtlbs_";
            success_ = false;
            return;
        }
        // Create the core's unified L2 TLB using the configured replacement policy.
        lltlbs_[i] = create_tlb(knobs_.TLB_replace_policy);
        if (lltlbs_[i] == NULL) {
            error_string_ = "Failed to create lltlbs_";
            success_ = false;
            return;
        }

        // Initialize L1I, L1D, and L2 TLB devices and register L1I/L1D as children of L2.
        std::vector<caching_device_t *> l2_children = { itlbs_[i], dtlbs_[i] };
        if (!itlbs_[i]->init(knobs_.TLB_L1I_assoc, (int)knobs_.page_size,
                             knobs_.TLB_L1I_entries, lltlbs_[i],
                             new tlb_stats_t((int)knobs_.page_size, warmup_enabled)) ||
            !dtlbs_[i]->init(knobs_.TLB_L1D_assoc, (int)knobs_.page_size,
                             knobs_.TLB_L1D_entries, lltlbs_[i],
                             new tlb_stats_t((int)knobs_.page_size, warmup_enabled)) ||
            !lltlbs_[i]->init(knobs_.TLB_L2_assoc, (int)knobs_.page_size,
                              knobs_.TLB_L2_entries, NULL,
                              new tlb_stats_t((int)knobs_.page_size, warmup_enabled),
                              nullptr, cache_inclusion_policy_t::NON_INC_NON_EXC, false,
                              -1, nullptr, l2_children)) {
            error_string_ =
                "Usage error: failed to initialize TLbs_. Ensure entry number, "
                "page size and associativity are powers of 2.";
            success_ = false;
            return;
        }

        // If coupled PTW data-cache simulation is requested, create an internal L1D cache.
        caching_device_t *coupled_dcache = nullptr;
        if (knobs_.couple_ptw_to_dcache) {
            std::unique_ptr<cache_t> dcache(new cache_lru_t("ptw_dcache"));
            int line_sz = static_cast<int>(knobs_.ptw_dcache_line_size);
            int total_sz = static_cast<int>(knobs_.ptw_dcache_size);
            int assoc = static_cast<int>(knobs_.ptw_dcache_assoc);
            if (!dcache->init(assoc, line_sz, total_sz, nullptr,
                              new cache_stats_t(line_sz, "", warmup_enabled))) {
                error_string_ =
                    "Usage error: failed to initialize coupled PTW data cache.";
                success_ = false;
                return;
            }
            coupled_dcache = dcache.get();
            internal_ptw_dcaches_[i] = std::move(dcache);
        }

        // Construct the per-core hardware Page Table Walker (and its Page Walk Cache).
        walkers_[i].reset(new page_table_walker_t(
            static_cast<int>(i), address_space_mgr_.get(), knobs_.TLB_pwc_entries,
            knobs_.TLB_pwc_assoc, coupled_dcache));

        // Attach the hardware Page Table Walker to the core's L2 TLB when PTW or coupled
        // PTW cache mode is enabled.
        if (knobs_.enable_page_table_walk || knobs_.couple_ptw_to_dcache) {
            lltlbs_[i]->set_page_table_walker(walkers_[i].get());
        }

        // Attach the shared huge-page detector to L1I, L1D, and L2 TLBs when huge-page
        // simulation is enabled.
        if (knobs_.enable_huge_pages) {
            const huge_page_detector_t *hp_det =
                &address_space_mgr_->get_huge_page_detector();
            itlbs_[i]->set_huge_page_detector(hp_det);
            dtlbs_[i]->set_huge_page_detector(hp_det);
            lltlbs_[i]->set_huge_page_detector(hp_det);
        }
    }
}

tlb_simulator_t::~tlb_simulator_t()
{
    // Clean up all per-core TLB devices and their statistics objects.
    for (unsigned int i = 0; i < knobs_.num_cores; i++) {
        // Delete internal coupled PTW data cache stats if allocated.
        if (i < internal_ptw_dcaches_.size() && internal_ptw_dcaches_[i] != nullptr) {
            delete internal_ptw_dcaches_[i]->get_stats();
            internal_ptw_dcaches_[i].reset();
        }
        // Try to handle failure during construction safely.
        if (itlbs_[i] == NULL)
            break;
        delete itlbs_[i]->get_stats();
        delete itlbs_[i];
        if (dtlbs_[i] == NULL)
            break;
        delete dtlbs_[i]->get_stats();
        delete dtlbs_[i];
        if (lltlbs_[i] == NULL)
            break;
        delete lltlbs_[i]->get_stats();
        delete lltlbs_[i];
    }
    // Delete the per-core pointer arrays.
    delete[] itlbs_;
    delete[] dtlbs_;
    delete[] lltlbs_;
}

std::string
tlb_simulator_t::create_v2p_from_file(std::istream &v2p_file)
{
    // If we are not using physical addresses, we don't need a virtual to physical mapping
    // at all.
    if (!knobs_.use_physical)
        return "";

    // Parse the v2p.textproto stream using base simulator_t::create_v2p_from_file().
    std::string error_str = simulator_t::create_v2p_from_file(v2p_file);
    if (!error_str.empty()) {
        return error_str;
    }
    // Overwrite tlb_simulator_t.knobs_.page_size with simulator_t.page_size_, which is
    // set to be the page size in v2p_file.
    knobs_.page_size = page_size_;
    // Load the parsed virtual-to-physical map into the MMU address space manager so the
    // 4-level radix page table and contiguous-PFN huge-page detector can use it.
    if (address_space_mgr_ != nullptr) {
        address_space_mgr_->load_external_v2p_map(0, virt2phys_);
    }
    return "";
}

bool
tlb_simulator_t::check_warmed_up()
{
    // If already warmed up, return true immediately.
    if (is_warmed_up_) {
        return true;
    }
    // Check if warmup_fraction is enabled and whether all L2 TLBs have reached threshold.
    if (knobs_.warmup_fraction > 0.0) {
        bool all_warm = true;
        for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
            if (lltlbs_[i]->get_loaded_fraction() < knobs_.warmup_fraction) {
                all_warm = false;
                break;
            }
        }
        if (all_warm) {
            is_warmed_up_ = true;
            reset_all_stats();
            return true;
        }
    }
    return false;
}

void
tlb_simulator_t::reset_all_stats()
{
    // Reset L1I, L1D, L2 TLB stats, MMU/PTW stats, and coupled cache stats on all cores.
    for (unsigned int i = 0; i < knobs_.num_cores; i++) {
        itlbs_[i]->get_stats()->reset();
        dtlbs_[i]->get_stats()->reset();
        lltlbs_[i]->get_stats()->reset();
        if (i < walkers_.size() && walkers_[i] != nullptr) {
            walkers_[i]->get_stats().reset();
        }
        if (i < internal_ptw_dcaches_.size() && internal_ptw_dcaches_[i] != nullptr &&
            internal_ptw_dcaches_[i]->get_stats() != nullptr) {
            internal_ptw_dcaches_[i]->get_stats()->reset();
        }
    }
}

bool
tlb_simulator_t::process_memref(const memref_t &memref)
{
    // Honor skip_refs at the beginning of the trace.
    if (knobs_.skip_refs > 0) {
        knobs_.skip_refs--;
        return true;
    }

    // The references after warmup and simulated ones are dropped.
    if (is_warmed_up_ && knobs_.sim_refs == 0)
        return true;

    // Both warmup and simulated references are simulated.
    if (!simulator_t::process_memref(memref))
        return false;

    if (memref.marker.type == TRACE_TYPE_MARKER) {
        // If a virtual-to-physical mapping marker was just processed by simulator_t,
        // also register it in the MMU address space manager for PTW and huge-page detection.
        if (knobs_.use_physical && !use_v2p_file_ && page_size_ > 0 &&
            memref.marker.marker_type == TRACE_MARKER_TYPE_VIRTUAL_ADDRESS &&
            address_space_mgr_ != nullptr) {
            addr_t vpage = page_start(memref.marker.marker_value);
            auto it = virt2phys_.find(vpage);
            if (it != virt2phys_.end()) {
                address_space_mgr_->register_external_v2p_mapping(memref.marker.pid,
                                                                  vpage, it->second);
            }
        }
        // We ignore markers before we ask core_for_thread, to avoid asking
        // too early on a timestamp marker.
        return true;
    }

    // We use a static scheduling of threads to cores, as it is
    // not practical to measure which core each thread actually
    // ran on for each memref.
    int core_index;
    if (memref.data.tid == last_thread_)
        core_index = last_core_index_;
    else {
        core_index = core_for_thread(memref.data.tid);
        last_thread_ = memref.data.tid;
        last_core_index_ = core_index;
    }

    // To support legacy physical-address swapping when page table walk simulation is off
    // without modifying the passed-in memref, we use indirection.
    // When enable_page_table_walk is true, hardware TLBs are indexed by virtual page
    // number (VPN) and store the translated PPN inside tlb_entry_t::ppn_.
    const memref_t *simref = &memref;
    memref_t phys_memref;
    if (knobs_.use_physical && !knobs_.enable_page_table_walk) {
        phys_memref = memref2phys(memref);
        simref = &phys_memref;
    }

    if (type_is_instr(simref->instr.type)) {
        // Route instruction fetch to the core's private L1 instruction TLB.
        itlbs_[core_index]->request(*simref);
    } else if (simref->data.type == TRACE_TYPE_READ ||
               simref->data.type == TRACE_TYPE_WRITE) {
        // Route data load/store to the core's private L1 data TLB.
        dtlbs_[core_index]->request(*simref);
        // If an internal coupled PTW data cache is active on this core, also issue the
        // translated physical data access into the data cache so application data and
        // PTE walks realistically share the cache.
        if (knobs_.couple_ptw_to_dcache &&
            internal_ptw_dcaches_[core_index] != nullptr) {
            memref_t data_phys_ref = *simref;
            addr_t ppn = 0;
            page_size_kind_t pkind = page_size_kind_t::PAGE_SIZE_4KB;
            if (dtlbs_[core_index]->lookup_entry(simref->data.pid, simref->data.addr,
                                                 &ppn, &pkind)) {
                addr_t pbase = ppn << compute_log2((int)knobs_.page_size);
                addr_t poff =
                    page_size_helper_t::extract_page_offset(simref->data.addr, pkind);
                data_phys_ref.data.addr = pbase + poff;
            }
            internal_ptw_dcaches_[core_index]->request(data_phys_ref);
        }
    } else if (simref->exit.type == TRACE_TYPE_THREAD_EXIT) {
        handle_thread_exit(simref->exit.tid);
        last_thread_ = 0;
    } else if (type_is_prefetch(simref->data.type) ||
               simref->flush.type == TRACE_TYPE_INSTR_FLUSH ||
               simref->flush.type == TRACE_TYPE_DATA_FLUSH ||
               simref->marker.type == TRACE_TYPE_MARKER ||
               simref->marker.type == TRACE_TYPE_INSTR_NO_FETCH) {
        // TLB simulator ignores prefetching, cache flushing, and markers.
    } else {
        error_string_ = "Unhandled memref type " + std::to_string(simref->data.type);
        return false;
    }

    if (knobs_.verbose >= 3) {
        std::cerr << "::" << simref->data.pid << "." << simref->data.tid << ":: "
                  << " @" << (void *)simref->data.pc << " "
                  << trace_type_names[simref->data.type] << " "
                  << (void *)simref->data.addr << " x" << simref->data.size << std::endl;
    }

    // Process counters for warmup and simulated references.
    if (!is_warmed_up_) {
        if (knobs_.warmup_refs > 0) {
            knobs_.warmup_refs--;
            // Reset TLB and MMU stats when warmup_refs reaches zero.
            if (knobs_.warmup_refs == 0) {
                is_warmed_up_ = true;
                reset_all_stats();
            }
        } else if (knobs_.warmup_fraction > 0.0) {
            check_warmed_up();
        }
    } else {
        knobs_.sim_refs--;
    }
    return true;
}

void
tlb_simulator_t::perform_tlb_shootdown(memref_pid_t pid, addr_t vaddr)
{
    // Unmap the page in the process's 4-level radix page table if present.
    if (address_space_mgr_ != nullptr) {
        radix_page_table_t *pt = address_space_mgr_->get_or_create_page_table(pid);
        pt->unmap_page(vaddr);
    }
    // Broadcast the shootdown invalidation across all simulated CPU cores' L2/L1 TLBs
    // and per-core Page Walk Caches.
    for (unsigned int i = 0; i < knobs_.num_cores; ++i) {
        if (lltlbs_[i] != nullptr) {
            lltlbs_[i]->invalidate_vaddr(pid, vaddr);
        }
        if (i < walkers_.size() && walkers_[i] != nullptr) {
            walkers_[i]->handle_shootdown(pid, vaddr);
        }
    }
}

void
tlb_simulator_t::set_core_ptw_data_cache(unsigned int core, caching_device_t *dcache)
{
    // Attach the external data cache to `core`'s hardware page table walker.
    if (core < walkers_.size() && walkers_[core] != nullptr) {
        walkers_[core]->set_ptw_data_cache(dcache);
        // Ensure the walker is attached to the core's L2 TLB.
        if (lltlbs_[core] != nullptr) {
            lltlbs_[core]->set_page_table_walker(walkers_[core].get());
        }
    }
}

caching_device_t *
tlb_simulator_t::get_core_ptw_data_cache(unsigned int core) const
{
    // Return the data cache currently attached to `core`'s page table walker.
    if (core < walkers_.size() && walkers_[core] != nullptr) {
        return walkers_[core]->get_ptw_data_cache();
    }
    return nullptr;
}

page_table_walker_t *
tlb_simulator_t::get_page_table_walker(unsigned int core) const
{
    // Return the page_table_walker_t for `core` if within range.
    if (core < walkers_.size()) {
        return walkers_[core].get();
    }
    return nullptr;
}

const mmu_stats_t *
tlb_simulator_t::get_mmu_stats(unsigned int core) const
{
    // Return the mmu_stats_t for `core` if within range.
    if (core < walkers_.size() && walkers_[core] != nullptr) {
        return &walkers_[core]->get_stats();
    }
    return nullptr;
}

const tlb_stats_t *
tlb_simulator_t::get_tlb_stats(unsigned int core, unsigned int level, bool is_instr) const
{
    // Validate core index.
    if (core >= knobs_.num_cores) {
        return nullptr;
    }
    // Level 1 returns L1I or L1D stats; Level 2 returns unified L2 TLB stats.
    if (level == 1) {
        tlb_t *tlb = is_instr ? itlbs_[core] : dtlbs_[core];
        return tlb != nullptr ? (const tlb_stats_t *)tlb->get_stats() : nullptr;
    } else if (level == 2) {
        return lltlbs_[core] != nullptr ? (const tlb_stats_t *)lltlbs_[core]->get_stats()
                                        : nullptr;
    }
    return nullptr;
}

bool
tlb_simulator_t::lookup_tlb_entry(unsigned int core, memref_pid_t pid, addr_t vaddr,
                                  bool is_instr, addr_t *ppn_out,
                                  page_size_kind_t *kind_out) const
{
    // Validate core index.
    if (core >= knobs_.num_cores) {
        return false;
    }
    // Check L1 TLB first, then fall back to L2 TLB.
    tlb_t *l1_tlb = is_instr ? itlbs_[core] : dtlbs_[core];
    if (l1_tlb != nullptr && l1_tlb->lookup_entry(pid, vaddr, ppn_out, kind_out)) {
        return true;
    }
    if (lltlbs_[core] != nullptr &&
        lltlbs_[core]->lookup_entry(pid, vaddr, ppn_out, kind_out)) {
        return true;
    }
    return false;
}

bool
tlb_simulator_t::print_results()
{
    // Print per-core L1I, L1D, and LL TLB statistics (and MMU/PTW statistics if enabled).
    std::cerr << "TLB simulation results:\n";
    for (unsigned int i = 0; i < knobs_.num_cores; i++) {
        if (print_core(i)) {
            std::cerr << "  L1I stats:" << std::endl;
            itlbs_[i]->get_stats()->print_stats("    ");
            std::cerr << "  L1D stats:" << std::endl;
            dtlbs_[i]->get_stats()->print_stats("    ");
            std::cerr << "  LL stats:" << std::endl;
            lltlbs_[i]->get_stats()->print_stats("    ");
            // When page table walk or coupled PTW cache is enabled, print MMU stats.
            if ((knobs_.enable_page_table_walk || knobs_.couple_ptw_to_dcache) &&
                i < walkers_.size() && walkers_[i] != nullptr) {
                std::cerr << "  MMU / Page Table Walk stats:" << std::endl;
                walkers_[i]->get_stats().print_stats("    ");
            }
            // When internal coupled PTW data cache is enabled, print its cache stats.
            if (knobs_.couple_ptw_to_dcache && i < internal_ptw_dcaches_.size() &&
                internal_ptw_dcaches_[i] != nullptr &&
                internal_ptw_dcaches_[i]->get_stats() != nullptr) {
                std::cerr << "  Coupled PTW L1D Cache stats:" << std::endl;
                internal_ptw_dcaches_[i]->get_stats()->print_stats("    ");
            }
        }
    }
    return true;
}

tlb_t *
tlb_simulator_t::create_tlb(std::string policy)
{
    // Support LFU (default), LRU, and FIFO replacement policies for TLBs.
    if (policy == REPLACE_POLICY_NON_SPECIFIED || // Default LFU.
        policy == REPLACE_POLICY_LFU) {           // Explicitly set to LFU.
        return new tlb_t;
    } else if (policy == REPLACE_POLICY_LRU) {    // Least Recently Used (LRU).
        return new tlb_lru_t;
    } else if (policy == REPLACE_POLICY_FIFO) {   // First-In-First-Out (FIFO).
        return new tlb_fifo_t;
    }

    // Undefined replacement policy.
    ERRMSG("Usage error: undefined replacement policy. "
           "Please choose " REPLACE_POLICY_LFU ", " REPLACE_POLICY_LRU ", or "
           REPLACE_POLICY_FIFO ".\n");
    return NULL;
}

} // namespace drmemtrace
} // namespace dynamorio
