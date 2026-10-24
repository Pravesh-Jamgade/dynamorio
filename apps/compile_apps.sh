#!/bin/bash

# Exit on any error
set -e

APPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$APPS_DIR/.." && pwd)"
TARGET_ARCH="${1:-x86}"

if [ "$TARGET_ARCH" == "clean" ] || [ "$2" == "clean" ]; then
    echo "=========================================="
    echo "Cleaning all benchmarks..."
    echo "=========================================="
    
    cd "$REPO_ROOT/apps/btree"
    make clean || rm -f BTree *.o
    rm -f "$REPO_ROOT/apps/BTree"

    cd "$REPO_ROOT/apps/gups-app"
    make clean || rm -f gups *.o *.a
    rm -f "$REPO_ROOT/apps/gups"

    cd "$REPO_ROOT/apps/hashjoin"
    rm -f query10
    cd tpch-dbgen-master
    make clean || rm -f dbgen qgen *.o

    cd "$REPO_ROOT/apps/xsbench/src"
    make clean || rm -f XSBench *.o
    rm -f "$REPO_ROOT/apps/XSBench"

    echo "=========================================="
    echo "All benchmarks cleaned successfully!"
    echo "=========================================="
    exit 0
fi

# Determine compiler based on target architecture
if [ "$TARGET_ARCH" == "arm" ] || [ "$TARGET_ARCH" == "arm64" ] || [ "$TARGET_ARCH" == "aarch64" ]; then
    ARCH_NAME="arm"
    CC_BIN="aarch64-linux-gnu-gcc"
    OUTPUT_SCRIPT="$REPO_ROOT/configs/doing/run_apps_arm.sh"
    
    if ! command -v "$CC_BIN" &> /dev/null; then
        echo "Error: $CC_BIN is not installed."
        echo "Please install it via: sudo apt-get update && sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu"
        exit 1
    fi
elif [ "$TARGET_ARCH" == "x86" ] || [ "$TARGET_ARCH" == "x86_64" ]; then
    ARCH_NAME="x86"
    CC_BIN="gcc"
    OUTPUT_SCRIPT="$REPO_ROOT/configs/doing/run_apps.sh"
else
    echo "Usage: $0 [x86|arm] [clean]"
    echo "Example:"
    echo "  $0 x86    # Cross/native compile for x86 and generate configs/doing/run_apps.sh"
    echo "  $0 arm    # Cross-compile for ARM64 (AArch64) and generate configs/doing/run_apps_arm.sh"
    echo "  $0 clean  # Clean compiled binaries"
    exit 1
fi

echo "=========================================="
echo "Target Architecture: $ARCH_NAME (using $CC_BIN)"
echo "=========================================="

echo "1. Compiling BTree..."
cd "$REPO_ROOT/apps/btree"
$CC_BIN -std=gnu99 -Wall -O3 btree.c -o BTree
cp BTree "$REPO_ROOT/apps/BTree"

echo "2. Compiling GUPS..."
cd "$REPO_ROOT/apps/gups-app"
$CC_BIN -O3 gups.c -o gups
cp gups "$REPO_ROOT/apps/gups"

echo "3. Compiling HashJoin (query10)..."
cd "$REPO_ROOT/apps/hashjoin"
$CC_BIN -O3 main_query10.c hash_table.c murmurhash3.c hash_join.c fnv1a.c -o query10 -lm

# Build dbgen on host only if needed (for generating datasets)
cd tpch-dbgen-master
if [ ! -f customer.tbl ] || [ ! -f orders.tbl ]; then
    echo "Generating TPC-H datasets with host gcc..."
    make CC=gcc || true
    ./dbgen -s 0.01 || true
fi

echo "4. Compiling XSBench..."
cd "$REPO_ROOT/apps/xsbench/src"
make clean
make CC="$CC_BIN"
cp XSBench "$REPO_ROOT/apps/XSBench"

cd "$REPO_ROOT"

echo "=========================================="
echo "Packaging compiled $ARCH_NAME binaries into $OUTPUT_SCRIPT..."
echo "=========================================="

chmod -R u+rw "$REPO_ROOT/apps/hashjoin/tpch-dbgen-master"/*.tbl 2>/dev/null || true

python3 -c "
import subprocess
import base64
import os

repo_root = '$REPO_ROOT'
output_file = '$OUTPUT_SCRIPT'
arch = '$ARCH_NAME'

header = '''#!/bin/bash
# =====================================================================
# WHY BASE64?
# gem5 hands the guest exactly ONE file via m5 readfile (this script).
# The stock Ubuntu disk image does not contain our benchmark binaries,
# so we smuggle them in *inside* this script:
#   host:  tar -czf apps/ | base64   -> pasted below as text
#   guest: base64 -d -> apps.tar.gz  -> tar -xzf
# base64 is needed because a shell script must be plain text: raw gzip
# bytes contain NULs/control chars that would corrupt it. base64 maps
# them to safe ASCII (A-Z a-z 0-9 + /) at a ~33% size cost.
# The quoted heredoc (<< PAYLOAD in quotes) feeds the text to base64 -d
# without the shell expanding any dollar/backtick characters inside it.
# Alternative: build a custom disk image with the apps preinstalled.
# =====================================================================
echo \\\"Starting simulation script for ''' + arch.upper() + '''\\\"
cd /root
mkdir -p apps
cd apps
echo \"Decoding payload...\"
base64 -d << 'PAYLOAD' > apps.tar.gz
'''

footer = '''PAYLOAD
echo \"Extracting payload...\"
tar -xzf apps.tar.gz

__TEST_TYPE__=\"smoke\"
__APP_NAME__=\"all\"

# =====================================================================
# 1. BTree (B-Tree data structure benchmark)
# =====================================================================
if [ \"\$__APP_NAME__\" == \"all\" ] || [ \"\$__APP_NAME__\" == \"btree\" ]; then
    if [ \"\$__TEST_TYPE__\" == \"smoke\" ]; then
        echo \"Running BTree (Smoke Test)...\"
        # Smoke test: 1,000 elements, 100 lookups. Runs very quickly to verify correctness.
        ./BTree 1000 100
    fi

    if [ \"\$__TEST_TYPE__\" == \"large\" ]; then
        echo \"Running BTree (Large Test)...\"
        # Large test: 10,000,000 elements, 1,000,000 lookups.
        # Stresses the TLB and forces Page Table Walks.
        ./BTree 10000000 1000000
    fi
fi

# =====================================================================
# 2. GUPS (Giga Updates Per Second - Random Access memory benchmark)
# =====================================================================
if [ \"\$__APP_NAME__\" == \"all\" ] || [ \"\$__APP_NAME__\" == \"gups\" ]; then
    if [ \"\$__TEST_TYPE__\" == \"large\" ]; then
        echo \"Running gups (Large Test)...\"
        # GUPS allocates 1 GB of memory. Completely random updates heavily exercise the TLB & MMU.
        ./gups-app/gups 1
    fi
fi

# =====================================================================
# 3. HashJoin (Database join algorithm benchmark)
# =====================================================================
if [ \"\$__APP_NAME__\" == \"all\" ] || [ \"\$__APP_NAME__\" == \"hashjoin\" ]; then
    if [ \"\$__TEST_TYPE__\" == \"smoke\" ]; then
        echo \"Running hashjoin (Smoke Test)...\"
        ./hashjoin/query10 murmur3 hashjoin/tpch-dbgen-master/customer.tbl 150 hashjoin/tpch-dbgen-master/orders.tbl 1500 17 1
    fi

    if [ \"\$__TEST_TYPE__\" == \"large\" ]; then
        echo \"Running hashjoin (Large Test)...\"
        ./hashjoin/query10 murmur3 hashjoin/tpch-dbgen-master/customer.tbl 1500 hashjoin/tpch-dbgen-master/orders.tbl 15000 16381 1
    fi
fi

# =====================================================================
# 4. XSBench (Monte Carlo particle transport simulation)
# =====================================================================
if [ \"\$__APP_NAME__\" == \"all\" ] || [ \"\$__APP_NAME__\" == \"xsbench\" ]; then
    if [ \"\$__TEST_TYPE__\" == \"smoke\" ]; then
        echo \"Running xsbench (Smoke Test)...\"
        ./XSBench -s small -g 100 -p 100 -l 100
    fi

    if [ \"\$__TEST_TYPE__\" == \"large\" ]; then
        echo \"Running xsbench (Large Test)...\"
        ./XSBench -s large -g 1000 -p 1000 -l 10000
    fi
fi

echo \"Done!\"
m5 exit
'''

p = subprocess.Popen(
    ['tar', '-czf', '-', '-C', 'apps', 
     '--exclude=compile_apps.sh',
     '--exclude=hashjoin/.git', 
     '--exclude=hashjoin/tpch-dbgen-master.zip', 
     '--exclude=xsbench/src/*.o', 
     '--exclude=hashjoin/tpch-dbgen-master/*.o', 
     '.'],
    stdout=subprocess.PIPE,
    cwd=repo_root
)
tar_data, _ = p.communicate()
b64_data = base64.b64encode(tar_data).decode('utf-8')

os.makedirs(os.path.dirname(output_file), exist_ok=True)
with open(output_file, 'w') as f:
    f.write(header + b64_data + '\n' + footer)

print(f'Successfully generated {output_file}')
"

echo "=========================================="
echo "Finished! $ARCH_NAME binaries compiled and packaged."
echo "=========================================="
