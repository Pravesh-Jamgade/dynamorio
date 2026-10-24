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

/* champsim_reader.h: Lightweight trace reader adapter for both standard 64-byte
 * and CloudSuite 96-byte ChampSim binary trace formats (including Google Workload
 * Traces: .trace, .gz, .xz). Translates instruction records, multi-memory loads,
 * multi-memory stores, and ASIDs into DynamoRIO memref_t streams for the
 * Out-of-Order CPU Core and Cache Simulator.
 */

#ifndef _DRMEMTRACE_CHAMPSIM_READER_H_ // Guard against multiple inclusions.
#define _DRMEMTRACE_CHAMPSIM_READER_H_ 1 // Define inclusion guard macro.

// Provide standard integer types.
#include <stdint.h>
// Provide C file I/O operations and popen/pclose.
#include <stdio.h>
// Provide standard string class.
#include <string>
// Provide FIFO queue container for buffering unpacked memory references.
#include <queue>
// Provide standard output streams for reporting.
#include <iostream>

// Provide DynamoRIO trace memory reference representation.
#include "memref.h"
// Provide Out-of-Order CPU and cache simulator interface.
#include "cpu/cpu_simulator.h"

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief Identifies whether a ChampSim trace uses the standard 64-byte layout
 *        or the 96-byte CloudSuite layout (with 4 destination memory addresses and ASID).
 */
enum class champsim_trace_format_t {
    FORMAT_STANDARD,   // Standard 64-byte ChampSim format (2 stores, 4 loads).
    FORMAT_CLOUDSUITE, // CloudSuite 96-byte format (4 stores, 4 loads, ASID).
    FORMAT_AUTO        // Automatically detect format based on file inspection.
};

#pragma pack(push, 1) // Pack struct to 1-byte alignment for exact binary trace format.
/**
 * @brief Standard 64-byte ChampSim instruction trace record.
 *        Used by Google Datacenter traces (e.g. Nutch, Search, BFS, BC, CC) and DPC-3.
 */
struct champsim_instr_t {
    uint64_t ip;                               // Program counter (Instruction Pointer) [8 bytes].
    uint8_t is_branch;                         // 1 if branch instruction, 0 otherwise [1 byte].
    uint8_t branch_taken;                      // 1 if branch was taken, 0 if not taken [1 byte].
    uint8_t destination_registers[2];          // Output/destination registers [2 bytes].
    uint8_t source_registers[4];               // Input/source registers [4 bytes].
    uint64_t destination_memory[2];            // Output memory write addresses (stores) [16 bytes].
    uint64_t source_memory[4];                 // Input memory read addresses (loads) [32 bytes].
};
#pragma pack(pop) // Restore default structure packing alignment.

// Ensure that champsim_instr_t is strictly 64 bytes in size.
static_assert(sizeof(champsim_instr_t) == 64,
              "champsim_instr_t binary layout must be exactly 64 bytes");

/**
 * @brief CloudSuite 96-byte ChampSim instruction trace record.
 *        Includes 4 destination memory addresses (SPARC / server workloads) and ASID.
 */
struct cloudsuite_instr_t {
    uint64_t ip;                               // Program counter (Instruction Pointer) [8 bytes].
    uint8_t is_branch;                         // 1 if branch instruction, 0 otherwise [1 byte].
    uint8_t branch_taken;                      // 1 if branch was taken, 0 if not taken [1 byte].
    uint8_t destination_registers[4];          // Output/destination registers (4 regs) [4 bytes].
    uint8_t source_registers[4];               // Input/source registers (4 regs) [4 bytes].
    // Compiler inserts 6 bytes padding here for 8-byte alignment of uint64_t.
    uint64_t destination_memory[4];            // Output memory write addresses (stores: 4) [32 bytes].
    uint64_t source_memory[4];                 // Input memory read addresses (loads: 4) [32 bytes].
    uint8_t asid[2];                           // Address Space Identifier [2 bytes].
    // Compiler inserts 6 bytes padding here for 8-byte structure alignment.
};

// Ensure that cloudsuite_instr_t is strictly 96 bytes in size.
static_assert(sizeof(cloudsuite_instr_t) == 96,
              "cloudsuite_instr_t binary layout must be exactly 96 bytes");

/**
 * @brief Unified internal instruction representation bridging both trace formats.
 */
struct generic_trace_instr_t {
    uint64_t ip = 0;                           // Program counter.
    uint8_t is_branch = 0;                     // 1 if branch instruction.
    uint8_t branch_taken = 0;                  // 1 if branch was taken.
    uint8_t destination_registers[4] = {};     // Destination architectural registers.
    uint8_t source_registers[4] = {};          // Source architectural registers.
    uint64_t destination_memory[4] = {};       // Destination memory write addresses.
    uint64_t source_memory[4] = {};            // Source memory read addresses.
    uint8_t asid[2] = {0xff, 0xff};            // Process / Address Space ID.
    size_t num_dest_mems = 2;                  // Number of valid destination memory addresses.
    size_t num_src_mems = 4;                   // Number of valid source memory addresses.
};

/**
 * @brief Adapter that reads both standard (64-byte) and CloudSuite (96-byte)
 *        ChampSim traces (.trace, .gz, .xz) and streams them into DynamoRIO
 *        memref_t references for CPU and cache simulation.
 */
class champsim_trace_reader_t {
public:
    /**
     * @brief Constructs the ChampSim trace reader.
     *
     * @param trace_path Path to the ChampSim trace (.trace, .trace.gz, or .trace.xz).
     * @param format     Trace format (Standard 64B, CloudSuite 96B, or Auto-detect).
     * @param pid        Default Process ID to assign to simulated trace references.
     * @param tid        Default Thread ID to assign to simulated trace references.
     */
    explicit champsim_trace_reader_t(
        const std::string &trace_path,
        champsim_trace_format_t format = champsim_trace_format_t::FORMAT_AUTO,
        memref_pid_t pid = 1000,
        memref_tid_t tid = 1000);

    /**
     * @brief Destructor closing open file handles or decompression pipes.
     */
    virtual ~champsim_trace_reader_t();

    /**
     * @brief Opens the trace file (spawning a decompression pipe if .gz or .xz)
     *        and detects format if set to FORMAT_AUTO.
     *
     * @return True if opened successfully; false on I/O error.
     */
    bool
    open();

    /**
     * @brief Closes the open file or pipe.
     */
    void
    close();

    /**
     * @brief Reads the next instruction record, parsing into unified generic_trace_instr_t.
     *
     * @param instr Output struct receiving the parsed instruction record.
     * @return True if an instruction was read; false on EOF or error.
     */
    bool
    read_next_generic_instruction(generic_trace_instr_t &instr);

    /**
     * @brief Reads a standard 64-byte ChampSim instruction record.
     *
     * @param instr Output struct receiving the parsed instruction record.
     * @return True if read; false on EOF.
     */
    bool
    read_next_standard_instruction(champsim_instr_t &instr);

    /**
     * @brief Reads a 96-byte CloudSuite instruction record.
     *
     * @param instr Output struct receiving the parsed instruction record.
     * @return True if read; false on EOF.
     */
    bool
    read_next_cloudsuite_instruction(cloudsuite_instr_t &instr);

    /**
     * @brief Retrieves the next DynamoRIO memref_t record, unpacking instruction
     *        fetch, loads, and stores in program order.
     *
     * @param memref Output record populated with the next simulated reference.
     * @return True if a reference was obtained; false on EOF.
     */
    bool
    get_next_memref(memref_t &memref);

    /**
     * @brief Feeds instructions and memory references directly into cpu_simulator_t.
     *
     * @param sim              Reference to the configured CPU simulator.
     * @param max_instructions Maximum instructions to simulate (0 = entire trace).
     * @param print_interval   Interval in instructions for progress heartbeat (0 = quiet).
     * @return Total count of instructions processed.
     */
    uint64_t
    run_simulation(cpu_simulator_t &sim,
                   uint64_t max_instructions = 0,
                   uint64_t print_interval = 100000);

    // Query and configuration methods.
    // Return true if file or pipe handle is currently open.
    bool is_open() const { return file_handle_ != nullptr; }
    // Return true if end-of-file was reached.
    bool is_eof() const { return at_eof_; }
    // Return active trace format (FORMAT_STANDARD or FORMAT_CLOUDSUITE).
    champsim_trace_format_t get_format() const { return format_; }
    // Return true if active format is CloudSuite.
    bool is_cloudsuite() const { return format_ == champsim_trace_format_t::FORMAT_CLOUDSUITE; }
    // Return count of instructions parsed from trace.
    uint64_t instructions_read() const { return instructions_read_; }
    // Return count of memref_t records emitted into simulator.
    uint64_t memrefs_emitted() const { return memrefs_emitted_; }
    // Return original trace file path.
    const std::string &get_trace_path() const { return trace_path_; }

    /**
     * @brief Parses user-supplied format flag/string into champsim_trace_format_t enum.
     *
     * @param format_str Format name ("default", "standard", "cloudsuite", "cs", "64", "96", "auto").
     * @param is_cloudsuite_flag Optional boolean flag (-is_cloudsuite) that overrides format_str.
     * @return champsim_trace_format_t The parsed enum value.
     */
    static champsim_trace_format_t
    parse_format_string(const std::string &format_str, bool is_cloudsuite_flag = false);

    /**
     * @brief Manually overrides the trace format after construction.
     *
     * @param format The desired format (FORMAT_STANDARD, FORMAT_CLOUDSUITE, or FORMAT_AUTO).
     */
    void
    set_format(champsim_trace_format_t format)
    {
        // Store resolved format.
        format_ = format;
        // Store requested format override.
        requested_format_ = format;
    }

private:
    // Probes trace file to detect whether it is standard 64-byte or CloudSuite 96-byte.
    void
    detect_format();

    // Evaluates sequentiality score for a given candidate stride (64 or 96).
    int
    score_format_step(const std::vector<uint8_t> &data, size_t step) const;

    // Reads n bytes from the preloaded buffer or file handle.
    size_t
    read_bytes(void *dest, size_t n);

    std::string trace_path_;                    // Path to the trace file.
    champsim_trace_format_t requested_format_;  // Format requested by caller.
    champsim_trace_format_t format_;            // Active format resolved (standard or cloudsuite).
    memref_pid_t pid_;                          // Assigned default process ID.
    memref_tid_t tid_;                          // Assigned default thread ID.
    FILE *file_handle_ = nullptr;               // File or pipe handle.
    bool is_pipe_ = false;                      // True if opened via popen().
    bool at_eof_ = false;                       // True if end of file reached.
    uint64_t instructions_read_ = 0;            // Total ChampSim instruction records read.
    uint64_t memrefs_emitted_ = 0;              // Total memref_t records emitted.
    std::queue<memref_t> memref_queue_;         // Queue buffering memrefs for current instruction.
    std::vector<uint8_t> preload_buffer_;       // Initial buffer used for format auto-detection.
    size_t preload_offset_ = 0;                 // Current read cursor into preload_buffer_.
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_CHAMPSIM_READER_H_ */
