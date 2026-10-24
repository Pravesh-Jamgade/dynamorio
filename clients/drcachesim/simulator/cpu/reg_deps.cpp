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
#include "cpu/reg_deps.h"

// Provide std::max.
#include <algorithm>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

register_scoreboard_t::register_scoreboard_t(size_t num_registers)
    // Store capacity and allocate registers array.
    : num_registers_(num_registers)
    , registers_(num_registers)
{
    // All registers begin in the ready state.
    reset();
}

bool
register_scoreboard_t::check_sources_ready(const dyn_instruction_t &instr,
                                           uint64_t current_cycle,
                                           uint64_t &earliest_issue_cycle) const
{
    // Start with the current simulation cycle as the baseline issue candidate.
    earliest_issue_cycle = current_cycle;
    bool all_ready = true;

    // Evaluate each source register operand.
    for (int reg : instr.src_regs) {
        // Skip invalid register IDs.
        if (reg < 0 || static_cast<size_t>(reg) >= num_registers_) {
            continue;
        }
        const auto &st = registers_[reg];
        // If operand is not yet ready, or ready at a future cycle:
        if (!st.is_ready || st.ready_cycle > current_cycle) {
            all_ready = false;
            // The instruction cannot issue until all source operands are produced.
            earliest_issue_cycle = std::max(earliest_issue_cycle, st.ready_cycle);
        }
    }

    // Return true only if all source operands are available right now.
    return all_ready;
}

void
register_scoreboard_t::allocate_destination(const dyn_instruction_t &instr)
{
    // Only instructions with a valid destination register allocate a scoreboard slot.
    if (instr.dest_reg >= 0 && static_cast<size_t>(instr.dest_reg) < num_registers_) {
        auto &st = registers_[instr.dest_reg];
        // Mark register unready; dependent instructions must wait for writeback.
        st.is_ready = false;
        st.producer_seq_id = instr.seq_id;
        st.ready_cycle = UINT64_MAX; // Will be set when writeback occurs.
    }
}

void
register_scoreboard_t::mark_writeback(const dyn_instruction_t &instr,
                                      uint64_t writeback_cycle)
{
    // Verify destination register is valid.
    if (instr.dest_reg >= 0 && static_cast<size_t>(instr.dest_reg) < num_registers_) {
        auto &st = registers_[instr.dest_reg];
        // Only mark ready if this instruction is still the latest registered producer.
        if (st.producer_seq_id == instr.seq_id) {
            st.is_ready = true;
            st.ready_cycle = writeback_cycle;
        }
    }
}

void
register_scoreboard_t::squash_after(uint64_t squashed_seq_id)
{
    // Invalidate any register mappings created by speculative instructions.
    for (auto &st : registers_) {
        if (!st.is_ready && st.producer_seq_id > squashed_seq_id) {
            // Revert back to ready.
            st.is_ready = true;
            st.producer_seq_id = 0;
            st.ready_cycle = 0;
        }
    }
}

void
register_scoreboard_t::reset()
{
    // Reset all registers to ready.
    for (auto &st : registers_) {
        st.is_ready = true;
        st.producer_seq_id = 0;
        st.ready_cycle = 0;
    }
}

} // namespace drmemtrace
} // namespace dynamorio
