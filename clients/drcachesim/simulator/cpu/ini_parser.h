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

// Define inclusion guard macro.
#ifndef _DRMEMTRACE_INI_PARSER_H_
#define _DRMEMTRACE_INI_PARSER_H_ 1

// Provide standard integer types.
#include <stdint.h>
// Provide string manipulation header.
#include <string>
// Provide map for hierarchical section and key-value storage.
#include <map>
// Provide set for circular include cycle detection.
#include <set>
// Provide vector for ordered sections.
#include <vector>

// Forward declare configuration knobs.
namespace dynamorio {
namespace drmemtrace {
struct cpu_simulator_knobs_t;
}
}

// Open outer DynamoRIO namespace.
namespace dynamorio {
// Open drmemtrace simulation namespace.
namespace drmemtrace {

/**
 * @brief INI file parser supporting hierarchical sections, typed retrieval,
 * and nested includes (`include = other.ini`) where child files overwrite parent settings.
 */
class ini_config_t {
public:
    /**
     * @brief Constructs an empty INI configuration.
     */
    ini_config_t();

    /**
     * @brief Loads and parses an INI configuration file.
     * Automatically resolves nested includes recursively and overwrites settings.
     *
     * @param filepath Path to the primary .ini file.
     * @return True if file loaded and parsed successfully; false on error.
     */
    bool
    load(const std::string &filepath);

    /**
     * @brief Parses INI configuration directly from a string (useful for unit tests).
     *
     * @param ini_text String containing INI formatted configuration text.
     * @param base_dir Directory to resolve any nested relative includes from.
     * @return True if parsed successfully; false on error.
     */
    bool
    parse_string(const std::string &ini_text, const std::string &base_dir = ".");

    /**
     * @brief Applies parsed INI values to a cpu_simulator_knobs_t structure.
     *
     * @param knobs Output structure receiving the configured parameters.
     */
    void
    apply_to_knobs(cpu_simulator_knobs_t &knobs) const;

    // Typed accessor methods.
    std::string
    get_string(const std::string &section, const std::string &key, const std::string &default_val = "") const;

    int64_t
    get_int64(const std::string &section, const std::string &key, int64_t default_val = 0) const;

    uint64_t
    get_uint64(const std::string &section, const std::string &key, uint64_t default_val = 0) const;

    bool
    get_bool(const std::string &section, const std::string &key, bool default_val = false) const;

    uint64_t
    get_bytesize(const std::string &section, const std::string &key, uint64_t default_val = 0) const;

    // Query methods.
    bool
    has_section(const std::string &section) const;

    bool
    has_key(const std::string &section, const std::string &key) const;

    // Dumps parsed configuration to console.
    void
    print_config() const;

private:
    // Internal recursive loader with include cycle tracking.
    bool
    load_internal(const std::string &filepath, std::set<std::string> &visited_files);

    // Helper trimming leading and trailing whitespace.
    static std::string
    trim(const std::string &str);

    // Helper converting string to lowercase.
    static std::string
    to_lower(const std::string &str);

    // Parses human-readable byte sizes with unit multipliers (K, KB, M, MB, G, GB).
    static uint64_t
    parse_bytes_value(const std::string &val, uint64_t default_val);

    // Map: section_name -> (key_name -> value_string).
    std::map<std::string, std::map<std::string, std::string>> sections_;
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _DRMEMTRACE_INI_PARSER_H_ */
