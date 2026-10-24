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

/* btb.h: Branch Target Buffer (BTB), 2-bit saturating counter branch direction
 * predictor, and Return Address Stack (RAS) for CPU front-end simulation.
 */

#ifndef _DRMEMTRACE_BTB_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_BTB_H_ 1 // Define inclusion guard macro.

// Provide standard integer types.
#include <stdint.h>
// Provide standard string class.
#include <string>
// Provide dynamic array container.
#include <vector>
// Provide standard output streams.
#include <iostream>

// Provide CPU types and branch prediction structures.
#include "cpu/cpu_types.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Entry inside the Branch Target Buffer (BTB).
 */
struct btb_entry_t {
    bool valid = false;  // True if entry contains a valid target.
    addr_t tag = 0;      // Branch instruction PC tag.
    addr_t target = 0;   // Predicted branch target PC.
};

/**
 * @brief Branch Target Buffer (BTB) caching branch target addresses.
 */
class btb_t {
public:
    /**
     * @brief Constructs a BTB with a specified entry capacity.
     *
     * @param num_entries Number of entries in the buffer (default 1024).
     */
    explicit btb_t(size_t num_entries = 1024);

    /**
     * @brief Looks up a target address for a branch instruction PC.
     *
     * @param pc         Instruction PC of the branch.
     * @param target_out Output target address if found.
     * @return True if BTB hit; false if BTB missed.
     */
    bool
    lookup(addr_t pc, addr_t &target_out) const;

    /**
     * @brief Updates or inserts a branch target in the BTB.
     *
     * @param pc     Instruction PC of the branch.
     * @param target Actual branch target address taken.
     */
    void
    update(addr_t pc, addr_t target);

    /**
     * @brief Resets all entries in the BTB.
     */
    void
    reset();

private:
    size_t num_entries_;                // Number of BTB slots.
    std::vector<btb_entry_t> entries_;  // Flat table indexed by hash of PC.
};

/**
 * @brief Return Address Stack (RAS) predicting call/return target addresses.
 */
class return_address_stack_t {
public:
    /**
     * @brief Constructs a RAS with a specified maximum depth.
     *
     * @param max_depth Maximum call stack depth (default 32).
     */
    explicit return_address_stack_t(size_t max_depth = 32);

    /**
     * @brief Pushes a return address onto the stack upon encountering a CALL.
     *
     * @param return_addr Expected return address (call PC + instruction length).
     */
    void
    push(addr_t return_addr);

    /**
     * @brief Pops a predicted return address upon encountering a RETURN.
     *
     * @param predicted_addr Output predicted return address.
     * @return True if RAS contained a return address; false if underflow.
     */
    bool
    pop(addr_t &predicted_addr);

    /**
     * @brief Resets the RAS.
     */
    void
    reset();

private:
    size_t max_depth_;             // Maximum stack capacity.
    std::vector<addr_t> stack_;    // LIFO return address buffer.
};

/**
 * @brief Complete CPU Branch Predictor combining 2-bit bimodal predictor, BTB, and RAS.
 */
class branch_predictor_t {
public:
    /**
     * @brief Constructs a branch predictor.
     *
     * @param btb_entries Number of entries in the BTB.
     * @param bimodal_entries Number of entries in the 2-bit counter table.
     * @param ras_depth   Capacity of the Return Address Stack.
     */
    explicit branch_predictor_t(size_t btb_entries = 1024,
                                size_t bimodal_entries = 4096,
                                size_t ras_depth = 32);

    /**
     * @brief Predicts direction and target for an instruction.
     *
     * @param pc      Instruction PC.
     * @param op_type High-level instruction category.
     * @return Populated branch_prediction_t with prediction metadata.
     */
    branch_prediction_t
    predict(addr_t pc, cpu_op_type_t op_type);

    /**
     * @brief Updates the branch predictor with the ground-truth branch outcome.
     *
     * @param pc           Instruction PC.
     * @param op_type      Instruction category.
     * @param actual_taken Ground truth whether branch was taken.
     * @param actual_target Ground truth target address (or fallthrough PC).
     * @param return_addr  Return address to push if this was a call.
     * @return Populated branch_prediction_t indicating if a misprediction occurred.
     */
    branch_prediction_t
    update(addr_t pc, cpu_op_type_t op_type, bool actual_taken,
           addr_t actual_target, addr_t return_addr = 0);

    /**
     * @brief Prints branch prediction statistics to an output stream.
     *
     * @param os     Output stream (e.g. std::cerr).
     * @param prefix Line prefix indentation.
     */
    void
    print_stats(std::ostream &os, const std::string &prefix = "") const;

    /**
     * @brief Resets all prediction tables and statistics counters.
     */
    void
    reset();

    // Accessors for metrics:
    int64_t total_branches() const { return total_branches_; }
    int64_t mispredicted_branches() const { return mispredicted_branches_; }
    double misprediction_rate() const;

private:
    btb_t btb_;                                // Branch Target Buffer.
    return_address_stack_t ras_;               // Return Address Stack.
    size_t bimodal_mask_;                      // Bitmask for indexing bimodal table.
    std::vector<uint8_t> bimodal_counters_;    // 2-bit saturating counters [0..3].

    // Statistics counters.
    int64_t total_branches_ = 0;               // Total branches predicted.
    int64_t conditional_branches_ = 0;         // Conditional branches.
    int64_t mispredicted_branches_ = 0;        // Total direction or target mispredictions.
    int64_t direction_mispredictions_ = 0;     // Direction mispredictions.
    int64_t target_mispredictions_ = 0;        // Target address mispredictions.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_BTB_H_ */
