#!/usr/bin/env bash
# Wrapper script to read nested INI configuration, compile the O3 engine,
# and optionally launch trace simulation.

# Exit immediately if a command exits with a non-zero status.
set -e

# Delegate execution to Python helper script with all passed arguments.
python3 scripts/build_from_config.py "$@"
