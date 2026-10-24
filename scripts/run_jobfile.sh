#!/usr/bin/env bash
# ==============================================================================
# run_jobfile.sh: Parallel Job Dispatcher for DynamoRIO Benchmark Experiments
#
# Reads a clean jobfile containing commands (default: jobs/jobfile.txt) and executes
# N jobs at a time concurrently using native bash process pooling.
# Generates per-job console logs and aggregates CSV simulation results into a summary.
# ==============================================================================

# Enable strict bash error handling.
set -eo pipefail

# Determine canonical script directory resolving any symlinks.
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
# Determine repository root directory.
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# Set default jobfile path containing clean commands.
JOBFILE="${REPO_ROOT}/jobs/jobfile.txt"
# Set default results directory.
OUT_DIR="${REPO_ROOT}/results"
# Default concurrency: number of jobs to run at a time (default: 1).
CONCURRENCY=1
# Flag for dry run.
DRY_RUN=0
# Flag for stats-only report.
STATS_ONLY=0
# Optional list or range of job line numbers to run (empty means all).
TARGET_LINES=""

# Define usage information function.
usage() {
    # Print usage header.
    echo "Usage: $0 [options]"
    # Explain jobfile option.
    echo "  --file <path>       Path to jobfile command list (default: jobs/jobfile.txt)"
    # Explain concurrency option.
    echo "  -n, -j, --parallel  Number of jobs to run concurrently at a time (default: 1)"
    # Explain lines filter option.
    echo "  --lines, --jobs     Comma-separated line numbers or ranges (e.g. 1,3,5-8,14)"
    # Explain outdir option.
    echo "  --outdir <dir>      Output directory for logs and stats (default: results)"
    # Explain dry-run option.
    echo "  --dry-run           Print commands that would be executed without running"
    # Explain stats-only option.
    echo "  --stats-only        Skip execution and print summary table of existing stats"
    # Explain help option.
    echo "  --help|-h           Show this help message"
    # Exit with code 0.
    exit 0
}

# Parse command line options.
while [[ $# -gt 0 ]]; do
    # Match argument flag.
    case "$1" in
        # Handle jobfile path option.
        --file)
            # Store jobfile path.
            JOBFILE="$2"
            # Shift past key and value.
            shift 2
            ;;
        # Handle concurrency option (-n, -j, --parallel).
        -n|-j|--parallel)
            # Store concurrency limit.
            CONCURRENCY="$2"
            # Shift past key and value.
            shift 2
            ;;
        # Handle line/job selection option.
        --lines|--jobs)
            # Store target lines string.
            TARGET_LINES="$2"
            # Shift past key and value.
            shift 2
            ;;
        # Handle output directory option.
        --outdir)
            # Store output directory path.
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
        # Handle stats only option.
        --stats-only)
            # Enable stats only flag.
            STATS_ONLY=1
            # Shift past flag.
            shift
            ;;
        # Handle help option.
        --help|-h)
            # Call usage function.
            usage
            ;;
        # Handle unrecognized option.
        *)
            # Print error.
            echo "Unknown option: $1" >&2
            # Display usage.
            usage
            ;;
    esac
done

# Ensure results directory exists.
mkdir -p "$OUT_DIR"

# Ensure dataset for HashJoin exists if HashJoin jobs are run.
if [[ -d "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master" && ! -f "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/customer.tbl" ]]; then
    # Inform user.
    echo "Generating missing TPC-H tables in apps/hashjoin/tpch-dbgen-master..."
    # Build tables.
    (cd "${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master" && make && ./dbgen -s 0.01)
fi

# Function to test whether a given line number is in the requested range.
line_is_selected() {
    # Extract candidate line index.
    local l_idx="$1"
    # If no filter specified, all lines are selected.
    if [[ -z "$TARGET_LINES" ]]; then
        # Return true.
        return 0
    fi
    # Split TARGET_LINES by commas.
    IFS=',' read -ra PARTS <<< "$TARGET_LINES"
    # Iterate over individual parts.
    for part in "${PARTS[@]}"; do
        # Check if part is a range (e.g. 5-8).
        if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then
            # Extract start of range.
            local r_start="${BASH_REMATCH[1]}"
            # Extract end of range.
            local r_end="${BASH_REMATCH[2]}"
            # Check if line index falls within range.
            if (( l_idx >= r_start && l_idx <= r_end )); then
                # Return true.
                return 0
            fi
        # Check if part is a single number.
        elif [[ "$part" =~ ^[0-9]+$ ]]; then
            # Check for exact match.
            if (( l_idx == part )); then
                # Return true.
                return 0
            fi
        fi
    done
    # Line not selected.
    return 1
}

# Function to parse and display comparative simulation statistics.
print_summary_table() {
    # Print table banner.
    echo "=================================================================================================="
    echo "                            SIMULATION EXPERIMENT RESULTS SUMMARY TABLE                           "
    echo "=================================================================================================="
    # Print column headers.
    printf "%-30s | %10s | %10s | %6s | %10s | %10s | %9s\n" \
        "Experiment" "Retired" "Cycles" "IPC" "L1D Miss%" "L2 Miss%" "PTW Walks"
    echo "-------------------------------+------------+------------+--------+------------+------------+----------"

    # Search for all generated stats_*.log files in OUT_DIR.
    for sfile in "${OUT_DIR}"/stats_*.log; do
        # Skip if glob didn't match any file.
        [[ -f "$sfile" ]] || continue

        # Extract base file name.
        local fname
        # Get basename without directory.
        fname="$(basename "$sfile")"

        # Format experiment name (strip stats_ prefix and .log suffix).
        local exp_name
        # Clean experiment name string.
        exp_name="$(echo "$fname" | sed -E 's/^stats_//; s/\.log$//')"

        # Extract scalar metrics using awk.
        local retired
        # Read retired instructions.
        retired="$(awk -F', *' '$1=="instructions_retired" {print $2}' "$sfile" 2>/dev/null || echo "N/A")"
        local cycles
        # Read cycles.
        cycles="$(awk -F', *' '$1=="cycles" {print $2}' "$sfile" 2>/dev/null || echo "N/A")"
        local ipc
        # Read IPC.
        ipc="$(awk -F', *' '$1=="ipc" {print $2}' "$sfile" 2>/dev/null || echo "N/A")"
        local l1d_miss
        # Read L1D miss rate.
        l1d_miss="$(awk -F', *' '$1=="L1D miss rate" {print $2}' "$sfile" 2>/dev/null || echo "N/A")"
        local l2_miss
        # Read L2 miss rate.
        l2_miss="$(awk -F', *' '$1=="L2 total miss rate" {print $2}' "$sfile" 2>/dev/null || echo "N/A")"
        local ptw
        # Read PTW walks.
        ptw="$(awk -F', *' '$1=="PTW walks" {print $2}' "$sfile" 2>/dev/null || echo "N/A")"

        # Format and display row.
        printf "%-30s | %10s | %10s | %6s | %10s | %10s | %9s\n" \
            "$exp_name" "${retired:-N/A}" "${cycles:-N/A}" "${ipc:-N/A}" \
            "${l1d_miss:-N/A}" "${l2_miss:-N/A}" "${ptw:-N/A}"
    done
    echo "=================================================================================================="
    echo "Detailed logs and CSV metrics located in: ${OUT_DIR}/"
}

# If stats-only requested, print table and exit immediately.
if [[ $STATS_ONLY -eq 1 ]]; then
    # Call summary function.
    print_summary_table
    # Exit cleanly.
    exit 0
fi

# Validate jobfile existence.
if [[ ! -f "$JOBFILE" ]]; then
    # Print error.
    echo "Error: Jobfile not found at: $JOBFILE" >&2
    # Exit with code 1.
    exit 1
fi

# Function to execute a single job in the background and log its timing and output.
execute_job() {
    # Extract line number.
    local line_num="$1"
    # Extract command string.
    local cmd="$2"
    # Derive console log filename.
    local console_log="${OUT_DIR}/console_line$(printf "%02d" "$line_num").log"
    # Extract benchmark name from command for display.
    local app_name
    # Parse application binary name from after '--'.
    app_name="$(echo "$cmd" | sed -E 's/.*--[[:space:]]+(\.\/)?apps\///' | awk '{print $1}')"

    # Print start notification.
    echo "[START] Job #${line_num} (${app_name}) running concurrently..."

    # Record start time.
    local t_start
    # Capture start epoch.
    t_start="$(date +%s)"

    # Execute command, redirecting both stdout and stderr to the dedicated console log.
    eval "$cmd" > "$console_log" 2>&1
    # Capture exit code.
    local ret=$?

    # Record end time.
    local t_end
    # Capture end epoch.
    t_end="$(date +%s)"
    # Compute elapsed seconds.
    local dur=$(( t_end - t_start ))

    # Check return code.
    if [[ $ret -eq 0 ]]; then
        # Print completion message.
        echo "[DONE]  Job #${line_num} (${app_name}) completed successfully in ${dur}s. (Log: ${console_log})"
    else
        # Print failure warning.
        echo "[FAIL]  Job #${line_num} (${app_name}) failed with exit code ${ret} after ${dur}s. (Log: ${console_log})" >&2
    fi
}

# Print execution banner.
echo "================================================================================"
echo " Starting Batch Job Execution from: ${JOBFILE}"
echo " Concurrency: Running up to ${CONCURRENCY} job(s) at a time"
if [[ -n "$TARGET_LINES" ]]; then
    echo " Filter: Running only jobs at line(s): ${TARGET_LINES}"
fi
if [[ $DRY_RUN -eq 1 ]]; then
    echo " Mode: DRY RUN (commands will be displayed but not executed)"
fi
echo "================================================================================"

# Initialize line counter.
LINE_NUM=0
# Initialize queued count.
TOTAL_QUEUED=0

# Read jobfile line by line.
while IFS= read -r line || [[ -n "$line" ]]; do
    # Trim whitespace.
    trimmed="$(echo "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"

    # Skip comments or empty lines.
    if [[ "$trimmed" =~ ^#.*$ || -z "$trimmed" ]]; then
        # Continue to next line.
        continue
    fi

    # Increment line number counter for every executable command.
    LINE_NUM=$(( LINE_NUM + 1 ))

    # Check if this line is selected by user filter.
    if ! line_is_selected "$LINE_NUM"; then
        # Skip line.
        continue
    fi

    # Increment queued count.
    TOTAL_QUEUED=$(( TOTAL_QUEUED + 1 ))

    # If dry run mode, print command and continue.
    if [[ $DRY_RUN -eq 1 ]]; then
        # Print job command.
        echo "[DRY-RUN] Job #${LINE_NUM}: ${trimmed}"
        # Move to next line.
        continue
    fi

    # Process pooling: if active background jobs have reached concurrency limit, wait for one to finish.
    while (( $(jobs -r -p | wc -l) >= CONCURRENCY )); do
        # Wait for any child job to finish (supported natively in bash 4.3+).
        wait -n 2>/dev/null || sleep 0.5
    done

    # Launch job in background with concurrency pooling.
    execute_job "$LINE_NUM" "$trimmed" &

done < "$JOBFILE"

# Wait for all remaining background jobs to finish.
wait

# Print completion banner.
echo "================================================================================"
echo " All ${TOTAL_QUEUED} queued job(s) finished!"
echo "================================================================================"
echo ""

# Print final comparative summary table across all generated statistics.
print_summary_table
