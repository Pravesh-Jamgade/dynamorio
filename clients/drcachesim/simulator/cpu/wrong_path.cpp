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
#include "cpu/wrong_path.h"

// Provide std::min and formatting manipulators.
#include <algorithm>
#include <iomanip>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

speculative_wrong_path_engine_t::speculative_wrong_path_engine_t(
    const wrong_path_knobs_t &knobs)
    // Store configuration knobs.
    : knobs_(knobs)
{
    // Constructor body.
}

std::vector<dyn_instruction_t>
speculative_wrong_path_engine_t::generate_wrong_path_stream(
    uint64_t branch_seq_id,
    addr_t branch_pc,
    const branch_prediction_t &pred_info,
    size_t fetch_width,
    uint64_t penalty_cycles,
    memref_pid_t pid,
    memref_tid_t tid,
    uint64_t &next_seq_id)
{
    // Vector holding generated speculative wrong-path instructions.
    std::vector<dyn_instruction_t> wrong_path;

    // Check if wrong-path execution is enabled.
    if (!knobs_.enable_wrong_path) {
        return wrong_path;
    }

    // Increment count of mispredictions handled.
    ++stats_.mispredictions_handled;

    // 1. Determine starting address of the wrong path based on prediction mismatch.
    addr_t spec_pc = 0;
    if (pred_info.actual_taken && !pred_info.predicted_taken) {
        // Ground truth took target, but predictor predicted Not-Taken:
        // Wrong path fetched along the fallthrough path!
        spec_pc = branch_pc + 4;
    } else if (!pred_info.actual_taken && pred_info.predicted_taken) {
        // Ground truth fell through, but predictor predicted Taken:
        // Wrong path fetched along the predicted target path!
        spec_pc = (pred_info.predicted_target != 0) ? pred_info.predicted_target
                                                    : (branch_pc + 0x100);
    } else {
        // Target misprediction on indirect call/jump:
        // Wrong path fetched at predicted target address.
        spec_pc = (pred_info.predicted_target != 0) ? pred_info.predicted_target
                                                    : (branch_pc + 4);
    }

    // 2. Compute how many speculative instructions fit within the squash penalty window.
    size_t max_fetchable = static_cast<size_t>(penalty_cycles * fetch_width);
    size_t count = std::min(knobs_.max_speculative_depth, max_fetchable);

    // 3. Synthesize instructions along the mispredicted path.
    for (size_t i = 0; i < count; ++i) {
        dyn_instruction_t instr;
        instr.seq_id = next_seq_id++;
        instr.pc = spec_pc;
        instr.pid = pid;
        instr.tid = tid;
        instr.is_wrong_path = true; // Mark as speculative wrong-path!

        // Pseudo-random deterministic operation assignment based on spec_pc hash.
        uint64_t hash = (spec_pc >> 2) ^ (i * 0x9e3779b97f4a7c15ULL);
        double r = static_cast<double>(hash % 1000) / 1000.0;

        if (r < knobs_.load_probability) {
            // Speculative Load instruction.
            instr.op_type = cpu_op_type_t::OP_LOAD;
            // Synthesize realistic data memory address in user space.
            instr.mem_addr = 0x7fff00000000ULL + ((hash % 1024) * 64);
            instr.mem_size = 8;
            instr.is_write = false;
            instr.dest_reg = static_cast<int>((hash >> 4) % 32);
            instr.src_regs.push_back(static_cast<int>((hash >> 9) % 32));
            ++stats_.loads_executed;
        } else if (r < knobs_.load_probability + knobs_.store_probability) {
            // Speculative Store instruction.
            instr.op_type = cpu_op_type_t::OP_STORE;
            instr.mem_addr = 0x7fff00000000ULL + ((hash % 1024) * 64);
            instr.mem_size = 8;
            instr.is_write = true;
            instr.dest_reg = -1;
            instr.src_regs.push_back(static_cast<int>((hash >> 4) % 32));
            ++stats_.stores_buffered;
        } else if (r < knobs_.load_probability + knobs_.store_probability +
                   knobs_.branch_probability) {
            // Inner branch instruction on the wrong path.
            instr.op_type = cpu_op_type_t::OP_BRANCH_COND;
            instr.dest_reg = -1;
        } else {
            // General ALU instruction on the wrong path.
            instr.op_type = cpu_op_type_t::OP_ALU;
            instr.dest_reg = static_cast<int>((hash >> 4) % 32);
            instr.src_regs.push_back(static_cast<int>((hash >> 9) % 32));
        }

        // Add to wrong-path stream.
        wrong_path.push_back(instr);
        ++stats_.instructions_fetched;

        // Advance PC along the speculative wrong path.
        spec_pc += 4;
    }

    // Return the sequence of speculative instructions.
    return wrong_path;
}

void
speculative_wrong_path_engine_t::simulate_speculative_fetch(
    unsigned int core_id,
    const dyn_instruction_t &instr,
    three_level_cache_t *cache_hierarchy,
    tlb_t *itlb)
{
    // Access L1 Instruction TLB to model speculative instruction translation.
    if (itlb != nullptr) {
        memref_t memref = {};
        memref.instr.type = TRACE_TYPE_INSTR;
        memref.instr.addr = instr.pc;
        memref.instr.size = 4;
        memref.instr.pid = instr.pid;
        memref.instr.tid = instr.tid;
        itlb->request(memref);
    }

    // Access L1 Instruction Cache to model speculative instruction cache allocation & pollution!
    if (cache_hierarchy != nullptr) {
        memref_t memref = {};
        memref.instr.type = TRACE_TYPE_INSTR;
        memref.instr.addr = instr.pc;
        memref.instr.size = 4;
        memref.instr.pid = instr.pid;
        memref.instr.tid = instr.tid;

        // Issue instruction fetch to L1I cache.
        cache_access_result_t res = cache_hierarchy->request_instruction(core_id, memref);
        ++stats_.icache_accesses;

        if (res.hit_l1i) {
            ++stats_.icache_hits;
        } else {
            // Speculative L1I miss: filled instruction cache line from L2/L3/DRAM!
            ++stats_.icache_misses;
        }
    }
}

void
speculative_wrong_path_engine_t::simulate_speculative_load(
    unsigned int core_id,
    const dyn_instruction_t &load_instr,
    three_level_cache_t *cache_hierarchy,
    tlb_t *dtlb)
{
    // Access L1 Data TLB to model speculative address translation.
    if (dtlb != nullptr) {
        memref_t memref = {};
        memref.data.type = TRACE_TYPE_READ;
        memref.data.addr = load_instr.mem_addr;
        memref.data.size = load_instr.mem_size;
        memref.data.pid = load_instr.pid;
        memref.data.tid = load_instr.tid;
        memref.data.pc = load_instr.pc;
        dtlb->request(memref);
    }

    // Access 3-Level Data Cache hierarchy to model speculative cache allocation & pollution!
    if (cache_hierarchy != nullptr) {
        memref_t memref = {};
        memref.data.type = TRACE_TYPE_READ;
        memref.data.addr = load_instr.mem_addr;
        memref.data.size = load_instr.mem_size;
        memref.data.pid = load_instr.pid;
        memref.data.tid = load_instr.tid;
        memref.data.pc = load_instr.pc;

        // Issue access through L1D -> L2 -> L3 -> DRAM.
        cache_access_result_t res = cache_hierarchy->request(core_id, memref);
        ++stats_.cache_accesses;

        if (res.level == cache_hit_level_t::HIT_L1D ||
            res.level == cache_hit_level_t::HIT_L2 ||
            res.level == cache_hit_level_t::HIT_L3) {
            ++stats_.cache_hits;
        } else {
            // Speculative miss: brought line from DRAM into L3, L2, and L1D!
            ++stats_.cache_misses;
        }
    }
}

void
speculative_wrong_path_engine_t::record_squash(size_t count)
{
    // Record count of speculative instructions purged upon branch resolution.
    stats_.instructions_squashed += count;
}

void
speculative_wrong_path_engine_t::print_stats(std::ostream &os,
                                             const std::string &prefix) const
{
    // Print formatted wrong-path execution statistics.
    os << prefix << "Speculative Wrong-Path Execution stats:\n";
    os << prefix << "  Branch mispredictions handled: " << std::setw(12)
       << stats_.mispredictions_handled << "\n";
    os << prefix << "  Wrong-path instrs fetched:     " << std::setw(12)
       << stats_.instructions_fetched << "\n";
    os << prefix << "  Wrong-path instrs squashed:    " << std::setw(12)
       << stats_.instructions_squashed << "\n";
    os << prefix << "  Wrong-path loads executed:     " << std::setw(12)
       << stats_.loads_executed << "\n";
    os << prefix << "  Wrong-path stores buffered:    " << std::setw(12)
       << stats_.stores_buffered << "\n";
    os << prefix << "  Speculative L1I fetches:       " << std::setw(12)
       << stats_.icache_accesses << "\n";
    os << prefix << "  Speculative L1I hits:          " << std::setw(12)
       << stats_.icache_hits << "\n";
    os << prefix << "  Speculative L1I misses:        " << std::setw(12)
       << stats_.icache_misses << " (polluting L1I)\n";
    os << prefix << "  Speculative cache accesses:    " << std::setw(12)
       << stats_.cache_accesses << "\n";
    os << prefix << "  Speculative cache hits:        " << std::setw(12)
       << stats_.cache_hits << "\n";
    os << prefix << "  Speculative cache misses:      " << std::setw(12)
       << stats_.cache_misses << " (polluting cache)\n";
}

void
speculative_wrong_path_engine_t::reset()
{
    // Reset all metrics.
    stats_ = wrong_path_stats_t();
}

} // namespace drmemtrace
} // namespace dynamorio
