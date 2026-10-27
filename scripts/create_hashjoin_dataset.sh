#!/usr/bin/env bash
# ==============================================================================
# create_hashjoin_dataset.sh: Standalone TPC-H Dataset Generator for HashJoin
#
# Generates the large TPC-H dataset (~10 GB input, Scale Factor 50: 7.5M customer
# rows and 75M order rows) required for server-scale HashJoin benchmarking.
# If the dataset already exists and is complete, generation is safely skipped
# unless --force is specified.
# ==============================================================================

# Enable strict error handling.
set -euo pipefail

# Determine canonical directory containing this script.
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
# Determine repository root directory.
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# Target directory containing the official TPC-H dbgen generator.
DBGEN_DIR="${REPO_ROOT}/apps/hashjoin/tpch-dbgen-master"

# Target paths for generated table files.
CUST_TBL="${DBGEN_DIR}/customer.tbl"
ORDERS_TBL="${DBGEN_DIR}/orders.tbl"

# Default configuration settings.
SCALE=50         # Scale factor 50 generates ~10 GB raw tables (7.5M customers, 75M orders).
FORCE=0          # Overwrite existing dataset if set to 1.
CHECK_ONLY=0     # Inspect dataset status without modifying if set to 1.
CLEAN_ONLY=0     # Remove generated dataset files if set to 1.

# Define usage information function.
usage() {
    # Print usage banner.
    echo "Usage: $0 [options]"
    # Explain scale option.
    echo "  --scale <SF>     TPC-H Scale Factor (default: 50 -> ~10 GB input, 75M orders)"
    # Explain force option.
    echo "  --force, -f      Force regeneration even if dataset already exists"
    # Explain check option.
    echo "  --check          Check dataset presence and size without generating"
    # Explain clean option.
    echo "  --clean          Remove existing .tbl dataset files"
    # Explain help option.
    echo "  --help, -h       Show this help message"
    # Exit with code 0.
    exit 0
}

# Parse command line options.
while [[ $# -gt 0 ]]; do
    # Match argument flag.
    case "$1" in
        # Scale factor argument.
        --scale)
            # Store scale value.
            SCALE="$2"
            # Shift past flag and value.
            shift 2
            ;;
        # Force regeneration flag.
        --force|-f)
            # Set force flag.
            FORCE=1
            # Shift past flag.
            shift
            ;;
        # Check-only flag.
        --check)
            # Set check-only flag.
            CHECK_ONLY=1
            # Shift past flag.
            shift
            ;;
        # Clean flag.
        --clean)
            # Set clean-only flag.
            CLEAN_ONLY=1
            # Shift past flag.
            shift
            ;;
        # Help flag.
        --help|-h)
            # Display usage.
            usage
            ;;
        # Unrecognized option.
        *)
            # Print error message.
            echo "Error: Unknown option '$1'" >&2
            # Display usage.
            usage
            ;;
    esac
done

# Handle --clean action if requested.
if [[ $CLEAN_ONLY -eq 1 ]]; then
    # Inform user.
    echo "[+] Cleaning TPC-H table files in ${DBGEN_DIR}..."
    # Remove all .tbl files safely.
    rm -f "${DBGEN_DIR}"/*.tbl
    # Confirm deletion.
    echo "[+] TPC-H table files removed."
    # Exit cleanly.
    exit 0
fi

# Function to check if the large dataset already exists and is complete.
dataset_is_present() {
    # Verify customer table exists and is non-empty.
    if [[ ! -s "$CUST_TBL" ]]; then
        # Customer table missing or empty.
        return 1
    fi
    # Verify orders table exists and is non-empty.
    if [[ ! -s "$ORDERS_TBL" ]]; then
        # Orders table missing or empty.
        return 1
    fi
    # Query orders table file size in bytes using stat.
    local osize
    # Use Linux stat -c%s or BSD stat -f%z fallback.
    osize=$(stat -c%s "$ORDERS_TBL" 2>/dev/null || stat -f%z "$ORDERS_TBL" 2>/dev/null || echo 0)
    # Require at least 1 GB threshold (1,073,741,824 bytes). SF=50 orders.tbl is ~8.3 GB.
    if (( osize < 1073741824 )); then
        # File is undersized.
        return 1
    fi
    # Dataset is present and valid.
    return 0
}

# Handle --check action if requested.
if [[ $CHECK_ONLY -eq 1 ]]; then
    # Check if dataset exists.
    if dataset_is_present; then
        # Print status header.
        echo "[+] HashJoin Large Dataset: FOUND & READY"
        # Display file details.
        ls -lh "$CUST_TBL" "$ORDERS_TBL"
        # Exit with code 0.
        exit 0
    else
        # Print status warning.
        echo "[-] HashJoin Large Dataset: NOT FOUND or INCOMPLETE"
        # Check customer table specifically.
        [[ -f "$CUST_TBL" ]] && ls -lh "$CUST_TBL" || echo "  - customer.tbl: Missing"
        # Check orders table specifically.
        [[ -f "$ORDERS_TBL" ]] && ls -lh "$ORDERS_TBL" || echo "  - orders.tbl: Missing"
        # Exit with code 1.
        exit 1
    fi
fi

# Print banner.
echo "==============================================================================="
echo " HashJoin TPC-H Dataset Manager (Target: Scale Factor SF=${SCALE})"
echo "==============================================================================="

# Check if dataset already exists and force flag is NOT set.
if [[ $FORCE -eq 0 ]] && dataset_is_present; then
    # Inform user that dataset already exists.
    echo "[+] Large HashJoin dataset already exists. Skipping generation."
    # Show existing file properties.
    ls -lh "$CUST_TBL" "$ORDERS_TBL"
    # Inform user how to force regeneration if desired.
    echo "[*] Tip: Use '$0 --force' to recreate or overwrite existing tables."
    echo "==============================================================================="
    # Exit cleanly.
    exit 0
fi

# Validate dbgen source directory.
if [[ ! -d "$DBGEN_DIR" ]]; then
    # Print error if directory missing.
    echo "Error: dbgen directory not found at: ${DBGEN_DIR}" >&2
    # Exit with error.
    exit 1
fi

# Navigate into dbgen directory.
cd "$DBGEN_DIR"

# Build dbgen binary if missing.
if [[ ! -x ./dbgen ]]; then
    # Inform user.
    echo "[+] Compiling dbgen executable..."
    # Build with make.
    make -f makefile
fi

# Inform user of generation start.
echo "[+] Generating customer table (-T c) at SF=${SCALE}..."
# Run dbgen for customer table.
./dbgen -T c -s "${SCALE}" -f

# Inform user of orders generation.
echo "[+] Generating orders table (-T O) at SF=${SCALE}..."
# Run dbgen for orders table.
./dbgen -T O -s "${SCALE}" -f

# Display completed dataset files.
echo "==============================================================================="
echo "[+] HashJoin dataset generation completed successfully:"
# List generated files with sizes.
ls -lh "$CUST_TBL" "$ORDERS_TBL"
echo "==============================================================================="
