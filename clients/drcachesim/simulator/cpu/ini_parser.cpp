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
#include "cpu/ini_parser.h"
// Include CPU simulator knobs definition.
#include "cpu/cpu_simulator.h"
// Include file stream for reading .ini files.
#include <fstream>
// Include string stream for text processing.
#include <sstream>
// Include console output for logging.
#include <iostream>
// Include algorithm for string manipulation.
#include <algorithm>

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

ini_config_t::ini_config_t()
{
    // Constructor body.
}

std::string
ini_config_t::trim(const std::string &str)
{
    // Find first non-whitespace character.
    size_t first = str.find_first_not_of(" \t\r\n");
    // If no non-whitespace characters found, return empty string.
    if (first == std::string::npos) {
        return "";
    }
    // Find last non-whitespace character.
    size_t last = str.find_last_not_of(" \t\r\n");
    // Return substring without whitespace padding.
    return str.substr(first, (last - first + 1));
}

std::string
ini_config_t::to_lower(const std::string &str)
{
    // Create copy.
    std::string s = str;
    // Transform all characters to lowercase.
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    // Return lowercase copy.
    return s;
}

uint64_t
ini_config_t::parse_bytes_value(const std::string &val, uint64_t default_val)
{
    // Clean and lowercase input string.
    std::string s = to_lower(trim(val));
    // Check for empty string.
    if (s.empty()) {
        return default_val;
    }

    // Determine multiplier suffix.
    uint64_t multiplier = 1;
    // Check for Gigabytes (g, gb).
    if (s.length() >= 2 && s.substr(s.length() - 2) == "gb") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        s = s.substr(0, s.length() - 2);
    } else if (!s.empty() && s.back() == 'g') {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        s.pop_back();
    }
    // Check for Megabytes (m, mb).
    else if (s.length() >= 2 && s.substr(s.length() - 2) == "mb") {
        multiplier = 1024ULL * 1024ULL;
        s = s.substr(0, s.length() - 2);
    } else if (!s.empty() && s.back() == 'm') {
        multiplier = 1024ULL * 1024ULL;
        s.pop_back();
    }
    // Check for Kilobytes (k, kb).
    else if (s.length() >= 2 && s.substr(s.length() - 2) == "kb") {
        multiplier = 1024ULL;
        s = s.substr(0, s.length() - 2);
    } else if (!s.empty() && s.back() == 'k') {
        multiplier = 1024ULL;
        s.pop_back();
    }

    // Trim any remaining whitespace after suffix removal.
    s = trim(s);
    // Parse integer base number.
    char *endptr = nullptr;
    uint64_t num = std::strtoull(s.c_str(), &endptr, 0);
    // Return calculated byte size.
    return num * multiplier;
}

bool
ini_config_t::load(const std::string &filepath)
{
    // Clear existing configuration.
    sections_.clear();
    // Maintain set of visited file paths to prevent infinite recursion on circular includes.
    std::set<std::string> visited_files;
    // Run recursive loader.
    return load_internal(filepath, visited_files);
}

bool
ini_config_t::load_internal(const std::string &filepath, std::set<std::string> &visited_files)
{
    // Check for circular include dependency.
    if (visited_files.find(filepath) != visited_files.end()) {
        std::cerr << "[INI Parser] Warning: Circular include detected for " << filepath << "\n";
        return true;
    }
    // Mark current file as visited.
    visited_files.insert(filepath);

    // Open configuration file stream.
    std::ifstream file(filepath.c_str());
    // Verify file opened successfully.
    if (!file.is_open()) {
        std::cerr << "[INI Parser] Error: Unable to open config file " << filepath << "\n";
        return false;
    }

    // Determine directory containing this file for relative path resolution.
    std::string base_dir = ".";
    size_t last_slash = filepath.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        base_dir = filepath.substr(0, last_slash);
    }

    // Read file line by line into stringstream.
    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();

    // Parse the file buffer.
    std::string current_section = "global";
    std::string line;
    std::vector<std::string> includes_to_process;

    // Process lines.
    while (std::getline(buffer, line)) {
        // Strip trailing comment starting with ';' or '#'.
        size_t comment_pos = line.find_first_of(";#");
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        // Trim leading and trailing whitespace.
        line = trim(line);
        // Skip empty lines.
        if (line.empty()) {
            continue;
        }

        // Check for section header: [section_name].
        if (line.front() == '[' && line.back() == ']') {
            current_section = to_lower(trim(line.substr(1, line.length() - 2)));
            continue;
        }

        // Check for key-value delimiter ('=' or ':').
        size_t delim_pos = line.find_first_of("=:");
        if (delim_pos != std::string::npos) {
            std::string key = to_lower(trim(line.substr(0, delim_pos)));
            std::string val = trim(line.substr(delim_pos + 1));

            // Check if key is a nested include instruction!
            if (key == "include" || (current_section == "include" && key == "file")) {
                std::string inc_path = val;
                // If relative path, prepend base_dir.
                if (!inc_path.empty() && inc_path[0] != '/') {
                    inc_path = base_dir + "/" + inc_path;
                }
                // Save include to process before child overrides.
                includes_to_process.push_back(inc_path);
            } else {
                // Store/overwrite key-value setting in current section.
                sections_[current_section][key] = val;
            }
        }
    }

    // Process nested includes first so that the current file's settings overwrite them!
    if (!includes_to_process.empty()) {
        // Backup current file's local overrides.
        auto local_overrides = sections_;
        // Clear local map to load parent includes.
        sections_.clear();

        // Recursively load each parent include in declared order.
        for (const auto &inc_path : includes_to_process) {
            if (!load_internal(inc_path, visited_files)) {
                return false;
            }
        }

        // Now overlay local overrides on top: child explicitly overwrites parent!
        for (const auto &sec_pair : local_overrides) {
            for (const auto &kv_pair : sec_pair.second) {
                sections_[sec_pair.first][kv_pair.first] = kv_pair.second;
            }
        }
    }

    return true;
}

bool
ini_config_t::parse_string(const std::string &ini_text, const std::string &base_dir)
{
    std::stringstream buffer(ini_text);
    std::string current_section = "global";
    std::string line;
    std::set<std::string> visited_files;

    while (std::getline(buffer, line)) {
        size_t comment_pos = line.find_first_of(";#");
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            current_section = to_lower(trim(line.substr(1, line.length() - 2)));
            continue;
        }

        size_t delim_pos = line.find_first_of("=:");
        if (delim_pos != std::string::npos) {
            std::string key = to_lower(trim(line.substr(0, delim_pos)));
            std::string val = trim(line.substr(delim_pos + 1));

            if (key == "include" || (current_section == "include" && key == "file")) {
                std::string inc_path = val;
                if (!inc_path.empty() && inc_path[0] != '/') {
                    inc_path = base_dir + "/" + inc_path;
                }
                load_internal(inc_path, visited_files);
            } else {
                sections_[current_section][key] = val;
            }
        }
    }
    return true;
}

std::string
ini_config_t::get_string(const std::string &section, const std::string &key, const std::string &default_val) const
{
    std::string sec = to_lower(section);
    std::string k = to_lower(key);
    auto it_sec = sections_.find(sec);
    if (it_sec != sections_.end()) {
        auto it_key = it_sec->second.find(k);
        if (it_key != it_sec->second.end()) {
            return it_key->second;
        }
    }
    return default_val;
}

int64_t
ini_config_t::get_int64(const std::string &section, const std::string &key, int64_t default_val) const
{
    std::string val = get_string(section, key, "");
    if (val.empty()) {
        return default_val;
    }
    return std::strtoll(val.c_str(), nullptr, 0);
}

uint64_t
ini_config_t::get_uint64(const std::string &section, const std::string &key, uint64_t default_val) const
{
    std::string val = get_string(section, key, "");
    if (val.empty()) {
        return default_val;
    }
    return std::strtoull(val.c_str(), nullptr, 0);
}

bool
ini_config_t::get_bool(const std::string &section, const std::string &key, bool default_val) const
{
    std::string val = to_lower(get_string(section, key, ""));
    if (val.empty()) {
        return default_val;
    }
    return (val == "1" || val == "true" || val == "yes" || val == "on");
}

uint64_t
ini_config_t::get_bytesize(const std::string &section, const std::string &key, uint64_t default_val) const
{
    std::string val = get_string(section, key, "");
    if (val.empty()) {
        return default_val;
    }
    return parse_bytes_value(val, default_val);
}

bool
ini_config_t::has_section(const std::string &section) const
{
    return sections_.find(to_lower(section)) != sections_.end();
}

bool
ini_config_t::has_key(const std::string &section, const std::string &key) const
{
    auto it_sec = sections_.find(to_lower(section));
    if (it_sec != sections_.end()) {
        return it_sec->second.find(to_lower(key)) != it_sec->second.end();
    }
    return false;
}

void
ini_config_t::apply_to_knobs(cpu_simulator_knobs_t &knobs) const
{
    // [cpu] or [core] section parameters.
    std::string cpu_sec = has_section("cpu") ? "cpu" : (has_section("core") ? "core" : "global");
    knobs.num_cores = get_uint64(cpu_sec, "num_cores", knobs.num_cores);
    knobs.fetch_width = get_uint64(cpu_sec, "fetch_width", knobs.fetch_width);
    knobs.decode_width = get_uint64(cpu_sec, "decode_width", knobs.decode_width);
    knobs.dispatch_width = get_uint64(cpu_sec, "dispatch_width", knobs.dispatch_width);
    knobs.commit_width = get_uint64(cpu_sec, "commit_width", knobs.commit_width);
    knobs.rob_capacity = get_uint64(cpu_sec, "rob_capacity", get_uint64(cpu_sec, "rob_size", knobs.rob_capacity));
    knobs.lq_capacity = get_uint64(cpu_sec, "lq_capacity", get_uint64(cpu_sec, "lq_size", knobs.lq_capacity));
    knobs.sq_capacity = get_uint64(cpu_sec, "sq_capacity", get_uint64(cpu_sec, "sq_size", knobs.sq_capacity));
    knobs.btb_entries = get_uint64(cpu_sec, "btb_entries", knobs.btb_entries);
    knobs.branch_mispredict_penalty = get_uint64(cpu_sec, "branch_mispredict_penalty", knobs.branch_mispredict_penalty);
    knobs.alu_latency = get_uint64(cpu_sec, "alu_latency", knobs.alu_latency);
    knobs.enable_wrong_path = get_bool(cpu_sec, "enable_wrong_path", knobs.enable_wrong_path);
    knobs.max_speculative_depth = get_uint64(cpu_sec, "max_speculative_depth", knobs.max_speculative_depth);

    // [l1i] or [icache] section parameters.
    std::string l1i_sec = has_section("l1i") ? "l1i" : "icache";
    knobs.l1i_size = get_bytesize(l1i_sec, "size", knobs.l1i_size);
    knobs.l1i_assoc = get_uint64(l1i_sec, "assoc", knobs.l1i_assoc);
    knobs.l1i_latency = get_uint64(l1i_sec, "latency", knobs.l1i_latency);
    knobs.l1i_mshr_capacity = get_uint64(l1i_sec, "mshr_size", get_uint64(l1i_sec, "mshr_capacity", knobs.l1i_mshr_capacity));
    knobs.l1i_rq_capacity = get_uint64(l1i_sec, "rq_size", get_uint64(l1i_sec, "rq_capacity", knobs.l1i_rq_capacity));
    knobs.l1i_pq_capacity = get_uint64(l1i_sec, "pq_size", get_uint64(l1i_sec, "pq_capacity", knobs.l1i_pq_capacity));

    // [l1d] or [dcache] section parameters.
    std::string l1d_sec = has_section("l1d") ? "l1d" : "dcache";
    knobs.l1d_size = get_bytesize(l1d_sec, "size", knobs.l1d_size);
    knobs.l1d_assoc = get_uint64(l1d_sec, "assoc", knobs.l1d_assoc);
    knobs.l1d_mshr_capacity = get_uint64(l1d_sec, "mshr_size", get_uint64(l1d_sec, "mshr_capacity", knobs.l1d_mshr_capacity));
    knobs.l1d_rq_capacity = get_uint64(l1d_sec, "rq_size", get_uint64(l1d_sec, "rq_capacity", knobs.l1d_rq_capacity));
    knobs.l1d_wq_capacity = get_uint64(l1d_sec, "wq_size", get_uint64(l1d_sec, "wq_capacity", knobs.l1d_wq_capacity));
    knobs.l1d_pq_capacity = get_uint64(l1d_sec, "pq_size", get_uint64(l1d_sec, "pq_capacity", knobs.l1d_pq_capacity));

    // [l2] section parameters.
    std::string l2_sec = "l2";
    knobs.l2_size = get_bytesize(l2_sec, "size", knobs.l2_size);
    knobs.l2_assoc = get_uint64(l2_sec, "assoc", knobs.l2_assoc);
    knobs.l2_mshr_capacity = get_uint64(l2_sec, "mshr_size", get_uint64(l2_sec, "mshr_capacity", knobs.l2_mshr_capacity));
    knobs.l2_rq_capacity = get_uint64(l2_sec, "rq_size", get_uint64(l2_sec, "rq_capacity", knobs.l2_rq_capacity));
    knobs.l2_wq_capacity = get_uint64(l2_sec, "wq_size", get_uint64(l2_sec, "wq_capacity", knobs.l2_wq_capacity));
    knobs.l2_pq_capacity = get_uint64(l2_sec, "pq_size", get_uint64(l2_sec, "pq_capacity", knobs.l2_pq_capacity));

    // [l3] or [llc] section parameters.
    std::string l3_sec = has_section("l3") ? "l3" : "llc";
    knobs.l3_size = get_bytesize(l3_sec, "size", knobs.l3_size);
    knobs.l3_assoc = get_uint64(l3_sec, "assoc", knobs.l3_assoc);
    knobs.l3_mshr_capacity = get_uint64(l3_sec, "mshr_size", get_uint64(l3_sec, "mshr_capacity", knobs.l3_mshr_capacity));
    knobs.l3_rq_capacity = get_uint64(l3_sec, "rq_size", get_uint64(l3_sec, "rq_capacity", knobs.l3_rq_capacity));
    knobs.l3_wq_capacity = get_uint64(l3_sec, "wq_size", get_uint64(l3_sec, "wq_capacity", knobs.l3_wq_capacity));
    knobs.l3_pq_capacity = get_uint64(l3_sec, "pq_size", get_uint64(l3_sec, "pq_capacity", knobs.l3_pq_capacity));

    // [mmu] section parameters.
    std::string mmu_sec = "mmu";
    knobs.pwc_entries = get_uint64(mmu_sec, "pwc_entries", knobs.pwc_entries);
    knobs.pwc_assoc = get_uint64(mmu_sec, "pwc_assoc", knobs.pwc_assoc);
    knobs.page_size = get_bytesize(mmu_sec, "page_size", knobs.page_size);

    // [l2_tlb] or [mmu] L2 TLB sector parameters.
    std::string l2_tlb_sec = has_section("l2_tlb") ? "l2_tlb" : (has_section("l2tlb") ? "l2tlb" : mmu_sec); // Section.
    knobs.l2_tlb_entries = get_uint64(l2_tlb_sec, "entries",
                               get_uint64(mmu_sec, "l2_tlb_entries",
                               get_uint64(l2_tlb_sec, "l2_tlb_entries", knobs.l2_tlb_entries))); // Total entries.
    knobs.l2_tlb_assoc = get_uint64(l2_tlb_sec, "assoc",
                             get_uint64(mmu_sec, "l2_tlb_assoc",
                             get_uint64(l2_tlb_sec, "l2_tlb_assoc", knobs.l2_tlb_assoc))); // Associativity.
    knobs.l2_tlb_sector_size = get_uint64(l2_tlb_sec, "sector_size",
                                   get_uint64(mmu_sec, "l2_tlb_sector_size",
                                   get_uint64(l2_tlb_sec, "sector",
                                   get_uint64(mmu_sec, "sector", knobs.l2_tlb_sector_size)))); // Sector size.

    // [memory] section parameters.
    std::string mem_sec = "memory";
    knobs.dram_latency = get_uint64(mem_sec, "dram_latency", knobs.dram_latency);
    knobs.line_size = get_bytesize(mem_sec, "line_size", knobs.line_size);

    // [simulation] or [cpu] section parameters for heartbeat and stats file.
    std::string sim_sec = has_section("simulation") ? "simulation" : (has_section("sim") ? "sim" : cpu_sec); // Section.
    knobs.heartbeat_interval = get_uint64(sim_sec, "heartbeat_interval",
                                          get_uint64(cpu_sec, "heartbeat_interval", knobs.heartbeat_interval)); // Interval.
    knobs.stats_file = get_string(sim_sec, "stats_file",
                                  get_string(cpu_sec, "stats_file", knobs.stats_file)); // Stats file.
}

void
ini_config_t::print_config() const
{
    std::cout << "\n=======================================================\n";
    std::cout << "          ACTIVE INI CONFIGURATION DUMP                \n";
    std::cout << "=======================================================\n";
    for (const auto &sec : sections_) {
        std::cout << "[" << sec.first << "]\n";
        for (const auto &kv : sec.second) {
            std::cout << "  " << kv.first << " = " << kv.second << "\n";
        }
    }
    std::cout << "=======================================================\n\n";
}

} // namespace drmemtrace
} // namespace dynamorio
