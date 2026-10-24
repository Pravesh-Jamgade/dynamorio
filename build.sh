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
        # Determine whether to use Ninja or Make based on existing CMake cache or tool availability.
        if [[ -f CMakeCache.txt ]]; then
            # Check if existing cache was configured for Ninja generator.
            if grep -q "CMAKE_GENERATOR:INTERNAL=Ninja" CMakeCache.txt 2>/dev/null; then
                # Keep using Ninja to prevent generator mismatch errors.
                USE_NINJA=1
            else
                # Keep using Make to match the existing Makefile cache.
                USE_NINJA=0
            fi
        # Check if ninja command is available on system for fresh configurations.
        elif command -v ninja >/dev/null 2>&1; then
            # Enable Ninja generator for fresh builds.
            USE_NINJA=1
        else
            # Default to Unix Makefiles generator.
            USE_NINJA=0
        fi

        # Execute build using the determined generator.
        if [[ $USE_NINJA -eq 1 ]]; then
            # Configure or update build with Ninja generator.
            cmake -GNinja ..
            # Compile targeted simulation binaries using Ninja with parallel cores.
            ninja -j"$NPROC" drrun drmemtrace drmemtrace_launcher drmemtrace_simulator
        else
            # Configure or update build with standard Make generator.
            cmake ..
            # Compile targeted simulation binaries using Make with parallel cores.
            make -j"$NPROC" drrun drmemtrace drmemtrace_launcher drmemtrace_simulator
        fi
        # Return to repository root directory.
        cd ..
        # Ensure root bin64 points to build/bin64 for universal compatibility.
        if [[ ! -L bin64 ]]; then
            # Remove any stale in-source bin64 directory.
            rm -rf bin64
            # Create symlink from bin64 to build/bin64.
            ln -s build/bin64 bin64
        fi
        # Display success message.
        echo "DynamoRIO build complete! drrun is at build/bin64/drrun (and bin64/drrun)"
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
