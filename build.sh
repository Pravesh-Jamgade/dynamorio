#!/usr/bin/env bash
# Quick build and test script for the Out-of-Order CPU, MMU, and 3-Level Cache Simulator.

# Exit immediately if a command exits with a non-zero status.
set -e

# Detect number of available CPU cores for parallel compilation.
NPROC=$(nproc 2>/dev/null || echo 4)

# Handle command line argument.
CMD="${1:-all}"

# Process command.
case "$CMD" in
    # Clean target.
    clean)
        echo "Cleaning build artifacts..."
        make clean
        ;;
    # Test target.
    test)
        echo "Building and running unit tests using $NPROC parallel jobs..."
        make -j"$NPROC" test
        ;;
    # Simulator-only target.
    sim)
        echo "Building standalone simulator using $NPROC parallel jobs..."
        make -j"$NPROC" sim
        ;;
    # Default target building everything.
    all|*)
        echo "Building simulator and tests using $NPROC parallel jobs..."
        make -j"$NPROC" all

        # If a DynamoRIO CMake build directory exists, also rebuild drcachesim automatically!
        if [ -f build/build.ninja ]; then
            echo ""
            echo "Rebuilding DynamoRIO live client (drcachesim) in build/..."
            ninja -C build drcachesim
        elif [ -f build/Makefile ]; then
            echo ""
            echo "Rebuilding DynamoRIO live client (drcachesim) in build/..."
            make -C build -j"$NPROC" drcachesim
        fi

        echo ""
        echo "=========================================================="
        echo "  BUILD SUCCESSFUL!"
        echo "  Run traces:    ./bin/run_cpu_sim --trace <path> [options]"
        echo "  Run live apps: ./build/bin64/drrun -t drmemtrace -simulator_type cpu -- ./app"
        echo "  Run tests:     ./build.sh test (or make test)"
        echo "=========================================================="
        ;;
esac
