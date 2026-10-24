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
#include "reader/champsim_reader.h"

// Provide standard algorithms and string operations.
#include <algorithm>
// Provide memory copy and manipulation functions.
#include <cstring>
// Provide standard string formatting.
#include <iomanip>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

champsim_trace_format_t
champsim_trace_reader_t::parse_format_string(const std::string &format_str, bool is_cloudsuite_flag)
{
    // Check if the boolean --is_cloudsuite shortcut flag was explicitly enabled.
    if (is_cloudsuite_flag) {
        // Return CloudSuite 96-byte format directly.
        return champsim_trace_format_t::FORMAT_CLOUDSUITE;
    }
    // Make a local modifiable copy of the format string.
    std::string s = format_str;
    // Convert format string characters to lowercase for case-insensitivity.
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    // Test for CloudSuite aliases: "cloudsuite", "cs", "96", "96b".
    if (s == "cloudsuite" || s == "cs" || s == "96" || s == "96b") {
        // Return CloudSuite 96-byte layout format.
        return champsim_trace_format_t::FORMAT_CLOUDSUITE;
    }
    // Test for Standard / Default aliases: "default", "standard", "64", "64b".
    if (s == "default" || s == "standard" || s == "64" || s == "64b") {
        // Return Default / Standard 64-byte layout format.
        return champsim_trace_format_t::FORMAT_STANDARD;
    }
    // Return automatic format detection if unspecified or "auto".
    return champsim_trace_format_t::FORMAT_AUTO;
}

champsim_trace_reader_t::champsim_trace_reader_t(
    const std::string &trace_path,
    champsim_trace_format_t format,
    memref_pid_t pid,
    memref_tid_t tid)
    // Store input trace path.
    : trace_path_(trace_path)
    // Store requested format mode.
    , requested_format_(format)
    // Initialize active format to standard initially.
    , format_(format == champsim_trace_format_t::FORMAT_AUTO
                  ? champsim_trace_format_t::FORMAT_STANDARD
                  : format)
    // Store default process ID.
    , pid_(pid)
    // Store default thread ID.
    , tid_(tid)
{
    // Constructor body.
}

champsim_trace_reader_t::~champsim_trace_reader_t()
{
    // Close any active file handles or child decompression processes.
    close();
}

int
champsim_trace_reader_t::score_format_step(const std::vector<uint8_t> &data, size_t step) const
{
    // Need at least 2 records to evaluate sequential deltas.
    if (data.size() < step * 2) {
        return -100;
    }

    int score = 0;
    size_t num_records = data.size() / step;
    size_t limit = std::min(num_records - 1, static_cast<size_t>(25));

    for (size_t i = 0; i < limit; ++i) {
        size_t off1 = i * step;
        size_t off2 = (i + 1) * step;

        uint64_t ip1 = 0, ip2 = 0;
        memcpy(&ip1, &data[off1], sizeof(uint64_t));
        memcpy(&ip2, &data[off2], sizeof(uint64_t));

        uint8_t is_br1 = data[off1 + 8];
        uint8_t br_tk1 = data[off1 + 9];
        uint8_t is_br2 = data[off2 + 8];
        uint8_t br_tk2 = data[off2 + 9];

        // Flag checks: is_branch and branch_taken must strictly be 0 or 1.
        if (is_br1 > 1 || br_tk1 > 1 || is_br2 > 1 || br_tk2 > 1) {
            return -100; // Invalid flag values for instruction record.
        }

        // Program counter check: IP cannot be 0.
        if (ip1 == 0 || ip2 == 0) {
            return -100;
        }

        // When the preceding instruction is not a branch, execution is sequential.
        if (is_br1 == 0) {
            if (ip2 > ip1 && (ip2 - ip1) <= 16) {
                // Typical sequential instruction length: 1 to 16 bytes.
                score += 10;
            } else if (ip2 == ip1 || (ip2 > ip1 && (ip2 - ip1) > 1000000) || ip2 < ip1) {
                score -= 5;
            }
        }
    }

    return score;
}

size_t
champsim_trace_reader_t::read_bytes(void *dest, size_t n)
{
    uint8_t *out = static_cast<uint8_t *>(dest);
    size_t bytes_copied = 0;

    // 1. Drain any remaining preloaded bytes from initial format probing.
    if (preload_offset_ < preload_buffer_.size()) {
        size_t avail = preload_buffer_.size() - preload_offset_;
        size_t to_copy = std::min(n, avail);
        memcpy(out, &preload_buffer_[preload_offset_], to_copy);
        preload_offset_ += to_copy;
        bytes_copied += to_copy;
        out += to_copy;
        n -= to_copy;
    }

    // 2. Read remainder from open file handle if needed.
    if (n > 0 && file_handle_ != nullptr) {
        size_t nread = fread(out, 1, n, file_handle_);
        bytes_copied += nread;
    }

    return bytes_copied;
}

void
champsim_trace_reader_t::detect_format()
{
    preload_buffer_.clear();
    preload_offset_ = 0;

    // Read up to 2880 bytes (30 records of 96B) into preload buffer for probing.
    std::vector<uint8_t> temp(96 * 30);
    size_t nread = fread(temp.data(), 1, temp.size(), file_handle_);
    if (nread > 0) {
        temp.resize(nread);
        preload_buffer_ = std::move(temp);
    }

    // If caller explicitly requested standard or cloudsuite, honor it directly without probing.
    if (requested_format_ != champsim_trace_format_t::FORMAT_AUTO) {
        // Set the active format to the user-requested format.
        format_ = requested_format_;
        // Check if user requested CloudSuite format.
        if (format_ == champsim_trace_format_t::FORMAT_CLOUDSUITE) {
            // Log user-configured format.
            std::cout << "[ChampSim Reader] Using user-specified flag: CloudSuite format (96-byte records)\n";
        } else {
            // Log user-configured format.
            std::cout << "[ChampSim Reader] Using user-specified flag: Default / Standard ChampSim format (64-byte records)\n";
        }
        // Return early without running auto-detection scoring.
        return;
    }

    // Auto-detection based on sequential instruction stride scoring.
    int score_64 = score_format_step(preload_buffer_, 64);
    int score_96 = score_format_step(preload_buffer_, 96);

    if (score_96 > score_64 && score_96 > 0) {
        format_ = champsim_trace_format_t::FORMAT_CLOUDSUITE;
        std::cout << "[ChampSim Reader] Auto-detected trace format: CloudSuite (96-byte records) [score: "
                  << score_96 << " vs 64B score: " << score_64 << "]\n";
    } else {
        format_ = champsim_trace_format_t::FORMAT_STANDARD;
        std::cout << "[ChampSim Reader] Auto-detected trace format: Standard ChampSim (64-byte records) [score: "
                  << score_64 << " vs 96B score: " << score_96 << "]\n";
    }
}

bool
champsim_trace_reader_t::open()
{
    // Close prior session if already open.
    close();

    // Check if the trace path is empty.
    if (trace_path_.empty()) {
        std::cerr << "champsim_trace_reader_t: Empty trace path specified!\n";
        return false;
    }

    // Reset EOF and progress state.
    at_eof_ = false;
    instructions_read_ = 0;
    memrefs_emitted_ = 0;
    while (!memref_queue_.empty()) {
        memref_queue_.pop();
    }

    // Determine decompression strategy based on file extension.
    std::string path = trace_path_;
    if (path.length() >= 3 && path.substr(path.length() - 3) == ".gz") {
        // Gzip compressed file: spawn parallel gzip decompression stream.
        std::string cmd = "gzip -dc \"" + path + "\" 2>/dev/null";
        file_handle_ = popen(cmd.c_str(), "r");
        is_pipe_ = true;
    } else if (path.length() >= 3 && path.substr(path.length() - 3) == ".xz") {
        // XZ/LZMA compressed file: spawn parallel xzcat decompression stream.
        std::string cmd = "xzcat \"" + path + "\" 2>/dev/null || xz -dc \"" + path + "\" 2>/dev/null";
        file_handle_ = popen(cmd.c_str(), "r");
        is_pipe_ = true;
    } else if (path.length() >= 4 && path.substr(path.length() - 4) == ".bz2") {
        // Bzip2 compressed file: spawn parallel bzip2 decompression stream.
        std::string cmd = "bzip2 -dc \"" + path + "\" 2>/dev/null";
        file_handle_ = popen(cmd.c_str(), "r");
        is_pipe_ = true;
    } else {
        // Uncompressed binary trace: open directly via standard C buffered I/O.
        file_handle_ = fopen(path.c_str(), "rb");
        is_pipe_ = false;
    }

    // Validate that the stream was successfully opened.
    if (file_handle_ == nullptr) {
        std::cerr << "champsim_trace_reader_t: Failed to open trace file: " << path << "\n";
        return false;
    }

    // Probe initial bytes to automatically distinguish CloudSuite from Standard format.
    detect_format();

    return true;
}

void
champsim_trace_reader_t::close()
{
    // Clear preload buffer.
    preload_buffer_.clear();
    preload_offset_ = 0;

    // Check if a handle is currently open.
    if (file_handle_ != nullptr) {
        if (is_pipe_) {
            // Close decompression pipe.
            pclose(file_handle_);
        } else {
            // Close standard file handle.
            fclose(file_handle_);
        }
        file_handle_ = nullptr;
    }
    is_pipe_ = false;
}

bool
champsim_trace_reader_t::read_next_standard_instruction(champsim_instr_t &instr)
{
    // Ensure the stream is open.
    if (file_handle_ == nullptr) {
        if (!open()) {
            return false;
        }
    }

    // Read exactly one 64-byte standard ChampSim record.
    size_t bytes_read = read_bytes(&instr, sizeof(champsim_instr_t));
    if (bytes_read < sizeof(champsim_instr_t)) {
        at_eof_ = true;
        return false;
    }

    ++instructions_read_;
    return true;
}

bool
champsim_trace_reader_t::read_next_cloudsuite_instruction(cloudsuite_instr_t &instr)
{
    // Ensure the stream is open.
    if (file_handle_ == nullptr) {
        if (!open()) {
            return false;
        }
    }

    // Read exactly one 96-byte CloudSuite ChampSim record.
    size_t bytes_read = read_bytes(&instr, sizeof(cloudsuite_instr_t));
    if (bytes_read < sizeof(cloudsuite_instr_t)) {
        at_eof_ = true;
        return false;
    }

    ++instructions_read_;
    return true;
}

bool
champsim_trace_reader_t::read_next_generic_instruction(generic_trace_instr_t &instr)
{
    // Check which format is active.
    if (format_ == champsim_trace_format_t::FORMAT_CLOUDSUITE) {
        // Read 96-byte CloudSuite instruction record.
        cloudsuite_instr_t cs = {};
        if (!read_next_cloudsuite_instruction(cs)) {
            return false;
        }

        // Copy fields into generic structure.
        instr.ip = cs.ip;
        instr.is_branch = cs.is_branch;
        instr.branch_taken = cs.branch_taken;
        for (int i = 0; i < 4; ++i) {
            instr.destination_registers[i] = cs.destination_registers[i];
            instr.source_registers[i] = cs.source_registers[i];
            instr.destination_memory[i] = cs.destination_memory[i];
            instr.source_memory[i] = cs.source_memory[i];
        }
        instr.asid[0] = cs.asid[0];
        instr.asid[1] = cs.asid[1];
        instr.num_dest_mems = 4; // CloudSuite supports up to 4 destination memory stores.
        instr.num_src_mems = 4;  // CloudSuite supports up to 4 source memory loads.
        return true;
    } else {
        // Read standard 64-byte ChampSim instruction record.
        champsim_instr_t std_instr = {};
        if (!read_next_standard_instruction(std_instr)) {
            return false;
        }

        // Copy fields into generic structure.
        instr.ip = std_instr.ip;
        instr.is_branch = std_instr.is_branch;
        instr.branch_taken = std_instr.branch_taken;
        for (int i = 0; i < 2; ++i) {
            instr.destination_registers[i] = std_instr.destination_registers[i];
            instr.destination_memory[i] = std_instr.destination_memory[i];
        }
        for (int i = 0; i < 4; ++i) {
            instr.source_registers[i] = std_instr.source_registers[i];
            instr.source_memory[i] = std_instr.source_memory[i];
        }
        instr.asid[0] = 0xff;
        instr.asid[1] = 0xff;
        instr.num_dest_mems = 2; // Standard format supports up to 2 destination stores.
        instr.num_src_mems = 4;  // Standard format supports up to 4 source loads.
        return true;
    }
}

bool
champsim_trace_reader_t::get_next_memref(memref_t &memref)
{
    // If the unpack queue is empty, read the next instruction.
    if (memref_queue_.empty()) {
        generic_trace_instr_t gen = {};
        if (!read_next_generic_instruction(gen)) {
            return false; // Reached EOF.
        }

        // Determine PID and TID (use CloudSuite ASID if valid, else default).
        memref_pid_t pid = (gen.asid[0] != 0xff) ? static_cast<memref_pid_t>(gen.asid[0]) : pid_;
        memref_tid_t tid = (gen.asid[1] != 0xff) ? static_cast<memref_tid_t>(gen.asid[1]) : tid_;

        // 1. Enqueue the instruction fetch reference.
        memref_t fetch_ref = {};
        fetch_ref.instr.type = TRACE_TYPE_INSTR;
        fetch_ref.instr.addr = gen.ip;
        fetch_ref.instr.size = 4; // Typical instruction width.
        fetch_ref.instr.pid = pid;
        fetch_ref.instr.tid = tid;
        memref_queue_.push(fetch_ref);

        // 2. Enqueue memory read operations (Loads).
        for (size_t i = 0; i < gen.num_src_mems; ++i) {
            if (gen.source_memory[i] != 0) {
                memref_t load_ref = {};
                load_ref.data.type = TRACE_TYPE_READ;
                load_ref.data.addr = gen.source_memory[i];
                load_ref.data.size = 8; // Standard 8-byte word access.
                load_ref.data.pc = gen.ip;
                load_ref.data.pid = pid;
                load_ref.data.tid = tid;
                memref_queue_.push(load_ref);
            }
        }

        // 3. Enqueue memory write operations (Stores).
        for (size_t i = 0; i < gen.num_dest_mems; ++i) {
            if (gen.destination_memory[i] != 0) {
                memref_t store_ref = {};
                store_ref.data.type = TRACE_TYPE_WRITE;
                store_ref.data.addr = gen.destination_memory[i];
                store_ref.data.size = 8; // Standard 8-byte word access.
                store_ref.data.pc = gen.ip;
                store_ref.data.pid = pid;
                store_ref.data.tid = tid;
                memref_queue_.push(store_ref);
            }
        }
    }

    // Retrieve the front reference from the unpack queue.
    memref = memref_queue_.front();
    memref_queue_.pop();
    ++memrefs_emitted_;
    return true;
}

uint64_t
champsim_trace_reader_t::run_simulation(cpu_simulator_t &sim,
                                        uint64_t max_instructions,
                                        uint64_t print_interval)
{
    // Ensure the trace stream is initialized.
    if (!is_open()) {
        if (!open()) {
            return 0;
        }
    }

    std::cout << "[ChampSim Reader] Replaying trace ("
              << (is_cloudsuite() ? "CloudSuite 96B" : "Standard 64B")
              << " format): " << trace_path_ << "\n";

    // Loop through memory references until EOF or target instruction count reached.
    memref_t ref;
    uint64_t last_heartbeat = 0;
    while (get_next_memref(ref)) {
        // Feed reference to the out-of-order CPU simulator.
        sim.process_memref(ref);

        // Check if a progress heartbeat should be printed.
        if (print_interval > 0 &&
            (instructions_read_ - last_heartbeat >= print_interval)) {
            std::cout << "  ... simulated " << instructions_read_ << " instructions ("
                      << memrefs_emitted_ << " memrefs)\n";
            last_heartbeat = instructions_read_;
        }

        // Check whether the maximum requested instruction count has been reached.
        if (max_instructions > 0 && instructions_read_ >= max_instructions) {
            break;
        }
    }

    // Flush and drain any remaining in-flight instructions from CPU cores.
    for (unsigned int c = 0; c < sim.get_knobs().num_cores; ++c) {
        cpu_core_t *core = sim.get_core(c);
        if (core != nullptr) {
            core->flush_and_drain_pipeline();
        }
    }

    std::cout << "[ChampSim Reader] Completed simulation of " << instructions_read_
              << " instructions (" << memrefs_emitted_ << " memrefs emitted).\n";

    return instructions_read_;
}

} // namespace drmemtrace
} // namespace dynamorio
