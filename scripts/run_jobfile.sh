#!/usr/bin/env bash
# ==============================================================================
# run_jobfile.sh: Jobfile Dispatcher for DynamoRIO Benchmark Experiments
#
# Reads a batch jobfile (default: jobs/jobfile.txt) and sequentially or selectively
# executes simulation experiments under the cycle-accurate OoO CPU & cache simulator.
# Generates per-job console logs and aggregates CSV simulation results into a summary.
# ==============================================================================

# Enable strict bash error handling.
set -eo pipefail

# Determine canonical script directory resolving any symlinks.
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
# Determine repository root directory.
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# Set default jobfile path.
JOBFILE="${REPO_ROOT}/jobs/jobfile.txt"
# Set default results directory.
OUT_DIR="${REPO_ROOT}/results"
# Flag for dry run.
DRY_RUN=0
# Flag for stats-only report.
STATS_ONLY=0
# Optional list or range of job numbers to run (empty means all).
TARGET_JOBS=""

# Define usage information function.
usage() {
    # Print usage header.
    echo "Usage: $0 [options]"
    # Explain jobfile option.
    echo "  --file <path>       Path to jobfile command list (default: jobs/jobfile.txt)"
    # Explain jobs filter option.
    echo "  --jobs <range>      Comma-separated job numbers or ranges (e.g. 1,3,5-8,14)"
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
        # Handle jobs selection option.
        --jobs)
            # Store target jobs string.
            TARGET_JOBS="$2"
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

# Function to test whether a given job number is in the requested range.
job_is_selected() {
    # Extract candidate job index.
    local j_idx="$1"
    # If no filter specified, all jobs are selected.
    if [[ -z "$TARGET_JOBS" ]]; then
        # Return true.
        return 0
    fi
    # Split TARGET_JOBS by commas.
    IFS=',' read -ra PARTS <<< "$TARGET_JOBS"
    # Iterate over individual parts.
    for part in "${PARTS[@]}"; do
        # Check if part is a range (e.g. 5-8).
        if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then
            # Extract start of range.
            local r_start="${BASH_REMATCH[1]}"
            # Extract end of range.
            local r_end="${BASH_REMATCH[2]}"
            # Check if job index falls within range.
            if (( j_idx >= r_start && j_idx <= r_end )); then
                # Return true.
                return 0
            fi
        # Check if part is a single number.
        elif [[ "$part" =~ ^[0-9]+$ ]]; then
            # Check for exact match.
            if (( j_idx == part )); then
                # Return true.
                return 0
            fi
        fi
    done
    # Job not selected.
    return 1
}

# Function to parse and display comparative simulation statistics.
print_summary_table() {
    # Print table banner.
    echo "=================================================================================================="
    echo "                            SIMULATION EXPERIMENT RESULTS SUMMARY TABLE                           "
    echo "=================================================================================================="
    # Print column headers.
    printf "%-8s | %-10s | %-8s | %10s | %10s | %6s | %10s | %10s | %9s\n" \
        "Job" "App" "Profile" "Retired" "Cycles" "IPC" "L1D Miss%" "L2 Miss%" "PTW Walks"
    echo "---------+------------+----------+------------+------------+--------+------------+------------+----------"

    # Search for all generated stats_job*.log files in OUT_DIR.
    for sfile in "${OUT_DIR}"/stats_job*.log; do
        # Skip if glob didn't match any file.
        [[ -f "$sfile" ]] || continue

        # Extract base file name.
        local fname
        # Get basename without directory.
        fname="$(basename "$sfile")"

        # Extract job number (e.g. job01).
        local jid
        # Parse job ID prefix.
        jid="$(echo "$fname" | grep -o -E 'job[0-9]+' || echo "N/A")"

        # Extract application name.
        local app
        # Match application name from filename pattern stats_jobXX_<app>_...log.
        app="$(echo "$fname" | sed -E 's/stats_job[0-9]+_([^_]+)_.*/\1/')"

        # Extract profile tag from filename.
        local profile
        # Determine profile from filename suffix.
        profile="$(echo "$fname" | sed -E 's/stats_job[0-9]+_[^_]+_([^\.]+)\.log/\1/')"

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
        printf "%-8s | %-10s | %-8s | %10s | %10s | %6s | %10s | %10s | %9s\n" \
            "$jid" "$app" "$profile" "${retired:-N/A}" "${cycles:-N/A}" "${ipc:-N/A}" \
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

# Track current job counter.
JOB_COUNTER=0
# Track total jobs executed.
JOBS_EXECUTED=0

# Print execution banner.
echo "================================================================================"
echo " Starting Batch Job Execution from: ${JOBFILE}"
if [[ -n "$TARGET_JOBS" ]]; then
    echo " Filter: Running only jobs in set: ${TARGET_JOBS}"
fi
if [[ $DRY_RUN -eq 1 ]]; then
    echo " Mode: DRY RUN (commands will be displayed but not executed)"
fi
echo "================================================================================"

# Read jobfile line by line.
while IFS= read -r line || [[ -n "$line" ]]; do
    # Trim leading and trailing whitespace.
    trimmed="$(echo "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"

    # Check if line is a comment or empty.
    if [[ "$trimmed" =~ ^#.*$ || -z "$trimmed" ]]; then
        # Check if comment line denotes a job header (e.g. # --- Job 03: ... ---).
        if [[ "$trimmed" =~ ^#[[:space:]]*---[[:space:]]*Job[[:space:]]+([0-9]+):[[:space:]]*(.*)[[:space:]]*--- ]]; then
            # Extract job number from comment.
            CURRENT_JOB_NUM="${BASH_REMATCH[1]}"
            # Extract job description.
            CURRENT_JOB_DESC="${BASH_REMATCH[2]}"
        fi
        # Skip comment or blank line.
        continue
    fi

    # Increment job counter for each executable command encountered.
    JOB_COUNTER=$(( JOB_COUNTER + 1 ))

    # Determine numeric index (use header number if available, else sequential counter).
    JOB_NUM="${CURRENT_JOB_NUM:-$JOB_COUNTER}"
    # Remove leading zeros for arithmetic comparison.
    JOB_NUM_INT=$(( 10#$JOB_NUM ))

    # Check if job is selected by user filter.
    if ! job_is_selected "$JOB_NUM_INT"; then
        # Skip non-selected job.
        continue
    fi

    # Increment executed count.
    JOBS_EXECUTED=$(( JOBS_EXECUTED + 1 ))

    # Determine console log filename corresponding to this job.
    CONSOLE_LOG="${OUT_DIR}/console_job$(printf "%02d" "$JOB_NUM_INT").log"

    # Print job header.
    echo "--------------------------------------------------------------------------------"
    echo " [JOB $(printf "%02d" "$JOB_NUM_INT")] ${CURRENT_JOB_DESC:-Command line run}"
    echo " [CMD] ${trimmed}"
    echo " [LOG] Console: ${CONSOLE_LOG}"
    echo "--------------------------------------------------------------------------------"

    # If dry run, print command and continue.
    if [[ $DRY_RUN -eq 1 ]]; then
        # Print dry-run notice.
        echo "[DRY-RUN] Command skipped."
        # Continue to next job.
        continue
    fi

    # Record start timestamp.
    start_ts="$(date +%s)"

    # Execute the simulation command from the jobfile line, piping output to console log.
    eval "${trimmed}" 2>&1 | tee "$CONSOLE_LOG"

    # Record end timestamp.
    end_ts="$(date +%s)"
    # Compute elapsed seconds.
    elapsed=$(( end_ts - start_ts ))

    # Print completion status.
    echo " [DONE] Job $(printf "%02d" "$JOB_NUM_INT") completed in ${elapsed}s."
    echo ""
done < "$JOBFILE"

# Print execution summary banner.
echo "================================================================================"
echo " Batch processing finished! Executed ${JOBS_EXECUTED} jobs."
echo "================================================================================"
echo ""

# Print final comparative summary table across all generated statistics.
print_summary_table
