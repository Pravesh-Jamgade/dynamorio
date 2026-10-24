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
#include "cpu/btb.h"

// Provide formatting manipulators.
#include <iomanip>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

btb_t::btb_t(size_t num_entries)
    // Store capacity and allocate entries table.
    : num_entries_(num_entries)
    , entries_(num_entries)
{
    // Constructor body.
}

bool
btb_t::lookup(addr_t pc, addr_t &target_out) const
{
    // Hash PC to find table index.
    size_t idx = (pc >> 2) % num_entries_;
    // Check if entry is valid and matches the PC tag.
    if (entries_[idx].valid && entries_[idx].tag == pc) {
        target_out = entries_[idx].target; // Return cached target address.
        return true;                       // BTB hit.
    }
    return false;                          // BTB miss.
}

void
btb_t::update(addr_t pc, addr_t target)
{
    // Hash PC to find table index.
    size_t idx = (pc >> 2) % num_entries_;
    // Overwrite the slot with the new branch target translation.
    entries_[idx].valid = true;
    entries_[idx].tag = pc;
    entries_[idx].target = target;
}

void
btb_t::reset()
{
    // Invalidate all entries.
    for (auto &entry : entries_) {
        entry.valid = false;
        entry.tag = 0;
        entry.target = 0;
    }
}

return_address_stack_t::return_address_stack_t(size_t max_depth)
    // Store maximum depth.
    : max_depth_(max_depth)
{
    // Reserve capacity to avoid dynamic reallocations.
    stack_.reserve(max_depth_);
}

void
return_address_stack_t::push(addr_t return_addr)
{
    // If stack has reached maximum depth, discard oldest entry from front.
    if (stack_.size() >= max_depth_) {
        stack_.erase(stack_.begin());
    }
    // Push the predicted return address onto the top of the stack.
    stack_.push_back(return_addr);
}

bool
return_address_stack_t::pop(addr_t &predicted_addr)
{
    // Check if the RAS is empty.
    if (stack_.empty()) {
        return false; // Underflow.
    }
    // Retrieve top element.
    predicted_addr = stack_.back();
    // Pop from stack.
    stack_.pop_back();
    return true; // Success.
}

void
return_address_stack_t::reset()
{
    // Clear the stack.
    stack_.clear();
}

branch_predictor_t::branch_predictor_t(size_t btb_entries,
                                       size_t bimodal_entries,
                                       size_t ras_depth)
    // Initialize BTB.
    : btb_(btb_entries)
    // Initialize RAS.
    , ras_(ras_depth)
    // Bitmask for indexing power-of-2 bimodal table.
    , bimodal_mask_(bimodal_entries - 1)
    // Initialize 2-bit counters to Weakly Taken (state 2).
    , bimodal_counters_(bimodal_entries, 2)
{
    // Constructor body.
}

branch_prediction_t
branch_predictor_t::predict(addr_t pc, cpu_op_type_t op_type)
{
    // Initialize prediction struct.
    branch_prediction_t pred;

    // Check if instruction is a branch or call/ret.
    if (op_type == cpu_op_type_t::OP_BRANCH_COND) {
        pred.is_branch = true;
        pred.is_conditional = true;
        // Query 2-bit counter: values 2 and 3 predict Taken; 0 and 1 predict Not Taken.
        size_t idx = (pc >> 2) & bimodal_mask_;
        pred.predicted_taken = (bimodal_counters_[idx] >= 2);
        // If predicted taken, look up target in BTB.
        if (pred.predicted_taken) {
            addr_t btb_target = 0;
            if (btb_.lookup(pc, btb_target)) {
                pred.predicted_target = btb_target;
            }
        }
    } else if (op_type == cpu_op_type_t::OP_BRANCH_UNCOND || op_type == cpu_op_type_t::OP_CALL) {
        pred.is_branch = true;
        pred.is_conditional = false;
        pred.predicted_taken = true; // Unconditional always taken.
        addr_t btb_target = 0;
        if (btb_.lookup(pc, btb_target)) {
            pred.predicted_target = btb_target;
        }
    } else if (op_type == cpu_op_type_t::OP_RETURN) {
        pred.is_branch = true;
        pred.is_conditional = false;
        pred.predicted_taken = true;
        // Pop predicted return address from Return Address Stack (RAS).
        addr_t ras_target = 0;
        if (ras_.pop(ras_target)) {
            pred.predicted_target = ras_target;
        }
    }

    // Return preliminary prediction.
    return pred;
}

branch_prediction_t
branch_predictor_t::update(addr_t pc, cpu_op_type_t op_type, bool actual_taken,
                           addr_t actual_target, addr_t return_addr)
{
    // Obtain baseline prediction.
    branch_prediction_t pred = predict(pc, op_type);
    // If not a branch, return empty prediction.
    if (!pred.is_branch) {
        return pred;
    }

    // Record ground-truth execution outcomes.
    pred.actual_taken = actual_taken;
    pred.actual_target = actual_target;
    // Check for direction misprediction.
    pred.direction_mispredicted = (pred.predicted_taken != actual_taken);
    // Check for target misprediction (only applicable if actually taken).
    if (actual_taken && pred.predicted_target != actual_target) {
        pred.target_mispredicted = true;
    }

    // Increment branch counters.
    ++total_branches_;
    if (pred.is_conditional) {
        ++conditional_branches_;
    }
    if (pred.direction_mispredicted) {
        ++direction_mispredictions_;
    }
    if (pred.target_mispredicted) {
        ++target_mispredictions_;
    }
    if (pred.is_mispredicted()) {
        ++mispredicted_branches_;
    }

    // Update 2-bit saturating counter for conditional branches.
    if (pred.is_conditional) {
        size_t idx = (pc >> 2) & bimodal_mask_;
        uint8_t cnt = bimodal_counters_[idx];
        if (actual_taken) {
            // Increment towards Strongly Taken (max 3).
            if (cnt < 3)
                bimodal_counters_[idx] = cnt + 1;
        } else {
            // Decrement towards Strongly Not Taken (min 0).
            if (cnt > 0)
                bimodal_counters_[idx] = cnt - 1;
        }
    }

    // Update BTB when branch is taken.
    if (actual_taken) {
        btb_.update(pc, actual_target);
    }

    // Push return address onto RAS when encountering a CALL.
    if (op_type == cpu_op_type_t::OP_CALL && return_addr != 0) {
        ras_.push(return_addr);
    }

    // Return the updated outcome.
    return pred;
}

double
branch_predictor_t::misprediction_rate() const
{
    // Calculate percentage rate of mispredicted branches.
    if (total_branches_ == 0)
        return 0.0;
    return (static_cast<double>(mispredicted_branches_) /
            static_cast<double>(total_branches_)) * 100.0;
}

void
branch_predictor_t::print_stats(std::ostream &os, const std::string &prefix) const
{
    // Print formatted branch prediction statistics.
    os << prefix << "Branch Predictor & BTB stats:\n";
    os << prefix << "  Total branches:                " << std::setw(12)
       << total_branches_ << "\n";
    os << prefix << "  Conditional branches:          " << std::setw(12)
       << conditional_branches_ << "\n";
    os << prefix << "  Mispredicted branches:         " << std::setw(12)
       << mispredicted_branches_ << "\n";
    os << prefix << "  Direction mispredictions:      " << std::setw(12)
       << direction_mispredictions_ << "\n";
    os << prefix << "  Target mispredictions:         " << std::setw(12)
       << target_mispredictions_ << "\n";
    os << std::fixed << std::setprecision(2);
    os << prefix << "  Misprediction rate:            " << std::setw(11)
       << misprediction_rate() << "%\n";
}

void
branch_predictor_t::reset()
{
    // Reset internal structures and metrics.
    btb_.reset();
    ras_.reset();
    std::fill(bimodal_counters_.begin(), bimodal_counters_.end(), 2);
    total_branches_ = 0;
    conditional_branches_ = 0;
    mispredicted_branches_ = 0;
    direction_mispredictions_ = 0;
    target_mispredictions_ = 0;
}

} // namespace drmemtrace
} // namespace dynamorio
