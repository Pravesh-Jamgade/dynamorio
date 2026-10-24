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

// Include input/output streams for console interaction.
#include <iostream>
// Include string stream for argument parsing.
#include <sstream>
// Include string manipulation routines.
#include <string>
// Include vector container for multi-argument management.
#include <vector>
// Include standard integer types.
#include <stdint.h>
// Include C standard library for exit and memory allocation.
#include <stdlib.h>
// Include algorithm header for string transformation.
#include <algorithm>

// Include CPU simulator knobs and tool class.
#include "cpu/cpu_simulator.h"
// Include INI configuration file parser.
#include "cpu/ini_parser.h"
// Include ChampSim trace reader adapter.
#include "reader/champsim_reader.h"

// Use dynamorio::drmemtrace simulation namespace.
using namespace dynamorio::drmemtrace;

// Prints CLI usage instructions and parameter documentation.
static void
print_usage(const char *prog_name)
{
    // Print banner and program usage syntax.
    std::cout << "\n=======================================================================\n";
    // Print title of the simulator driver.
    std::cout << "  DynamoRIO Out-of-Order CPU, MMU & 3-Level Cache Trace Simulator\n";
    // Print banner separator.
    std::cout << "=======================================================================\n";
    // Print usage syntax.
    std::cout << "Usage: " << prog_name << " [options]\n\n";
    // Print configuration options section.
    std::cout << "Configuration Options:\n";
    // Print config option explanation.
    std::cout << "  --config, -c <file.ini>  Load pipeline and cache parameters from INI file\n\n";
    // Print trace options section.
    std::cout << "Trace Options:\n";
    // Print trace option explanation.
    std::cout << "  --trace <path>           Path to ChampSim binary trace (.trace, .gz, .xz)\n";
    // Print format flag option explanation.
    std::cout << "  --format <fmt>           Trace format: 'default', 'standard', 'cloudsuite', or 'auto' (default: auto)\n";
    // Print cloudsuite shortcut flag explanation.
    std::cout << "  --is_cloudsuite          Force CloudSuite 96-byte format (4 stores, 4 loads, ASID)\n";
    // Print max instruction limit explanation.
    std::cout << "  --max_instrs <N>         Maximum instructions to simulate (default: 1000000; 0 = entire trace)\n";
    // Print progress interval explanation.
    std::cout << "  --print_interval <N>     Progress heartbeat interval in instructions (default: 100000)\n\n";
    // Print CPU pipeline knobs section.
    std::cout << "CPU Pipeline Options:\n";
    // Print number of cores option explanation.
    std::cout << "  --cores <N>              Number of CPU cores (default: 1)\n";
    // Print ROB capacity option explanation.
    std::cout << "  --rob_size <N>           Reorder Buffer (ROB) capacity in entries (default: 256)\n";
    // Print LQ capacity option explanation.
    std::cout << "  --lq_size <N>            Load Queue capacity in entries (default: 72)\n";
    // Print SQ capacity option explanation.
    std::cout << "  --sq_size <N>            Store Queue capacity in entries (default: 56)\n";
    // Print BTB entries option explanation.
    std::cout << "  --btb_entries <N>        Branch Target Buffer entries (default: 4096)\n";
    // Print branch mispredict penalty explanation.
    std::cout << "  --mispredict_penalty <N> Branch misprediction penalty in cycles (default: 14)\n";
    // Print speculative execution toggle explanation.
    std::cout << "  --enable_wrong_path <0|1> Simulate speculative wrong-path execution (default: 1)\n";
    // Print maximum speculative depth explanation.
    std::cout << "  --speculative_depth <N>  Max instructions fetched along wrong path (default: 32)\n\n";
    // Print Memory and Cache hierarchy options section.
    std::cout << "Cache & MMU Options:\n";
    // Print L1I cache size option explanation.
    std::cout << "  --l1i_size <bytes>       Private L1 Instruction Cache size in bytes (default: 32768)\n";
    // Print L1I associativity option explanation.
    std::cout << "  --l1i_assoc <N>          Private L1 Instruction Cache associativity (default: 8)\n";
    // Print L1D cache size option explanation.
    std::cout << "  --l1d_size <bytes>       Private L1 Data Cache size in bytes (default: 32768)\n";
    // Print L1D associativity option explanation.
    std::cout << "  --l1d_assoc <N>          Private L1 Data Cache associativity (default: 8)\n";
    // Print L2 cache size option explanation.
    std::cout << "  --l2_size <bytes>        Private unified L2 Cache size in bytes (default: 262144)\n";
    // Print L2 associativity option explanation.
    std::cout << "  --l2_assoc <N>           Private unified L2 Cache associativity (default: 8)\n";
    // Print L3 cache size option explanation.
    std::cout << "  --l3_size <bytes>        Shared unified L3 / LLC size in bytes (default: 8388608)\n";
    // Print L3 associativity option explanation.
    std::cout << "  --l3_assoc <N>           Shared unified L3 / LLC associativity (default: 16)\n";
    // Print PWC entries option explanation.
    std::cout << "  --pwc_entries <N>        Page Walk Cache entries per core (default: 32)\n";
    // Print help option explanation.
    std::cout << "  --help, -h               Print this help message\n\n";
    // Print example command invocation.
    std::cout << "Examples:\n";
    // Print standard trace run example.
    std::cout << "  " << prog_name << " --trace /media/pravesh/Storage/ALL_TRACES/bc-0.trace.gz --max_instrs 500000\n";
    // Print CloudSuite trace run example with explicit format flag.
    std::cout << "  " << prog_name << " --trace /media/pravesh/Storage/ALL_TRACES/nutch_phase0_core1.trace.xz --format cloudsuite\n";
    // Print trailing separator.
    std::cout << "=======================================================================\n\n";
}

// Program entry point.
int
main(int argc, char **argv)
{
    // Define trace path string.
    std::string trace_path = "";
    // Define format string (default: auto).
    std::string format_str = "auto";
    // Define boolean flag for is_cloudsuite shortcut.
    bool is_cloudsuite = false;
    // Define maximum instruction limit (default: 1,000,000).
    uint64_t max_instrs = 1000000;
    // Define progress heartbeat interval.
    uint64_t print_interval = 100000;

    // Instantiate default CPU simulator configuration knobs.
    cpu_simulator_knobs_t knobs;

    // Parse command line arguments.
    for (int i = 1; i < argc; ++i) {
        // Capture current argument string.
        std::string arg = argv[i];

        // Check for help flags.
        if (arg == "--help" || arg == "-h") {
            // Print usage guidelines.
            print_usage(argv[0]);
            // Exit program cleanly.
            return 0;
        }
        // Check for INI configuration file path option.
        else if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            // Retrieve configuration file path.
            std::string config_file = argv[++i];
            // Instantiate INI configuration parser.
            ini_config_t ini_parser;
            // Attempt to load and parse the INI file.
            if (!ini_parser.load(config_file)) {
                // Report error loading INI file.
                std::cerr << "Error: Failed to load INI configuration file '" << config_file << "'\n";
                // Return failure code.
                return 1;
            }
            // Apply parsed configuration parameters to simulator knobs.
            ini_parser.apply_to_knobs(knobs);
        }
        // Check for trace file path option.
        else if (arg == "--trace" && i + 1 < argc) {
            // Store trace file path.
            trace_path = argv[++i];
        }
        // Check for explicit format string option.
        else if (arg == "--format" && i + 1 < argc) {
            // Store requested format string.
            format_str = argv[++i];
        }
        // Check for CloudSuite boolean shortcut flag.
        else if (arg == "--is_cloudsuite") {
            // Set CloudSuite flag to true.
            is_cloudsuite = true;
        }
        // Check for max instructions option.
        else if (arg == "--max_instrs" && i + 1 < argc) {
            // Parse unsigned long long integer for instruction count.
            max_instrs = std::strtoull(argv[++i], nullptr, 0);
        }
        // Check for progress heartbeat interval option.
        else if ((arg == "--print_interval" || arg == "--heartbeat_interval" || arg == "--heartbeat-interval") && i + 1 < argc) {
            // Parse unsigned long long integer for heartbeat interval.
            print_interval = std::strtoull(argv[++i], nullptr, 0); // Read interval.
            knobs.heartbeat_interval = print_interval;             // Update knob.
        }
        // Check for stats log filename option.
        else if ((arg == "--stats_file" || arg == "--stats-file") && i + 1 < argc) {
            knobs.stats_file = argv[++i];                          // Store stats output filename.
        }
        // Check for number of cores option.
        else if (arg == "--cores" && i + 1 < argc) {
            // Parse core count.
            knobs.num_cores = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for ROB capacity option.
        else if (arg == "--rob_size" && i + 1 < argc) {
            // Parse ROB capacity.
            knobs.rob_capacity = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for Load Queue capacity option.
        else if (arg == "--lq_size" && i + 1 < argc) {
            // Parse Load Queue capacity.
            knobs.lq_capacity = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for Store Queue capacity option.
        else if (arg == "--sq_size" && i + 1 < argc) {
            // Parse Store Queue capacity.
            knobs.sq_capacity = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for BTB entries option.
        else if (arg == "--btb_entries" && i + 1 < argc) {
            // Parse BTB entries.
            knobs.btb_entries = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for branch misprediction penalty option.
        else if (arg == "--mispredict_penalty" && i + 1 < argc) {
            // Parse branch penalty cycles.
            knobs.branch_mispredict_penalty = std::strtoull(argv[++i], nullptr, 0);
        }
        // Check for wrong-path speculative execution enable option.
        else if (arg == "--enable_wrong_path" && i + 1 < argc) {
            // Parse boolean toggle (1 = true, 0 = false).
            knobs.enable_wrong_path = (std::strtoul(argv[++i], nullptr, 0) != 0);
        }
        // Check for maximum speculative depth option.
        else if (arg == "--speculative_depth" && i + 1 < argc) {
            // Parse speculative depth.
            knobs.max_speculative_depth = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for L1I cache size option.
        else if (arg == "--l1i_size" && i + 1 < argc) {
            // Parse L1I size.
            knobs.l1i_size = std::strtoull(argv[++i], nullptr, 0);
        }
        // Check for L1I associativity option.
        else if (arg == "--l1i_assoc" && i + 1 < argc) {
            // Parse L1I associativity.
            knobs.l1i_assoc = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for L1D cache size option.
        else if (arg == "--l1d_size" && i + 1 < argc) {
            // Parse L1D size.
            knobs.l1d_size = std::strtoull(argv[++i], nullptr, 0);
        }
        // Check for L1D associativity option.
        else if (arg == "--l1d_assoc" && i + 1 < argc) {
            // Parse L1D associativity.
            knobs.l1d_assoc = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for L2 cache size option.
        else if (arg == "--l2_size" && i + 1 < argc) {
            // Parse L2 size.
            knobs.l2_size = std::strtoull(argv[++i], nullptr, 0);
        }
        // Check for L2 associativity option.
        else if (arg == "--l2_assoc" && i + 1 < argc) {
            // Parse L2 associativity.
            knobs.l2_assoc = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for L3 cache size option.
        else if (arg == "--l3_size" && i + 1 < argc) {
            // Parse L3 size.
            knobs.l3_size = std::strtoull(argv[++i], nullptr, 0);
        }
        // Check for L3 associativity option.
        else if (arg == "--l3_assoc" && i + 1 < argc) {
            // Parse L3 associativity.
            knobs.l3_assoc = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for PWC entries option.
        else if (arg == "--pwc_entries" && i + 1 < argc) {
            // Parse PWC entries.
            knobs.pwc_entries = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for L1I MSHR capacity option.
        else if (arg == "--l1i_mshr" && i + 1 < argc) {
            // Parse L1I MSHR capacity.
            knobs.l1i_mshr_capacity = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for L1D MSHR capacity option.
        else if (arg == "--l1d_mshr" && i + 1 < argc) {
            // Parse L1D MSHR capacity.
            knobs.l1d_mshr_capacity = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for L2 MSHR capacity option.
        else if (arg == "--l2_mshr" && i + 1 < argc) {
            // Parse L2 MSHR capacity.
            knobs.l2_mshr_capacity = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for L3 MSHR capacity option.
        else if (arg == "--l3_mshr" && i + 1 < argc) {
            // Parse L3 MSHR capacity.
            knobs.l3_mshr_capacity = std::strtoul(argv[++i], nullptr, 0);
        }
        // Check for DRAM latency option.
        else if (arg == "--dram_latency" && i + 1 < argc) {
            // Parse DRAM latency.
            knobs.dram_latency = std::strtoull(argv[++i], nullptr, 0);
        }
        // Positional argument without prefix treated as trace path.
        else if (arg[0] != '-' && trace_path.empty()) {
            // Assign positional argument to trace path.
            trace_path = arg;
        }
        // Unrecognized argument.
        else {
            // Report unknown option error.
            std::cerr << "Error: Unrecognized command line option '" << arg << "'\n";
            // Print usage guidelines.
            print_usage(argv[0]);
            // Return failure error code.
            return 1;
        }
    }

    // Validate that a trace path was provided.
    if (trace_path.empty()) {
        // Print error message regarding missing trace argument.
        std::cerr << "Error: No trace path specified! Please provide --trace <path>.\n";
        // Print usage help.
        print_usage(argv[0]);
        // Return failure code.
        return 1;
    }

    // Resolve trace format enum from string and boolean flag.
    champsim_trace_format_t format =
        champsim_trace_reader_t::parse_format_string(format_str, is_cloudsuite);

    // Instantiate ChampSim trace reader with configured path and format.
    champsim_trace_reader_t reader(trace_path, format);

    // Open trace file or streaming decompression pipeline.
    if (!reader.open()) {
        // Report file opening failure.
        std::cerr << "Error: Failed to open trace file: " << trace_path << std::endl;
        // Return failure code.
        return 1;
    }

    // Instantiate complete Out-of-Order CPU Core & 3-Level Cache Simulator.
    cpu_simulator_t sim(knobs);

    // Print simulation banner.
    std::cout << "\n=======================================================================\n";
    // Print banner header.
    std::cout << "  LAUNCHING OUT-OF-ORDER CPU & 3-LEVEL CACHE SIMULATION\n";
    // Print trace path.
    std::cout << "  Trace:        " << trace_path << "\n";
    // Print resolved trace format.
    std::cout << "  Format:       "
              << (reader.get_format() == champsim_trace_format_t::FORMAT_CLOUDSUITE
                      ? "CloudSuite (96-byte records)"
                      : "Default / Standard ChampSim (64-byte records)")
              << "\n";
    // Print cores simulated.
    std::cout << "  Cores:        " << knobs.num_cores << "\n";
    // Print ROB size.
    std::cout << "  ROB:          " << knobs.rob_capacity << " entries\n";
    // Print LSQ size.
    std::cout << "  LSQ:          LQ=" << knobs.lq_capacity << ", SQ=" << knobs.sq_capacity << " entries\n";
    // Print Wrong-Path status.
    std::cout << "  Wrong-Path:   " << (knobs.enable_wrong_path ? "ENABLED" : "DISABLED") << "\n";
    // Print Cache sizes.
    std::cout << "  Hierarchy:    L1I=" << (knobs.l1i_size / 1024) << "KB, L1D=" << (knobs.l1d_size / 1024)
              << "KB, L2=" << (knobs.l2_size / 1024) << "KB, L3=" << (knobs.l3_size / (1024 * 1024)) << "MB\n";
    // Print MSHR capacities.
    std::cout << "  MSHRs:        L1I=" << knobs.l1i_mshr_capacity << ", L1D=" << knobs.l1d_mshr_capacity
              << ", L2=" << knobs.l2_mshr_capacity << ", L3=" << knobs.l3_mshr_capacity << "\n";
    // Print max instructions.
    std::cout << "  Target:       " << (max_instrs == 0 ? "Entire trace" : std::to_string(max_instrs) + " instructions") << "\n";
    // Print banner divider.
    std::cout << "=======================================================================\n\n";

    // Run simulation loop through trace reader.
    uint64_t simulated = reader.run_simulation(sim, max_instrs, print_interval);

    // Close trace reader stream.
    reader.close();

    // Print final completed status.
    std::cout << "\nSimulation finished! Total instructions processed: " << simulated << "\n\n";

    // Output all detailed pipeline, branch predictor, ROB, LSQ, cache, and MMU metrics.
    sim.print_results();

    // Return success status code.
    return 0;
}
