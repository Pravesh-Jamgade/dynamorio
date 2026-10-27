#!/usr/bin/env bash
# ==============================================================================
# generate_tpch_data.sh: Wrapper for create_hashjoin_dataset.sh
# ==============================================================================
# Enable strict error handling.
set -euo pipefail

# Determine script directory.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Extract requested scale factor (default: 50).
SCALE="${1:-50}"

# Forward execution to create_hashjoin_dataset.sh with --scale and --force flags.
exec "${SCRIPT_DIR}/create_hashjoin_dataset.sh" --scale "${SCALE}" --force
