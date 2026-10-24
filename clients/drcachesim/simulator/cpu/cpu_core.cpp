/* **********************************************************
 * Copyright (c) 2026 Google, Inc.  All rights reserved.
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

// Implementation header.
#include "cpu/cpu_core.h"

// Provide std::max and formatting.
#include <algorithm>
#include <iomanip>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

cpu_core_t::cpu_core_t(const cpu_core_knobs_t &knobs,
                       three_level_cache_t *cache_hierarchy,
                       tlb_t *itlb,
                       tlb_t *dtlb,
                       tlb_t *l2_tlb,
                       page_table_walker_t *ptw)
    // Store configuration knobs.
    : knobs_(knobs)
    // Connect external 3-level cache hierarchy.
    , cache_hierarchy_(cache_hierarchy)
    // Connect TLB levels and hardware page table walker.
    , itlb_(itlb)
    , dtlb_(dtlb)
    , l2_tlb_(l2_tlb)
    , ptw_(ptw)
    // Initialize Branch Target Buffer & Predictor with configured entries.
    , branch_predictor_(knobs.btb_entries)
    // Initialize Reorder Buffer with configured capacity.
    , rob_(knobs.rob_capacity)
    // Initialize Load-Store Queue with configured capacities.
    , lsq_(knobs.lq_capacity, knobs.sq_capacity)
{
    // Configure speculative wrong-path engine.
    wrong_path_knobs_t wp_knobs;
    wp_knobs.enable_wrong_path = knobs_.enable_wrong_path;
    wp_knobs.max_speculative_depth = knobs_.max_speculative_depth;
    wrong_path_engine_ = speculative_wrong_path_engine_t(wp_knobs);
}

bool
cpu_core_t::process_memref(const memref_t &memref)
{
    // If the Front-End is currently stalled recovering from a branch misprediction:
    if (current_cycle_ < branch_recovery_until_cycle_) {
        // Advance the simulation clock to model the pipeline flush restart latency.
        uint64_t flush_delta = branch_recovery_until_cycle_ - current_cycle_;
        step_cycles(flush_delta);
    }

    // Build a dynamic instruction record for the incoming trace reference.
    dyn_instruction_t instr;
    instr.seq_id = next_seq_id_++;
    instr.pid = memref.data.pid;
    instr.tid = memref.data.tid;
    instr.fetch_cycle = current_cycle_;

    // 1. Classify operation and handle Front-End instruction fetch.
    if (type_is_instr(memref.instr.type)) {
        instr.pc = memref.instr.addr;
        instr.op_type = cpu_op_type_t::OP_ALU;
        // Pseudo-assign architectural register dependencies based on PC hash.
        instr.dest_reg = static_cast<int>((instr.pc >> 3) % 32);
        instr.src_regs.push_back(static_cast<int>((instr.pc >> 4) % 32));
        ++total_instructions_fetched_;

        // Route instruction fetch through L1 Instruction TLB if connected.
        if (itlb_ != nullptr) {
            itlb_->request(memref);
        }

        // Route instruction fetch through L1 Instruction Cache if connected.
        if (cache_hierarchy_ != nullptr) {
            cache_access_result_t icache_res =
                cache_hierarchy_->request_instruction(knobs_.core_id, memref);
            if (icache_res.level != cache_hit_level_t::HIT_L1I) {
                // Front-End stalls fetching missing instruction cache line from L2/L3/DRAM!
                step_cycles(icache_res.latency_cycles);
                icache_miss_stall_cycles_ += icache_res.latency_cycles;
            }
        }

        // Branch detection heuristic: if PC jumps by more than 16 bytes or backwards,
        // treat as a branch instruction.
        if (last_pc_ != 0 && (instr.pc > last_pc_ + 16 || instr.pc < last_pc_)) {
            instr.op_type = cpu_op_type_t::OP_BRANCH_COND;
            // Update the branch predictor with the observed target jump.
            branch_prediction_t pred = branch_predictor_.update(
                last_pc_, cpu_op_type_t::OP_BRANCH_COND, /*actual_taken=*/true,
                /*actual_target=*/instr.pc);

            // If the branch predictor or BTB mispredicted:
            if (pred.is_mispredicted()) {
                // Incur pipeline squash penalty.
                branch_recovery_until_cycle_ = current_cycle_ + knobs_.branch_mispredict_penalty;
                branch_squash_cycles_ += knobs_.branch_mispredict_penalty;

                // 1. Speculative Wrong-Path Execution!
                // Fetch and execute instructions along the mispredicted path before the branch resolves!
                if (wrong_path_engine_.is_enabled()) {
                    std::vector<dyn_instruction_t> wrong_path =
                        wrong_path_engine_.generate_wrong_path_stream(
                            instr.seq_id, last_pc_, pred, knobs_.fetch_width,
                            knobs_.branch_mispredict_penalty, instr.pid, instr.tid,
                            next_seq_id_);

                    // Dispatch and execute wrong-path instructions through the pipeline!
                    for (auto &wp_instr : wrong_path) {
                        // Speculative instruction fetch accesses L1I cache and ITLB!
                        wrong_path_engine_.simulate_speculative_fetch(
                            knobs_.core_id, wp_instr, cache_hierarchy_, itlb_);

                        // Allocate space in the ROB and LSQ.
                        if (rob_.can_dispatch()) {
                            rob_.dispatch(wp_instr);
                            scoreboard_.allocate_destination(wp_instr);

                            if (wp_instr.op_type == cpu_op_type_t::OP_LOAD) {
                                if (lsq_.can_dispatch_load()) {
                                    lsq_.dispatch_load(wp_instr);
                                    // Speculatively access the TLB and 3-level data cache!
                                    // This realistically pulls speculative lines into L1D/L2/L3!
                                    wrong_path_engine_.simulate_speculative_load(
                                        knobs_.core_id, wp_instr, cache_hierarchy_, dtlb_);
                                }
                            } else if (wp_instr.op_type == cpu_op_type_t::OP_STORE) {
                                if (lsq_.can_dispatch_store()) {
                                    // Stores buffer in the SQ, but NEVER commit to cache!
                                    lsq_.dispatch_store(wp_instr);
                                }
                            }
                        }
                    }
                }

                // 2. Once the branch resolves after its penalty, squash younger speculative instructions!
                size_t squashed = rob_.squash_after(instr.seq_id);
                scoreboard_.squash_after(instr.seq_id);
                lsq_.squash_after(instr.seq_id);
                wrong_path_engine_.record_squash(squashed);
            }
        }
        last_pc_ = instr.pc;
    } else if (memref.data.type == TRACE_TYPE_READ) {
        // Data Load instruction.
        instr.pc = memref.data.pc;
        instr.op_type = cpu_op_type_t::OP_LOAD;
        instr.mem_addr = memref.data.addr;
        instr.mem_size = memref.data.size;
        instr.is_write = false;
        instr.dest_reg = static_cast<int>((instr.mem_addr >> 3) % 32);
        instr.src_regs.push_back(static_cast<int>((instr.pc >> 3) % 32));
        ++total_loads_;
    } else if (memref.data.type == TRACE_TYPE_WRITE) {
        // Data Store instruction.
        instr.pc = memref.data.pc;
        instr.op_type = cpu_op_type_t::OP_STORE;
        instr.mem_addr = memref.data.addr;
        instr.mem_size = memref.data.size;
        instr.is_write = true;
        instr.dest_reg = -1; // Stores do not produce register results.
        instr.src_regs.push_back(static_cast<int>((instr.pc >> 3) % 32));
        instr.src_regs.push_back(static_cast<int>((instr.mem_addr >> 3) % 32));
        ++total_stores_;
    } else {
        // Non-memory, non-instruction markers do not consume pipeline slots.
        return true;
    }

    // 2. Dispatch the instruction into the ROB and LSQ (handling backpressure stalls).
    dispatch_instruction(instr);

    // 3. Execute the instruction through dependencies and memory hierarchy.
    execute_instruction(instr);

    // 4. Advance simulation clock and commit ready instructions in program order.
    step_cycles(1);

    return true;
}

void
cpu_core_t::dispatch_instruction(dyn_instruction_t &instr)
{
    // Check if the ROB or LSQ is full. If full, step simulation cycles until space frees up!
    while (!rob_.can_dispatch() ||
           (instr.op_type == cpu_op_type_t::OP_LOAD && !lsq_.can_dispatch_load()) ||
           (instr.op_type == cpu_op_type_t::OP_STORE && !lsq_.can_dispatch_store())) {
        // Advance clock to allow in-flight instructions to finish and retire.
        step_cycles(1);
    }

    // Record dispatch cycle.
    instr.dispatch_cycle = current_cycle_;
    instr.stage = pipeline_stage_t::STAGE_DISPATCH;

    // Dispatch into the Reorder Buffer.
    rob_.dispatch(instr);

    // Dispatch into the Load-Store Queue if a memory operation.
    if (instr.op_type == cpu_op_type_t::OP_LOAD) {
        lsq_.dispatch_load(instr);
    } else if (instr.op_type == cpu_op_type_t::OP_STORE) {
        lsq_.dispatch_store(instr);
    }

    // Allocate destination register in the scoreboard.
    scoreboard_.allocate_destination(instr);
}

void
cpu_core_t::execute_instruction(dyn_instruction_t &instr)
{
    // Evaluate when all source register operands become ready via the scoreboard.
    uint64_t operand_ready_cycle = current_cycle_;
    scoreboard_.check_sources_ready(instr, current_cycle_, operand_ready_cycle);

    // Earliest issue cycle is max(current_cycle_, operand_ready_cycle).
    uint64_t issue_cycle = std::max(current_cycle_, operand_ready_cycle);
    instr.issue_cycle = issue_cycle;
    instr.stage = pipeline_stage_t::STAGE_EXECUTE;

    uint64_t completion_cycle = issue_cycle;

    // Handle execution based on operation category.
    if (instr.op_type == cpu_op_type_t::OP_LOAD) {
        instr.stage = pipeline_stage_t::STAGE_MEMORY;
        uint64_t forwarding_store_id = 0;
        // 1. Probe Store Queue for Store-to-Load Forwarding!
        if (lsq_.check_store_forwarding(instr.seq_id, instr.mem_addr, instr.mem_size,
                                        forwarding_store_id)) {
            // Store forwarding hit: 1 cycle latency bypass without accessing cache/TLB!
            completion_cycle = issue_cycle + 1;
        } else {
            // 2. Not forwarded: must access DTLB and 3-level data cache hierarchy.
            uint64_t tlb_latency = 1; // Default 1-cycle L1 DTLB hit.
            if (dtlb_ != nullptr) {
                memref_t memref = {};
                memref.data.type = TRACE_TYPE_READ;
                memref.data.addr = instr.mem_addr;
                memref.data.size = instr.mem_size;
                memref.data.pid = instr.pid;
                memref.data.tid = instr.tid;
                memref.data.pc = instr.pc;
                dtlb_->request(memref);
            }

            // 3. Probe 3-Level Data Cache Hierarchy (L1D -> L2 -> L3 -> DRAM).
            uint64_t cache_latency = 4; // Default L1D hit latency.
            if (cache_hierarchy_ != nullptr) {
                memref_t memref = {};
                memref.data.type = TRACE_TYPE_READ;
                memref.data.addr = instr.mem_addr;
                memref.data.size = instr.mem_size;
                memref.data.pid = instr.pid;
                memref.data.tid = instr.tid;
                memref.data.pc = instr.pc;
                cache_access_result_t c_res = cache_hierarchy_->request(knobs_.core_id, memref);
                cache_latency = c_res.latency_cycles;
            }

            // Total load completion latency.
            completion_cycle = issue_cycle + tlb_latency + cache_latency;
        }
    } else if (instr.op_type == cpu_op_type_t::OP_STORE) {
        // Store instructions write their value into the Store Queue in 1 cycle,
        // and only drain to the L1D cache upon in-order ROB commit!
        completion_cycle = issue_cycle + 1;
    } else {
        // General ALU instructions complete after configured execution latency.
        completion_cycle = issue_cycle + knobs_.alu_latency;
    }

    // Publish result to the scoreboard on writeback.
    scoreboard_.mark_writeback(instr, completion_cycle);
    // Mark instruction ready in the Reorder Buffer at completion_cycle.
    rob_.mark_ready(instr.seq_id, completion_cycle);
}

void
cpu_core_t::step_cycles(uint64_t num_cycles)
{
    // Advance simulation clock by num_cycles.
    for (uint64_t c = 0; c < num_cycles; ++c) {
        ++current_cycle_;

        // 1. In-order retirement: commit up to commit_width completed instructions from ROB.
        std::vector<dyn_instruction_t> retired_instrs;
        size_t committed = rob_.commit(knobs_.commit_width, current_cycle_, retired_instrs);
        total_instructions_retired_ += committed;

        // Process retiring instructions.
        for (const auto &ret_instr : retired_instrs) {
            if (ret_instr.op_type == cpu_op_type_t::OP_LOAD) {
                // Retire load from Load Queue.
                lsq_.commit_load(ret_instr.seq_id);
            } else if (ret_instr.op_type == cpu_op_type_t::OP_STORE) {
                // Mark store committed in Store Queue, allowing it to drain to L1D cache.
                lsq_.commit_store(ret_instr.seq_id);
            }
        }

        // 2. Drain committed stores from Store Queue to L1D cache in program order.
        std::vector<dyn_instruction_t> drained_stores;
        lsq_.drain_stores(knobs_.commit_width, drained_stores);
        for (const auto &st_instr : drained_stores) {
            // Issue store write to the 3-level data cache hierarchy.
            if (cache_hierarchy_ != nullptr) {
                memref_t memref = {};
                memref.data.type = TRACE_TYPE_WRITE;
                memref.data.addr = st_instr.mem_addr;
                memref.data.size = st_instr.mem_size;
                memref.data.pid = st_instr.pid;
                memref.data.tid = st_instr.tid;
                memref.data.pc = st_instr.pc;
                cache_hierarchy_->request(knobs_.core_id, memref);
            }
        }
    }
}

void
cpu_core_t::flush_and_drain_pipeline()
{
    // Drain all remaining in-flight instructions from the ROB.
    while (!rob_.empty()) {
        step_cycles(1);
    }
}

double
cpu_core_t::get_ipc() const
{
    // Calculate Instructions Per Cycle.
    if (current_cycle_ == 0)
        return 0.0;
    return static_cast<double>(total_instructions_retired_) /
        static_cast<double>(current_cycle_);
}

double
cpu_core_t::get_cpi() const
{
    // Calculate Cycles Per Instruction.
    if (total_instructions_retired_ == 0)
        return 0.0;
    return static_cast<double>(current_cycle_) /
        static_cast<double>(total_instructions_retired_);
}

void
cpu_core_t::print_stats(std::ostream &os, const std::string &prefix) const
{
    // Print formatted CPU Core performance summary.
    os << prefix << "Core #" << knobs_.core_id << " Out-of-Order CPU Pipeline stats:\n";
    os << prefix << "  Total instructions fetched:    " << std::setw(12)
       << total_instructions_fetched_ << "\n";
    os << prefix << "  Total instructions retired:    " << std::setw(12)
       << total_instructions_retired_ << "\n";
    os << prefix << "  Total loads:                   " << std::setw(12)
       << total_loads_ << "\n";
    os << prefix << "  Total stores:                  " << std::setw(12)
       << total_stores_ << "\n";
    os << prefix << "  Branch squash penalty cycles:  " << std::setw(12)
       << branch_squash_cycles_ << "\n";
    os << prefix << "  L1I cache miss stall cycles:   " << std::setw(12)
       << icache_miss_stall_cycles_ << "\n";
    os << prefix << "  Total execution cycles:        " << std::setw(12)
       << current_cycle_ << "\n";
    os << std::fixed << std::setprecision(4);
    os << prefix << "  Overall CPI:                   " << std::setw(12)
       << get_cpi() << "\n";
    os << prefix << "  Overall IPC:                   " << std::setw(12)
       << get_ipc() << "\n";

    // Print sub-component breakdowns.
    branch_predictor_.print_stats(os, prefix + "  ");
    rob_.print_stats(os, prefix + "  ");
    lsq_.print_stats(os, prefix + "  ");
    wrong_path_engine_.print_stats(os, prefix + "  ");
}

void
cpu_core_t::reset()
{
    // Reset all internal structures and metrics.
    branch_predictor_.reset();
    scoreboard_.reset();
    rob_.reset();
    lsq_.reset();
    wrong_path_engine_.reset();
    current_cycle_ = 1;
    next_seq_id_ = 1;
    branch_recovery_until_cycle_ = 0;
    last_pc_ = 0;
    total_instructions_fetched_ = 0;
    total_instructions_retired_ = 0;
    total_loads_ = 0;
    total_stores_ = 0;
    branch_squash_cycles_ = 0;
    icache_miss_stall_cycles_ = 0;
}

} // namespace drmemtrace
} // namespace dynamorio
