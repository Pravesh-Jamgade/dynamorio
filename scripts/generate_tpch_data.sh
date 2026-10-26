#!/usr/bin/env bash
# ==============================================================================
# generate_tpch_data.sh: Generate Scaled TPC-H Datasets for HashJoin
# Supports generating small, medium, and very very large datasets (10s of GB).
# ==============================================================================
set -euo pipefail

# Directory where tpch-dbgen is located.
DBGEN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../apps/hashjoin/tpch-dbgen-master" && pwd)"

# Target scale factor passed as parameter (default: 50 -> ~10 GB input).
SCALE="${1:-50}"

echo "==============================================================================="
echo "Generating TPC-H tables at Scale Factor SF=${SCALE} in ${DBGEN_DIR}"
echo "==============================================================================="

# Navigate to dbgen directory.
cd "${DBGEN_DIR}"

# Build dbgen executable if missing.
if [ ! -f ./dbgen ]; then
    echo "[+] Compiling dbgen..."
    make -f makefile
fi

# Generate customer table (-T c).
echo "[+] Generating customer table (customer.tbl)..."
./dbgen -T c -s "${SCALE}" -f

# Generate orders table (-T O).
echo "[+] Generating orders table (orders.tbl)..."
./dbgen -T O -s "${SCALE}" -f

# Display file sizes.
echo "==============================================================================="
echo "[+] TPC-H table generation completed:"
ls -lh customer.tbl orders.tbl
echo "==============================================================================="
