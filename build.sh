#!/usr/bin/env bash
# Quick build and test script for the Out-of-Order CPU, MMU, and 3-Level Cache Simulator.

# Exit immediately if a command exits with a non-zero status.
set -e

# Verify Makefile integrity: detect if in-source cmake overwrote our custom Makefile.
if grep -q "CMAKE_BINARY_DIR" Makefile 2>/dev/null; then
    # Print warning message indicating an in-source cmake conflict occurred.
    echo "[!] Warning: Root Makefile was overwritten by in-source CMake execution."
    # Inform user that the original simulator Makefile is being restored from Git.
    echo "[!] Restoring original simulator Makefile from Git..."
    # Restore the tracked Makefile from Git.
    git checkout Makefile 2>/dev/null || true
    # Remove lingering in-source CMakeCache.txt.
    rm -f CMakeCache.txt
fi

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
        # Inform user of building the standalone simulator.
        echo "Building standalone simulator using $NPROC parallel jobs..."
        # Compile standalone binary via Makefile.
        make -j"$NPROC" sim
        ;;
    # DynamoRIO runtime and tracer build target (drrun & drmemtrace).
    drrun|cmake|runtime)
        # Inform user of building DynamoRIO runtime.
        echo "Building DynamoRIO runtime (drrun) and drcachesim tracer..."
        # Create dedicated out-of-source build directory.
        mkdir -p build && cd build
        # Check if ninja generator is available on system.
        if command -v ninja >/dev/null 2>&1; then
            # Configure build using Ninja generator.
            cmake -GNinja ..
            # Compile targeted simulation binaries (drrun, drmemtrace, launcher, and simulator).
            ninja -j"$NPROC" drrun drmemtrace drmemtrace_launcher drmemtrace_simulator
        else
            # Fall back to standard CMake Unix Makefiles generator.
            cmake ..
            # Compile targeted simulation binaries (drrun, drmemtrace, launcher, and simulator).
            make -j"$NPROC" drrun drmemtrace drmemtrace_launcher drmemtrace_simulator
        fi
        # Return to repository root directory.
        cd ..
        # Display success message.
        echo "DynamoRIO build complete! drrun is at build/bin64/drrun"
        ;;
    # Default target building everything.
    all|*)
        echo "Building simulator and tests using $NPROC parallel jobs..."
        make -j"$NPROC" all

        echo ""
        echo "=========================================================="
        echo "  BUILD SUCCESSFUL!"
        echo "  Run traces:    ./bin/run_cpu_sim --trace <path> [options]"
        echo "  Run tests:     ./build.sh test (or make test)"
        echo "=========================================================="
        ;;
esac
