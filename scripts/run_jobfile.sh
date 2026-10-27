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

# Capture current git commit ID for reproducibility logging across all jobs.
GIT_COMMIT="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo "unknown")"
# Export GIT_COMMIT_ID to environment for simulator process detection.
export GIT_COMMIT_ID="$GIT_COMMIT"

# Generate a globally unique execution identifier combining high-resolution epoch, PID, and randomness.
RUN_ID="$(date +%s%N 2>/dev/null || date +%s)_pid${$}_rnd${RANDOM}"
# Register cleanup trap to remove named pipes created by this specific run upon exit.
trap 'rm -f /tmp/drcachesimpipe_run'"${RUN_ID}"'_* 2>/dev/null || true' EXIT INT TERM

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

# Safely clean up orphaned pipes from terminated runs without affecting concurrently active runs.
clean_stale_pipes() {
    # Scan existing named pipes in /tmp matching our naming convention.
    for ppath in /tmp/drcachesimpipe_*; do
        # Guard if glob did not match any file.
        [[ -e "$ppath" ]] || continue
        # If the pipe contains a PID tag, check if the owning process is still alive.
        if [[ "$ppath" =~ _pid([0-9]+)_ ]]; then
            local owner_pid="${BASH_REMATCH[1]}"
            # If the process is still running, leave its pipe intact!
            if kill -0 "$owner_pid" 2>/dev/null; then
                continue
            fi
        fi
        # Remove orphaned pipe from terminated process.
        rm -f "$ppath" 2>/dev/null || true
    done
}
# Clean orphaned pipes.
clean_stale_pipes

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

# Function to ensure the large TPC-H dataset exists once, skipping if already present.
ensure_hashjoin_datasets() {
    # Flag to track whether any selected job executes HashJoin.
    local need_hashjoin=0
    # Line counter for jobfile traversal.
    local cur_line=0

    # Scan the jobfile line by line.
    while IFS= read -r jline || [[ -n "$jline" ]]; do
        # Trim whitespace from candidate command.
        local tline
        # Remove leading and trailing spaces.
        tline="$(echo "$jline" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"

        # Skip comment lines or empty lines.
        if [[ "$tline" =~ ^#.*$ || -z "$tline" ]]; then
            # Move to next line.
            continue
        fi

        # Increment line index counter.
        cur_line=$(( cur_line + 1 ))

        # Check if this line is in the selected execution set.
        if ! line_is_selected "$cur_line"; then
            # Skip unselected line.
            continue
        fi

        # Check if command line executes query10 HashJoin.
        if [[ "$tline" =~ query10 ]]; then
            # Flag that HashJoin is required.
            need_hashjoin=1
            # Break early once requirement confirmed.
            break
        fi
    done < "$JOBFILE"

    # If no selected jobs invoke HashJoin, no dataset generation is required.
    if (( need_hashjoin == 0 )); then
        # Return success immediately.
        return 0
    fi

    # Define customer table file path.
    local cust_tbl="${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/customer.tbl"
    # Define orders table file path.
    local orders_tbl="${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master/orders.tbl"

    # Check if the large dataset already exists on disk.
    if [[ -f "$cust_tbl" && -f "$orders_tbl" ]]; then
        # Query file size in bytes using stat without reading file contents.
        local orders_size
        # Get size using Linux stat or BSD stat fallback.
        orders_size=$(stat -c%s "$orders_tbl" 2>/dev/null || stat -f%z "$orders_tbl" 2>/dev/null || echo 0)
        # SF=50 orders.tbl is ~8.3 GB (8.85B bytes); check for at least 1 GB threshold (1,073,741,824 bytes).
        if (( orders_size >= 1073741824 )); then
            # Large dataset already exists; do not create a new one.
            return 0
        fi
    fi

    # Large dataset missing or incomplete: generate large dataset (SF=50, ~10 GB) once.
    echo "================================================================================"
    # Print status notification banner.
    echo " [TPC-H] HashJoin large dataset not found. Generating SF=50 (~10 GB) once..."
    # Print banner footer.
    echo "================================================================================"
    # Execute standalone dataset creation script to generate SF=50 tables.
    "${REPO_ROOT}/scripts/create_hashjoin_dataset.sh"
}

# Function to parse and display comparative simulation statistics.
print_summary_table() {
    # Print table banner.
    echo "========================================================================================================================================"
    echo "                                              SIMULATION EXPERIMENT RESULTS SUMMARY TABLE                                               "
    echo "========================================================================================================================================"
    # Print column headers including completion timestamp and duration.
    printf "%-26s | %19s | %10s | %10s | %10s | %6s | %10s | %10s | %9s\n" \
        "Experiment" "Completed At" "Duration" "Retired" "Cycles" "IPC" "L1D Miss%" "L2 Miss%" "PTW Walks"
    echo "---------------------------+---------------------+------------+------------+------------+--------+------------+------------+----------"

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

        # Extract completion timestamp from stats file if present.
        local comp_time
        # Read completion_time entry.
        comp_time="$(awk -F', *' '$1=="completion_time" {print $2}' "$sfile" 2>/dev/null || echo "N/A")"
        # Extract formatted duration from stats file if present.
        local comp_dur
        # Read duration entry in hr:m:s format (or duration_formatted fallback).
        comp_dur="$(awk -F', *' '$1=="duration" || $1=="duration_formatted" {print $2}' "$sfile" 2>/dev/null || echo "N/A")"

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

        # Format and display row including completion timestamp and duration.
        printf "%-26s | %19s | %10s | %10s | %10s | %6s | %10s | %10s | %9s\n" \
            "$exp_name" "${comp_time:-N/A}" "${comp_dur:-N/A}" "${retired:-N/A}" "${cycles:-N/A}" "${ipc:-N/A}" \
            "${l1d_miss:-N/A}" "${l2_miss:-N/A}" "${ptw:-N/A}"
    done
    echo "========================================================================================================================================"
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

# Ensure required TPC-H datasets are present before launching jobs (skipped in dry-run).
if [[ $DRY_RUN -ne 1 ]]; then
    # Validate and generate matching dataset scale if any HashJoin job is selected.
    ensure_hashjoin_datasets
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

    # Capture start date and time formatted string.
    local start_time_str
    # Format current date and time (YYYY-MM-DD HH:MM:SS).
    start_time_str="$(date +"%Y-%m-%d %H:%M:%S")"

    # Print start notification with timestamp.
    echo "[START] [${start_time_str}] Job #${line_num} (${app_name}) running concurrently..."

    # Rewrite stats_file argument in cmd to route directly into OUT_DIR if present.
    local target_stats_file=""
    if [[ "$cmd" =~ -stats_file[[:space:]]+([^[:space:]]+) ]]; then
        # Capture original stats file path.
        local orig_sfile="${BASH_REMATCH[1]}"
        # Extract basename of stats file.
        local sfile_base
        sfile_base="$(basename "$orig_sfile")"
        # Construct destination path inside OUT_DIR.
        target_stats_file="${OUT_DIR}/${sfile_base}"
        # Ensure target directory exists on disk.
        mkdir -p "$(dirname "$target_stats_file")"
        # Replace original path with target path in command string.
        cmd="$(echo "$cmd" | sed -E "s|-stats_file[[:space:]]+[^[:space:]]+|-stats_file ${target_stats_file}|")"
    fi

    # Assign a globally unique IPC pipe name for this job across different script runs and concurrent jobs.
    local unique_ipc="drcachesimpipe_run${RUN_ID}_job$(printf "%02d" "${line_num}")_sub${BASHPID:-$$}_${RANDOM}"
    # Remove any stale pipe for this job.
    rm -f "/tmp/${unique_ipc}"*
    # Inject or update -ipc_name in command to isolate named pipes across runs.
    if [[ "$cmd" =~ -ipc_name[[:space:]]+[^[:space:]]+ ]]; then
        # Replace existing ipc_name with unique pipe name.
        cmd="$(echo "$cmd" | sed -E "s|-ipc_name[[:space:]]+[^[:space:]]+|-ipc_name ${unique_ipc}|")"
    else
        # Inject -ipc_name right after 'drmemtrace '.
        cmd="$(echo "$cmd" | sed -E "s|drmemtrace |drmemtrace -ipc_name ${unique_ipc} |")"
    fi

    # Record start time in epoch seconds.
    local t_start
    # Capture start epoch.
    t_start="$(date +%s)"

    # Execute command, redirecting both stdout and stderr to the dedicated console log.
    eval "$cmd" > "$console_log" 2>&1
    # Capture exit code.
    local ret=$?

    # Clean up unique IPC pipe after process finishes.
    rm -f "/tmp/${unique_ipc}"*

    # Record end time in epoch seconds.
    local t_end
    # Capture end epoch.
    t_end="$(date +%s)"
    # Capture completion date and time formatted string.
    local completion_time_str
    # Format completion timestamp (YYYY-MM-DD HH:MM:SS).
    completion_time_str="$(date +"%Y-%m-%d %H:%M:%S")"

    # Compute elapsed seconds.
    local dur=$(( t_end - t_start ))

    # Format elapsed duration strictly as hr:m:s (HH:MM:SS).
    local dur_str
    # Calculate hours, minutes, and seconds formatted with two digits each.
    dur_str="$(printf "%02d:%02d:%02d" $(( dur / 3600 )) $(( (dur % 3600) / 60 )) $(( dur % 60 )))"

    # Append complete timing summary block to the job's console log.
    {
        # Write blank line.
        echo ""
        # Write header line.
        echo "================================================================================"
        # Write job line number and application.
        echo " Execution Status Summary for Job #${line_num} (${app_name})"
        # Write start timestamp.
        echo " Started At:        ${start_time_str}"
        # Write completion timestamp.
        echo " Completed At:      ${completion_time_str}"
        # Write duration strictly as hr:m:s.
        echo " Duration:          ${dur_str}"
        # Write process exit code.
        echo " Exit Code:         ${ret}"
        # Write footer line.
        echo "================================================================================"
    } >> "$console_log"

    # Check if target stats file exists on disk.
    if [[ -n "$target_stats_file" && -f "$target_stats_file" ]]; then
        # Append completion timestamp to the CSV statistics file.
        echo "completion_time, ${completion_time_str}" >> "$target_stats_file"
        # Append duration formatted as hr:m:s to the CSV statistics file.
        echo "duration, ${dur_str}" >> "$target_stats_file"
    fi

    # Check return code.
    if [[ $ret -eq 0 ]]; then
        # Print completion message displaying duration strictly as hr:m:s.
        echo "[DONE]  [${completion_time_str}] Job #${line_num} (${app_name}) completed successfully in ${dur_str}. (Log: ${console_log})"
    else
        # Print failure warning displaying duration strictly as hr:m:s.
        echo "[FAIL]  [${completion_time_str}] Job #${line_num} (${app_name}) failed with exit code ${ret} after ${dur_str}. (Log: ${console_log})" >&2
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
