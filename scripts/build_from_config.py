#!/usr/bin/env python3
# Script to read and validate nested INI configurations, compile the O3 simulator,
# and optionally launch trace simulation.

# Import standard operating system interface.
import os
# Import system parameters and functions.
import sys
# Import command-line argument parser.
import argparse
# Import subprocess for invoking make and simulator.
import subprocess

def parse_nested_ini(file_path, visited=None):
    # Initialize visited set if not provided.
    if visited is None:
        visited = set()
    # Normalize absolute path to prevent cyclic inclusion loops.
    abs_path = os.path.abspath(file_path)
    # Check if this file was already visited.
    if abs_path in visited:
        print(f"Warning: Circular include detected for {file_path}, skipping.")
        return {}
    # Mark file as visited.
    visited.add(abs_path)
    # Ensure file exists.
    if not os.path.isfile(abs_path):
        print(f"Error: INI configuration file not found: {file_path}")
        sys.exit(1)

    # Base directory of current INI file for resolving relative includes.
    base_dir = os.path.dirname(abs_path)
    # List of nested include paths to load first.
    includes_to_load = []
    # Local section dictionary: section -> (key -> value).
    local_sections = {}
    # Current active section header (default: global).
    current_section = "global"

    # Read and parse file line by line.
    with open(abs_path, 'r', encoding='utf-8') as f:
        for line in f:
            # Strip trailing comments starting with ';' or '#'.
            c_pos = -1
            for comment_char in [';', '#']:
                pos = line.find(comment_char)
                if pos != -1 and (c_pos == -1 or pos < c_pos):
                    c_pos = pos
            if c_pos != -1:
                line = line[:c_pos]
            # Strip whitespace.
            sline = line.strip()
            # Skip empty lines.
            if not sline:
                continue
            # Check for section header: [section].
            if sline.startswith('[') and sline.endswith(']'):
                current_section = sline[1:-1].strip().lower()
                continue
            # Check for key-value delimiter ('=' or ':').
            delim = '=' if '=' in sline else (':' if ':' in sline else None)
            if delim:
                parts = sline.split(delim, 1)
                k = parts[0].strip().lower()
                v = parts[1].strip()
                # Check for include directive.
                if k == "include":
                    inc_path = v
                    if not os.path.isabs(inc_path):
                        inc_path = os.path.join(base_dir, inc_path)
                    includes_to_load.append(inc_path)
                else:
                    if current_section not in local_sections:
                        local_sections[current_section] = {}
                    local_sections[current_section][k] = v

    # Merged result map.
    merged = {}
    # Process includes first so parent configurations establish baseline values.
    for inc in includes_to_load:
        parent_map = parse_nested_ini(inc, visited)
        for sec, kvs in parent_map.items():
            if sec not in merged:
                merged[sec] = {}
            merged[sec].update(kvs)

    # Apply local section definitions, overwriting any matching parent keys!
    for sec, kvs in local_sections.items():
        if sec not in merged:
            merged[sec] = {}
        merged[sec].update(kvs)

    # Return final resolved configuration map.
    return merged

def main():
    # Instantiate argument parser.
    parser = argparse.ArgumentParser(description="Read nested INI config and compile/run DynamoRIO O3 simulator.")
    # Add configuration file argument.
    parser.add_argument("--config", "-c", default="configs/o3_default.ini", help="Path to INI config file")
    # Add target argument (sim, test, all).
    parser.add_argument("--target", default="sim", choices=["sim", "test", "all"], help="Build target")
    # Add parallel build jobs count.
    parser.add_argument("-j", "--jobs", default=str(os.cpu_count() or 4), help="Number of parallel make jobs")
    # Add optional trace path to run immediately after building.
    parser.add_argument("--trace", help="Optional trace file to run immediately after build")
    # Add max instructions limit.
    parser.add_argument("--max_instrs", default="1000000", help="Max instructions to simulate (default: 1M)")
    # Parse arguments.
    args = parser.parse_args()

    # Parse nested INI configuration.
    print(f"Loading and resolving nested INI configuration: {args.config}")
    cfg = parse_nested_ini(args.config)

    # Print summary of key microarchitectural parameters.
    print("\n=======================================================")
    print("       ACTIVE MICROARCHITECTURE CONFIGURATION")
    print("=======================================================")
    for sec, kvs in sorted(cfg.items()):
        print(f"[{sec}]")
        for k, v in sorted(kvs.items()):
            print(f"  {k} = {v}")
    print("=======================================================\n")

    # Construct compile command.
    build_cmd = ["make", f"-j{args.jobs}", args.target]
    print(f"Executing compilation: {' '.join(build_cmd)}")
    res = subprocess.run(build_cmd)
    if res.returncode != 0:
        print("Compilation failed!")
        sys.exit(res.returncode)
    print("Compilation succeeded!\n")

    # If trace file specified, execute simulator with the config.
    if args.trace:
        sim_cmd = [
            "./bin/run_cpu_sim",
            "--config", args.config,
            "--trace", args.trace,
            "--max_instrs", args.max_instrs
        ]
        print(f"Launching simulation: {' '.join(sim_cmd)}")
        sim_res = subprocess.run(sim_cmd)
        sys.exit(sim_res.returncode)

if __name__ == "__main__":
    main()
