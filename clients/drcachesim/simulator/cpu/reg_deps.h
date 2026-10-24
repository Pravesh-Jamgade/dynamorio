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

/* reg_deps.h: Register Dependency Tracker and Scoreboard modeling RAW data
 * hazards, register producer-consumer mappings, and operand ready cycles.
 */

#ifndef _DRMEMTRACE_REG_DEPS_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_REG_DEPS_H_ 1 // Define inclusion guard macro.

// Provide standard integer types.
#include <stdint.h>
// Provide dynamic array container.
#include <vector>

// Provide CPU types and dynamic instruction structures.
#include "cpu/cpu_types.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Represents the state of a single register in the scoreboard.
 */
struct register_state_t {
    bool is_ready = true;             // True if the register holds a valid/ready value.
    uint64_t producer_seq_id = 0;     // Sequence ID of the in-flight producing instruction.
    uint64_t ready_cycle = 0;         // Cycle when the register value becomes available.
};

/**
 * @brief Register Scoreboard tracking Read-After-Write (RAW) data dependencies.
 */
class register_scoreboard_t {
public:
    /**
     * @brief Constructs a scoreboard tracking a given number of architectural registers.
     *
     * @param num_registers Number of registers tracked (default 128).
     */
    explicit register_scoreboard_t(size_t num_registers = 128);

    /**
     * @brief Evaluates whether all source register operands for an instruction are ready,
     *        and determines the earliest cycle the instruction can issue.
     *
     * @param instr Dynamic instruction being evaluated.
     * @param earliest_issue_cycle Output earliest cycle when all operands are ready.
     * @return True if all source operands are ready at or before the current cycle.
     */
    bool
    check_sources_ready(const dyn_instruction_t &instr,
                        uint64_t current_cycle,
                        uint64_t &earliest_issue_cycle) const;

    /**
     * @brief Allocates the destination register of an instruction during dispatch/rename,
     *        marking the register as pending (not ready).
     *
     * @param instr Instruction being dispatched.
     */
    void
    allocate_destination(const dyn_instruction_t &instr);

    /**
     * @brief Marks the destination register of an instruction as ready upon writeback.
     *
     * @param instr Instruction completing writeback.
     * @param writeback_cycle Cycle when the result value is published on the bypass network.
     */
    void
    mark_writeback(const dyn_instruction_t &instr, uint64_t writeback_cycle);

    /**
     * @brief Squashes any in-flight register allocations produced by instructions with
     *        seq_id > squashed_seq_id (e.g., following a branch misprediction).
     *
     * @param squashed_seq_id Sequence ID of the mispredicted branch.
     */
    void
    squash_after(uint64_t squashed_seq_id);

    /**
     * @brief Resets all registers to ready state.
     */
    void
    reset();

private:
    size_t num_registers_;                       // Total tracked registers.
    std::vector<register_state_t> registers_;    // Array of register status entries.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_REG_DEPS_H_ */
