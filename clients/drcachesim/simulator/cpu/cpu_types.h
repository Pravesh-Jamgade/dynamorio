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

/* cpu_types.h: Value types, enums, instruction representations, and pipeline
 * structures for out-of-order CPU core simulation.
 */

#ifndef _DRMEMTRACE_CPU_TYPES_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_CPU_TYPES_H_ 1 // Define inclusion guard macro.

// Provide standard integer types.
#include <stdint.h>
// Provide standard string class.
#include <string>
// Provide dynamic array container.
#include <vector>

// Provide trace definitions and addresses.
#include "memref.h"
#include "trace_entry.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief High-level instruction categories for microarchitectural execution.
 */
enum class cpu_op_type_t {
    OP_ALU,            // Arithmetic, logic, shift, or general integer register op.
    OP_LOAD,           // Memory read operation.
    OP_STORE,          // Memory write operation.
    OP_BRANCH_COND,    // Conditional branch (requires direction and target prediction).
    OP_BRANCH_UNCOND,  // Unconditional direct branch or jump.
    OP_CALL,           // Function call instruction (pushes return address).
    OP_RETURN,         // Function return instruction (pops return address).
    OP_OTHER           // System calls, synchronization, or unclassified instructions.
};

/**
 * @brief Out-of-order pipeline stages for an in-flight instruction.
 */
enum class pipeline_stage_t {
    STAGE_FETCH,       // Retrieved from instruction cache / trace.
    STAGE_DECODE,      // Decoded into architectural operation and register operands.
    STAGE_RENAME,      // Register dependencies mapped in scoreboard / RAT.
    STAGE_DISPATCH,    // Inserted into Reorder Buffer (ROB) and Issue Queue / LSQ.
    STAGE_ISSUE,       // Waiting for operands to become ready.
    STAGE_EXECUTE,     // Computation in execution unit / ALU.
    STAGE_MEMORY,      // Memory access (TLB lookup, cache hierarchy, LSQ forwarding).
    STAGE_WRITEBACK,   // Results produced; broadcast to dependent instructions.
    STAGE_COMMIT       // In-order retirement from the head of the ROB.
};

/**
 * @brief Outcome of branch target and direction prediction for a branch.
 */
struct branch_prediction_t {
    bool is_branch = false;          // True if instruction is any branch or call/ret.
    bool is_conditional = false;     // True if instruction is a conditional branch.
    bool predicted_taken = false;    // Direction predicted by branch predictor.
    bool actual_taken = false;       // Ground truth direction observed in trace.
    addr_t predicted_target = 0;     // Target address predicted by BTB.
    addr_t actual_target = 0;        // Actual target address taken in trace.
    bool direction_mispredicted = false; // True if direction was mispredicted.
    bool target_mispredicted = false;    // True if target address was mispredicted.

    // Returns whether any misprediction occurred requiring a pipeline flush.
    bool
    is_mispredicted() const
    {
        return direction_mispredicted || target_mispredicted; // Return combined status.
    }
};

/**
 * @brief Represents a dynamic instruction instance moving through the pipeline.
 */
struct dyn_instruction_t {
    uint64_t seq_id = 0;             // Globally unique monotonically increasing sequence ID.
    addr_t pc = 0;                   // Program counter of the instruction.
    cpu_op_type_t op_type = cpu_op_type_t::OP_ALU; // Instruction category.
    memref_pid_t pid = 0;            // Process ID.
    memref_tid_t tid = 0;            // Thread ID.

    // Register operands (for Register Dependency Tracking & RAW hazard modeling).
    int dest_reg = -1;               // Destination register (-1 if none or memory store).
    std::vector<int> src_regs;       // Source operand registers.

    // Memory access properties (for Load/Store instructions).
    addr_t mem_addr = 0;             // Target virtual address.
    size_t mem_size = 0;             // Access width in bytes.
    bool is_write = false;           // True for store, false for load.

    // Microarchitectural tracking flags.
    bool is_wrong_path = false;      // True if fetched along a mispredicted branch path.
    branch_prediction_t branch_info; // Branch prediction metadata if a branch.
    pipeline_stage_t stage = pipeline_stage_t::STAGE_FETCH; // Current pipeline stage.
    uint64_t fetch_cycle = 0;        // Cycle when fetched.
    uint64_t dispatch_cycle = 0;     // Cycle when dispatched into ROB.
    uint64_t issue_cycle = 0;        // Cycle when issued to execution unit.
    uint64_t ready_cycle = 0;        // Cycle when data/execution completes.
    uint64_t commit_cycle = 0;       // Cycle when retired from ROB.
    bool ready_for_commit = false;   // True when execution/memory completed.
    bool rob_head_stalled = false;   // True if stalled at the head of the ROB.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_CPU_TYPES_H_ */
