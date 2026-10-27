#!/usr/bin/env bash
# ==============================================================================
# run_benchmarks.sh: Automated Benchmark Runner for DynamoRIO O3 CPU Simulator
#
# Runs modgarnet benchmark applications (BTree, GUPS, HashJoin, XSBench, hello)
# through DynamoRIO's cycle-accurate Out-of-Order CPU & cache simulation engine.
# Collects detailed CSV statistics, periodic heartbeats, and per-benchmark logs.
# ==============================================================================

# Enable strict bash error handling.
set -eo pipefail

# Determine canonical script directory resolving any symlinks.
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
# Determine repository root directory.
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# Set default simulator launcher path.
DRRUN="${REPO_ROOT}/build/bin64/drrun"
# Set default configuration file.
CONFIG_FILE="${REPO_ROOT}/configs/o3_server.ini"
# Set default output directory for results and logs.
OUT_DIR="${REPO_ROOT}/results"
# Set default benchmark application (default: all).
APP="all"
# Set default execution mode (smoke or large).
MODE="smoke"
# Set default heartbeat interval (10,000,000 instructions; 0 disables).
HEARTBEAT="10000000"
# Set default reference limit (empty means run to completion).
SIM_REFS=""
# Set default GUPS memory allocation in GB (0=256MB smoke; 1=1GB; can be set to 64, 128, etc.).
GUPS_MEM=""
# Flag for dry run.
DRY_RUN=0

# Define usage information function.
usage() {
    # Print usage banner.
    echo "Usage: $0 [options]"
    # Explain app option.
    echo "  --app <name>        Benchmark to run: hello, btree, gups, hashjoin, xsbench, all (default: all)"
    # Explain mode option.
    echo "  --mode <mode>       Execution mode: smoke (fast) or large (full footprint) (default: smoke)"
    # Explain config option.
    echo "  --config <file>     Path to CPU simulator INI config (default: configs/o3_server.ini)"
    # Explain heartbeat option.
    echo "  --heartbeat <N>     Progress heartbeat instruction interval (default: 10000000)"
    # Explain sim_refs option.
    echo "  --sim-refs <N>      Limit maximum simulated memory references (default: unlimited)"
    # Explain gups-mem option.
    echo "  --gups-mem <GB>     Memory size for GUPS benchmark in GB (e.g. 64 on large servers)"
    # Explain outdir option.
    echo "  --outdir <dir>      Directory where logs and stats are stored (default: results)"
    # Explain dry-run option.
    echo "  --dry-run           Print commands without executing"
    # Explain list option.
    echo "  --list              List available benchmarks and their commands"
    # Explain help option.
    echo "  --help              Display this help message"
    # Exit with success.
    exit 0
}

# Define list function showing benchmark commands.
list_benchmarks() {
    # Print header.
    echo "Available Benchmark Applications in DynamoRIO:"
    # List hello.
    echo "  1. hello:"
    # Command for hello.
    echo "     Smoke: ./apps/hello"
    # List BTree.
    echo "  2. btree (B+ Tree traversal benchmark):"
    # Smoke command for BTree.
    echo "     Smoke: ./apps/BTree 1000 100"
    # Large command for BTree.
    echo "     Large: ./apps/BTree 200000000 200000000 256"
    # List GUPS.
    echo "  3. gups (Giga Updates Per Second memory stressor):"
    # Smoke command for GUPS.
    echo "     Smoke: ./apps/gups 0  (256 MB table)"
    # Large command for GUPS.
    echo "     Large: ./apps/gups 1  (1 GB table)"
    # List HashJoin.
    echo "  4. hashjoin (TPC-H Query 10 hash join):"
    # Smoke command for HashJoin.
    echo "     Smoke: ./apps/hashjoin/query10 murmur3 apps/hashjoin/tpch-dbgen-master/customer.tbl 150 apps/hashjoin/tpch-dbgen-master/orders.tbl 1500 17 1"
    # Large command for HashJoin.
    echo "     Large: ./apps/hashjoin/query10 murmur3 apps/hashjoin/tpch-dbgen-master/customer.tbl 1500 apps/hashjoin/tpch-dbgen-master/orders.tbl 15000 16381 1"
    # List XSBench.
    echo "  5. xsbench (Monte Carlo neutron cross-section lookup):"
    # Smoke command for XSBench.
    echo "     Smoke: ./apps/XSBench -s small -g 100 -p 100 -l 100"
    # Large command for XSBench.
    echo "     Large: ./apps/XSBench -s large -g 1000 -p 1000 -l 10000"
    # Exit with success.
    exit 0
}

# Parse command line arguments.
while [[ $# -gt 0 ]]; do
    # Match argument flag.
    case "$1" in
        # Handle app option.
        --app)
            # Store app value in lowercase.
            APP="$(echo "$2" | tr '[:upper:]' '[:lower:]')"
            # Shift past key and value.
            shift 2
            ;;
        # Handle mode option.
        --mode)
            # Store mode value in lowercase.
            MODE="$(echo "$2" | tr '[:upper:]' '[:lower:]')"
            # Shift past key and value.
            shift 2
            ;;
        # Handle config file option.
        --config)
            # Store config path.
            CONFIG_FILE="$2"
            # Shift past key and value.
            shift 2
            ;;
        # Handle heartbeat option.
        --heartbeat)
            # Store heartbeat interval.
            HEARTBEAT="$2"
            # Shift past key and value.
            shift 2
            ;;
        # Handle sim_refs option.
        --sim-refs)
            # Store reference limit.
            SIM_REFS="$2"
            # Shift past key and value.
            shift 2
            ;;
        # Handle GUPS memory allocation option in GB.
        --gups-mem|--mem)
            # Store GUPS memory size in GB.
            GUPS_MEM="$2"
            # Shift past key and value.
            shift 2
            ;;
        # Handle outdir option.
        --outdir)
            # Store output directory.
            OUT_DIR="$2"
            # Shift past key and value.
            shift 2
            ;;
        # Handle dry run option.
        --dry-run)
            # Enable dry run flag.
            DRY_RUN=1
            # Shift past flag.
            shift
            ;;
        # Handle list option.
        --list)
            # Call list function.
            list_benchmarks
            ;;
        # Handle help option.
        --help|-h)
            # Call usage function.
            usage
            ;;
        # Handle unknown option.
        *)
            # Print error message.
            echo "Unknown option: $1" >&2
            # Call usage function.
            usage
            ;;
    esac
done

# Ensure DynamoRIO binary launcher exists.
if [[ ! -x "$DRRUN" && $DRY_RUN -eq 0 ]]; then
    # Print error if launcher not found.
    echo "Error: DynamoRIO launcher not found at $DRRUN" >&2
    # Suggest building DynamoRIO via build.sh drrun or cmake.
    echo "Please build DynamoRIO first via:" >&2
    # Suggest convenient helper command.
    echo "  ./build.sh drrun" >&2
    # Suggest manual cmake and ninja command.
    echo "  (or: mkdir -p build && cd build && cmake -GNinja .. && ninja)" >&2
    # Exit with failure.
    exit 1
fi

# Ensure output directory exists.
mkdir -p "$OUT_DIR"

# Define function to run an individual benchmark under DynamoRIO O3 engine.
run_single_benchmark() {
    # Extract benchmark name parameter.
    local name="$1"
    # Extract application executable and argument array.
    shift
    # Store remaining arguments as command array.
    local app_cmd=("$@")

    # Set log file paths for console output and CSV statistics.
    local stats_file="${OUT_DIR}/stats_${name}_${MODE}.log"
    # Set console log path.
    local console_log="${OUT_DIR}/console_${name}_${MODE}.log"

    # Assemble drrun simulator options.
    local dr_args=(
        # Use DynamoRIO memory trace tool.
        -t drmemtrace
        # Activate Out-of-Order CPU simulator.
        -simulator_type cpu
        # Pass microarchitecture INI configuration.
        -cpu_config "$CONFIG_FILE"
        # Pass progress heartbeat interval.
        -heartbeat_interval "$HEARTBEAT"
        # Write CSV simulation results to destination file.
        -stats_file "$stats_file"
    )

    # Append sim_refs option if explicitly requested.
    if [[ -n "$SIM_REFS" ]]; then
        # Add sim_refs argument.
        dr_args+=(-sim_refs "$SIM_REFS")
    fi

    # Print status header.
    echo "================================================================================"
    # Print current benchmark and mode.
    echo " [RUN] Benchmark: ${name} | Mode: ${MODE}"
    # Print simulator command.
    echo " [CMD] ${DRRUN} ${dr_args[*]} -- ${app_cmd[*]}"
    # Print output files.
    echo " [OUT] Stats: ${stats_file} | Console: ${console_log}"
    # Print separator.
    echo "================================================================================"

    # If dry run, do not execute.
    if [[ $DRY_RUN -eq 1 ]]; then
        # Inform user of dry run.
        echo "[DRY-RUN] Skipping actual execution."
        # Return cleanly.
        return 0
    fi

    # Record start timestamp.
    local start_time
    # Capture start epoch.
    start_time="$(date +%s)"
    # Capture start date and time formatted string.
    local start_time_str
    # Format current date and time (YYYY-MM-DD HH:MM:SS).
    start_time_str="$(date +"%Y-%m-%d %H:%M:%S")"

    # Execute simulator with process redirection to console log while displaying to terminal.
    "${DRRUN}" "${dr_args[@]}" -- "${app_cmd[@]}" 2>&1 | tee "$console_log"

    # Record end timestamp.
    local end_time
    # Capture end epoch.
    end_time="$(date +%s)"
    # Capture completion date and time formatted string.
    local completion_time_str
    # Format completion timestamp (YYYY-MM-DD HH:MM:SS).
    completion_time_str="$(date +"%Y-%m-%d %H:%M:%S")"

    # Compute wall-clock elapsed time in seconds.
    local elapsed=$(( end_time - start_time ))

    # Format elapsed duration strictly as hr:m:s (HH:MM:SS).
    local dur_str
    # Calculate hours, minutes, and seconds formatted with two digits each.
    dur_str="$(printf "%02d:%02d:%02d" $(( elapsed / 3600 )) $(( (elapsed % 3600) / 60 )) $(( elapsed % 60 )))"

    # Append completion metrics to the stats file if it exists.
    if [[ -f "$stats_file" ]]; then
        # Append completion timestamp.
        echo "completion_time, ${completion_time_str}" >> "$stats_file"
        # Append duration formatted as hr:m:s.
        echo "duration, ${dur_str}" >> "$stats_file"
    fi

    # Append complete timing summary block to the console log.
    {
        # Write blank line.
        echo ""
        # Write separator.
        echo "================================================================================"
        # Write benchmark execution summary header.
        echo " Execution Summary for ${name} (${MODE})"
        # Write start timestamp.
        echo " Started At:       ${start_time_str}"
        # Write completion timestamp.
        echo " Completed At:     ${completion_time_str}"
        # Write duration strictly as hr:m:s.
        echo " Duration:         ${dur_str}"
        # Write separator.
        echo "================================================================================"
    } >> "$console_log"

    # Print completion status with timestamp.
    echo "--------------------------------------------------------------------------------"
    # Print completion timestamp and duration strictly as hr:m:s.
    echo " [DONE] Completed ${name} (${MODE}) at ${completion_time_str} (Duration: ${dur_str})."
    # Print separator.
    echo "--------------------------------------------------------------------------------"
    # Print newline.
    echo ""
}

# Determine list of benchmarks to execute based on --app argument.
case "$APP" in
    # Handle hello.
    hello)
        # Run hello.
        run_single_benchmark "hello" "${REPO_ROOT}/apps/hello"
        ;;
    # Handle btree.
    btree)
        # Select command based on mode.
        if [[ "$MODE" == "smoke" ]]; then
            # Run smoke test for BTree (1,000 elements, 100 lookups).
            run_single_benchmark "btree" "${REPO_ROOT}/apps/BTree" "1000" "100"
        else
            # Run large test for BTree (200,000,000 elements, 200,000,000 lookups, 256B payload).
            run_single_benchmark "btree" "${REPO_ROOT}/apps/BTree" "200000000" "200000000" "256"
        fi
        ;;
    # Handle gups.
    gups)
        # Determine memory allocation parameter in GB.
        g_mem=""
        # Check if explicitly provided via --gups-mem.
        if [[ -n "$GUPS_MEM" ]]; then
            # Use explicitly specified memory size.
            g_mem="$GUPS_MEM"
        elif [[ "$MODE" == "smoke" ]]; then
            # Smoke mode defaults to 0 (256 MB table).
            g_mem="0"
        else
            # Large mode defaults to 64 GB (or 1 GB if not specified).
            g_mem="64"
        fi
        # Run GUPS benchmark with configured table size.
        run_single_benchmark "gups" "${REPO_ROOT}/apps/gups" "$g_mem"
        ;;
    # Handle hashjoin.
    hashjoin)
        # Check if TPC-H datasets exist.
        if [[ ! -f "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/customer.tbl" ]]; then
            # Generate datasets if missing.
            echo "Generating TPC-H tables in apps/hashjoin/tpch-dbgen-master..."
            # Change directory to dbgen.
            (cd "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master" && make && ./dbgen -s 0.01)
        fi
        # Select command based on mode.
        if [[ "$MODE" == "smoke" ]]; then
            # Run smoke test for HashJoin.
            run_single_benchmark "hashjoin" "${REPO_ROOT}/apps/hashjoin/query10" "murmur3" \
                "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/customer.tbl" "150" \
                "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/orders.tbl" "1500" "17" "1"
        else
            # Run large test for HashJoin.
            run_single_benchmark "hashjoin" "${REPO_ROOT}/apps/hashjoin/query10" "murmur3" \
                "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/customer.tbl" "1500" \
                "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/orders.tbl" "15000" "16381" "1"
        fi
        ;;
    # Handle xsbench.
    xsbench)
        # Select command based on mode.
        if [[ "$MODE" == "smoke" ]]; then
            # Run smoke test for XSBench (small grid, 100 particles, 100 lookups).
            run_single_benchmark "xsbench" "${REPO_ROOT}/apps/XSBench" "-s" "small" "-g" "100" "-p" "100" "-l" "100"
        else
            # Run large test for XSBench (large grid, 1,000 particles, 10,000 lookups).
            run_single_benchmark "xsbench" "${REPO_ROOT}/apps/XSBench" "-s" "large" "-g" "1000" "-p" "1000" "-l" "10000"
        fi
        ;;
    # Handle all benchmarks.
    all)
        # 1. Run hello.
        run_single_benchmark "hello" "${REPO_ROOT}/apps/hello"

        # 2. Run BTree.
        if [[ "$MODE" == "smoke" ]]; then
            # Smoke test for BTree.
            run_single_benchmark "btree" "${REPO_ROOT}/apps/BTree" "1000" "100"
        else
            # Large test for BTree (200,000,000 elements, 200,000,000 lookups, 256B payload).
            run_single_benchmark "btree" "${REPO_ROOT}/apps/BTree" "200000000" "200000000" "256"
        fi

        # 3. Run GUPS.
        # Determine memory allocation parameter in GB for GUPS.
        g_mem=""
        # Check if explicitly provided via --gups-mem.
        if [[ -n "$GUPS_MEM" ]]; then
            # Use explicitly specified memory size.
            g_mem="$GUPS_MEM"
        elif [[ "$MODE" == "smoke" ]]; then
            # Smoke mode defaults to 0 (256 MB table).
            g_mem="0"
        else
            # Large mode defaults to 64 GB table.
            g_mem="64"
        fi
        # Run GUPS with determined table size.
        run_single_benchmark "gups" "${REPO_ROOT}/apps/gups" "$g_mem"

        # 4. Run HashJoin.
        if [[ ! -f "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/customer.tbl" ]]; then
            # Generate datasets if missing.
            (cd "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master" && make && ./dbgen -s 0.01)
        fi
        if [[ "$MODE" == "smoke" ]]; then
            # Smoke test for HashJoin.
            run_single_benchmark "hashjoin" "${REPO_ROOT}/apps/hashjoin/query10" "murmur3" \
                "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/customer.tbl" "150" \
                "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/orders.tbl" "1500" "17" "1"
        else
            # Large test for HashJoin.
            run_single_benchmark "hashjoin" "${REPO_ROOT}/apps/hashjoin/query10" "murmur3" \
                "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/customer.tbl" "1500" \
                "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/orders.tbl" "15000" "16381" "1"
        fi

        # 5. Run XSBench.
        if [[ "$MODE" == "smoke" ]]; then
            # Smoke test for XSBench.
            run_single_benchmark "xsbench" "${REPO_ROOT}/apps/XSBench" "-s" "small" "-g" "100" "-p" "100" "-l" "100"
        else
            # Large test for XSBench.
            run_single_benchmark "xsbench" "${REPO_ROOT}/apps/XSBench" "-s" "large" "-g" "1000" "-p" "1000" "-l" "10000"
        fi
        ;;
    # Handle invalid benchmark name.
    *)
        # Print error.
        echo "Error: Unknown benchmark application '$APP'." >&2
        # Suggest valid choices.
        echo "Valid choices: hello, btree, gups, hashjoin, xsbench, all" >&2
        # Exit with failure.
        exit 1
        ;;
esac

# Summary table generation: parse all CSV stats files created in this run.
echo "================================================================================"
echo "                  SIMULATION RESULTS COMPARISON SUMMARY TABLE                  "
echo "================================================================================"
# Print table header.
printf "%-10s | %-6s | %10s | %10s | %6s | %10s | %10s | %9s\n" \
    "Benchmark" "Mode" "Inst Retired" "Cycles" "IPC" "L1D Miss%" "L2 Miss%" "PTW Walks"
echo "-----------+--------+--------------+------------+--------+------------+------------+----------"

# Iterate over matching stats log files.
for sfile in "${OUT_DIR}"/stats_*_${MODE}.log; do
    # Skip if file does not exist.
    [[ -f "$sfile" ]] || continue

    # Extract benchmark name from filename.
    bname="$(basename "$sfile" | sed -E "s/stats_([^_]+)_${MODE}\.log/\1/")"

    # Extract scalar metrics from CSV log using awk.
    retired="$(awk -F', *' '$1=="instructions_retired" {print $2}' "$sfile")"
    cycles="$(awk -F', *' '$1=="cycles" {print $2}' "$sfile")"
    ipc="$(awk -F', *' '$1=="ipc" {print $2}' "$sfile")"
    l1d_miss_rate="$(awk -F', *' '$1=="L1D miss rate" {print $2}' "$sfile")"
    l2_miss_rate="$(awk -F', *' '$1=="L2 total miss rate" {print $2}' "$sfile")"
    ptw_walks="$(awk -F', *' '$1=="PTW walks" {print $2}' "$sfile")"

    # Format values for table display.
    printf "%-10s | %-6s | %10s | %10s | %6s | %10s | %10s | %9s\n" \
        "$bname" "$MODE" "${retired:-N/A}" "${cycles:-N/A}" "${ipc:-N/A}" \
        "${l1d_miss_rate:-N/A}" "${l2_miss_rate:-N/A}" "${ptw_walks:-N/A}"
done
echo "================================================================================"
echo "Detailed CSV logs saved in: ${OUT_DIR}/"
