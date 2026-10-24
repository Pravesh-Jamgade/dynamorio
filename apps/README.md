# Benchmark Applications & DynamoRIO Simulation Guide

This directory contains benchmark workloads for architectural evaluation, stressing the Out-of-Order (O3) CPU pipeline, Multi-Level Cache Hierarchy (L1D, L2, shared LLC), and the Hardware Memory Management Unit (MMU) / Page Table Walker (PTW).

---

## 1. Quick Compilation (`compile_apps.sh`)

Use [`apps/compile_apps.sh`](file:///media/pravesh/Storage/code/dyn/dynamorio/apps/compile_apps.sh) to compile all benchmark suites for the target host architecture:

```bash
# Compile natively for x86-64 (using host gcc):
./apps/compile_apps.sh x86

# Clean all compiled binaries and temporary object files:
./apps/compile_apps.sh clean
```

Executable binaries are generated at the root of [`apps/`](file:///media/pravesh/Storage/code/dyn/dynamorio/apps/):
* [`apps/hello`](file:///media/pravesh/Storage/code/dyn/dynamorio/apps/hello): Baseline sanity check application.
* [`apps/BTree`](file:///media/pravesh/Storage/code/dyn/dynamorio/apps/BTree): B+ Tree index lookup and traversal benchmark.
* [`apps/gups`](file:///media/pravesh/Storage/code/dyn/dynamorio/apps/gups): Giga Updates Per Second memory and TLB stressor.
* [`apps/hashjoin/query10`](file:///media/pravesh/Storage/code/dyn/dynamorio/apps/hashjoin/query10): TPC-H Query 10 hash join relational engine.
* [`apps/XSBench`](file:///media/pravesh/Storage/code/dyn/dynamorio/apps/XSBench): Monte Carlo neutron cross-section computational kernel.

---

## 2. Automated Benchmark Runner (`run_benchmarks.sh`)

An automated job runner is available at [`run_benchmarks.sh`](file:///media/pravesh/Storage/code/dyn/dynamorio/run_benchmarks.sh) (or [`scripts/run_benchmarks.sh`](file:///media/pravesh/Storage/code/dyn/dynamorio/scripts/run_benchmarks.sh)). It manages launcher paths, INI configuration profiles, periodic progress heartbeats, CSV statistics generation, and tabular comparison reports.

### Common Options:
* `--app <name>`: `hello`, `btree`, `gups`, `hashjoin`, `xsbench`, or `all` (default: `all`)
* `--mode <mode>`: `smoke` (fast test) or `large` (full memory footprint) (default: `smoke`)
* `--config <file>`: INI configuration path (default: `configs/o3_server.ini`)
* `--heartbeat <N>`: Instruction interval between progress heartbeats (default: `10000000`)
* `--gups-mem <GB>`: Table size in GB for GUPS (e.g. `64` for 512GB RAM servers; `0` for 256MB smoke)
* `--sim-refs <N>`: Limit maximum simulated memory references (useful for bounded large runs)
* `--outdir <dir>`: Output directory for logs (default: `results/`)
* `--dry-run`: Preview execution commands without launching
* `--list`: List available benchmarks and their commands

---

## 3. Detailed Per-Application Execution Guide

### 3.1. Hello (`apps/hello`)
* **Description**: Minimal C program used to verify simulator initialization, dynamic linker loading, dynamic instruction execution, and stats emission.
* **Standalone Native**:
  ```bash
  ./apps/hello
  ```
* **Under DynamoRIO O3 Simulator (Direct)**:
  ```bash
  ./build/bin64/drrun -t drmemtrace -simulator_type cpu -cpu_config configs/o3_server.ini -stats_file results/stats_hello.log -- ./apps/hello
  ```
* **Via Job Script**:
  ```bash
  ./run_benchmarks.sh --app hello
  ```

---

### 3.2. BTree (`apps/BTree`)
* **Description**: Evaluates pointer-chasing memory access patterns and TLB/Cache behavior during B+ Tree traversals and insertions.
* **Arguments**: `./apps/BTree <num_elements> <num_lookups>`
* **Modes**:
  * **Smoke Test**: `1000` elements, `100` lookups (instantaneous).
  * **Large Test**: `10000000` elements, `1000000` lookups (massive memory footprint, heavy TLB pressure).
* **Standalone Native**:
  ```bash
  # Smoke:
  ./apps/BTree 1000 100

  # Large:
  ./apps/BTree 10000000 1000000
  ```
* **Under DynamoRIO O3 Simulator (Direct)**:
  ```bash
  # Smoke run:
  ./build/bin64/drrun -t drmemtrace -simulator_type cpu -cpu_config configs/o3_server.ini -stats_file results/stats_btree_smoke.log -- ./apps/BTree 1000 100

  # Large run with 10M heartbeat:
  ./build/bin64/drrun -t drmemtrace -simulator_type cpu -cpu_config configs/o3_server.ini -heartbeat_interval 10000000 -stats_file results/stats_btree_large.log -- ./apps/BTree 10000000 1000000
  ```
* **Via Job Script**:
  ```bash
  # Smoke run:
  ./run_benchmarks.sh --app btree --mode smoke

  # Large run:
  ./run_benchmarks.sh --app btree --mode large
  ```

---

### 3.3. GUPS (`apps/gups`)
* **Description**: Giga Updates Per Second. Performs uniform pseudo-random 64-bit read-modify-write updates across a massive memory table, causing almost 100% TLB and Last-Level Cache misses to heavily exercise the hardware page table walker.
* **Arguments**: `./apps/gups <memory_in_GB>`
  * `0`: Quick smoke test (allocates 256 MB table).
  * `1`: 1 GB table (suitable for typical developer machines with 16GB RAM).
  * `64`: **64 GB table** (recommended for production servers with 512GB RAM).
* **Standalone Native**:
  ```bash
  # Smoke test (256 MB):
  ./apps/gups 0

  # Large test (1 GB for workstations, 64 GB for servers):
  ./apps/gups 1
  ./apps/gups 64
  ```
* **Under DynamoRIO O3 Simulator (Direct)**:
  ```bash
  # 256 MB smoke run:
  ./build/bin64/drrun -t drmemtrace -simulator_type cpu -cpu_config configs/o3_server.ini -stats_file results/stats_gups_smoke.log -- ./apps/gups 0

  # 64 GB server run (optionally bounded to 50M references):
  ./build/bin64/drrun -t drmemtrace -simulator_type cpu -cpu_config configs/o3_server.ini -sim_refs 50000000 -heartbeat_interval 5000000 -stats_file results/stats_gups_64gb.log -- ./apps/gups 64
  ```
* **Via Job Script**:
  ```bash
  # Smoke run:
  ./run_benchmarks.sh --app gups --mode smoke

  # Server 64 GB run:
  ./run_benchmarks.sh --app gups --mode large --gups-mem 64

  # Server 64 GB run bounded to 20M references:
  ./run_benchmarks.sh --app gups --mode large --gups-mem 64 --sim-refs 20000000 --heartbeat 2000000
  ```

---

### 3.4. HashJoin (`apps/hashjoin/query10`)
* **Description**: Database relational hash join kernel (TPC-H Query 10). Builds an in-memory hash table on `customer.tbl` and probes it with streamed records from `orders.tbl`.
* **Prerequisites**: Dataset files in `apps/hashjoin/tpch-dbgen-master/` (automatically built by `compile_apps.sh` or `run_benchmarks.sh`).
* **Arguments**:
  ```bash
  ./apps/hashjoin/query10 <hash_algo> <small_tbl> <small_rows> <large_tbl> <large_rows> <hash_table_size> <run_id>
  ```
* **Modes**:
  * **Smoke Test**: 150 customer rows, 1,500 order rows, hash table size 17.
  * **Large Test**: 1,500 customer rows, 15,000 order rows, hash table size 16,381.
* **Standalone Native**:
  ```bash
  # Smoke:
  ./apps/hashjoin/query10 murmur3 apps/hashjoin/tpch-dbgen-master/customer.tbl 150 apps/hashjoin/tpch-dbgen-master/orders.tbl 1500 17 1

  # Large:
  ./apps/hashjoin/query10 murmur3 apps/hashjoin/tpch-dbgen-master/customer.tbl 1500 apps/hashjoin/tpch-dbgen-master/orders.tbl 15000 16381 1
  ```
* **Under DynamoRIO O3 Simulator (Direct)**:
  ```bash
  # Smoke run:
  ./build/bin64/drrun -t drmemtrace -simulator_type cpu -cpu_config configs/o3_server.ini -stats_file results/stats_hashjoin_smoke.log -- ./apps/hashjoin/query10 murmur3 apps/hashjoin/tpch-dbgen-master/customer.tbl 150 apps/hashjoin/tpch-dbgen-master/orders.tbl 1500 17 1
  ```
* **Via Job Script**:
  ```bash
  # Smoke run:
  ./run_benchmarks.sh --app hashjoin --mode smoke

  # Large run:
  ./run_benchmarks.sh --app hashjoin --mode large
  ```

---

### 3.5. XSBench (`apps/XSBench`)
* **Description**: Monte Carlo neutron transport macroscopic cross-section lookup loop. Involves random memory accesses into a large energy grid data structure, stressing memory bandwidth and TLB caches.
* **Arguments**: `./apps/XSBench -s <size> -g <gridpoints> -p <particles> -l <lookups>`
* **Modes**:
  * **Smoke Test**: `-s small -g 100 -p 100 -l 100`
  * **Large Test**: `-s large -g 1000 -p 1000 -l 10000`
* **Standalone Native**:
  ```bash
  # Smoke:
  ./apps/XSBench -s small -g 100 -p 100 -l 100

  # Large:
  ./apps/XSBench -s large -g 1000 -p 1000 -l 10000
  ```
* **Under DynamoRIO O3 Simulator (Direct)**:
  ```bash
  # Smoke run:
  ./build/bin64/drrun -t drmemtrace -simulator_type cpu -cpu_config configs/o3_server.ini -stats_file results/stats_xsbench_smoke.log -- ./apps/XSBench -s small -g 100 -p 100 -l 100
  ```
* **Via Job Script**:
  ```bash
  # Smoke run:
  ./run_benchmarks.sh --app xsbench --mode smoke

  # Large run:
  ./run_benchmarks.sh --app xsbench --mode large
  ```

---

## 4. Scraping CSV Metrics & Results

All simulations emit structured CSV formatted statistics both to the terminal and to the file specified by `-stats_file` (default `results/stats_<app>_<mode>.log`).

### Scalar Metrics Format:
```csv
ipc, 0.5156
cpi, 1.9394
cycles, 449224
instructions_retired, 231628
instructions_fetched, 179228
loads, 39283
stores, 13117
L1D hits, 51305
L1D misses, 9845
L1D miss rate, 0.1610
L2 hits, 7521
L2 misses, 4181
L2 total miss rate, 0.0137
LLC hits, 0
LLC misses, 4181
PTW walks, 179
PTW memory reads, 194
Avg PTW mem reads, 1.0838
```

### Vector PTW Cache Hit Metrics Format:
```csv
cpu0,ptw_level0, L1I,L1D,L2,LLC,DRAM
cpu0,ptw_level0, 0, 102, 27, 0, 50
cpu0,ptw_level1, 0, 1, 4, 0, 5
cpu0,ptw_level2, 0, 0, 0, 0, 3
cpu0,ptw_level3, 0, 0, 0, 0, 2
cpu0,ptw_level4, 0, 0, 0, 0, 2
```

### Parsing Metrics with Awk:
```bash
# Extract IPC across all smoke benchmark runs:
awk -F', *' '$1=="ipc" {print FILENAME, $2}' results/stats_*_smoke.log

# Extract Page Table Walks:
awk -F', *' '$1=="PTW walks" {print FILENAME, $2}' results/stats_*_smoke.log
```

---

## 5. Region of Interest (ROI) & Function-Level Simulation

When benchmarking large applications (e.g. GUPS with 64GB table allocation, BTree with 10M elements, or TPC-H database table generation), simulating the entire execution from `main()` spends millions of cycles in setup, libc dynamic linking, and memory allocation.

DynamoRIO supports isolating and simulating **only the critical computational kernel** using Region of Interest (ROI) markers or function filters.

### Method 1: Function Markers via `roi_begin()` / `roi_end()` (Zero External Dependencies)

Define empty, non-inlined functions at the boundaries of your hot loop:

```c
// Prevent compiler from inlining boundary functions
__attribute__((noinline)) void roi_begin() {
    __asm__ volatile("" : : : "memory");
}

__attribute__((noinline)) void roi_end() {
    __asm__ volatile("" : : : "memory");
}

int main() {
    // 1. Setup / Memory Allocation (SKIPPED)
    initialize_huge_tables();

    // 2. Start of Region of Interest
    roi_begin();

    // >>> ONLY THIS CODE IS SIMULATED <<<
    run_benchmark_kernel();

    // 3. End of Region of Interest
    roi_end();

    // 4. Teardown / Verification (SKIPPED)
    verify_and_cleanup();
    return 0;
}
```

* **Compilation**: Compile normally with standard `gcc` (no DynamoRIO headers or libraries needed):
  ```bash
  gcc -O3 my_app.c -o my_app
  ```
* **Execution**: Pass `-record_function` flags to `drrun`:
  ```bash
  ./build/bin64/drrun -t drmemtrace \
    -record_function "roi_begin|0" \
    -record_function "roi_end|0" \
    -simulator_type cpu \
    -cpu_config configs/o3_server.ini \
    -- ./my_app
  ```

---

### Method 2: DynamoRIO Native Start/Stop API (`dr_app_start()` / `dr_app_stop()`)

The official DynamoRIO application interface allows the process to run at 100% native CPU hardware speed (zero binary instrumentation overhead) until `dr_app_start()` is reached:

```c
#include "dr_api.h"

int main() {
    // Runs at full native hardware clock speed
    allocate_64gb_memory();

    // Attach DynamoRIO and start cycle-accurate simulation
    dr_app_start();

    // === Region of Interest ===
    run_parallel_kernel();

    // Detach DynamoRIO and return to native execution
    dr_app_stop();

    free_memory();
    return 0;
}
```

* **Compilation**: Link against DynamoRIO:
  ```bash
  gcc -O3 my_app.c \
    -I/media/pravesh/Storage/code/dyn/dynamorio/build/include \
    -L/media/pravesh/Storage/code/dyn/dynamorio/build/lib64/release \
    -ldynamorio \
    -o my_app
  ```
* **Execution**: Run with standard `drrun` (no special flags required):
  ```bash
  ./build/bin64/drrun -t drmemtrace -simulator_type cpu -cpu_config configs/o3_server.ini -- ./my_app
  ```

---

### Method 3: Instruction Window ROI (Zero Code Modifications)

If you do not wish to modify application source code, specify instruction thresholds on the command line:

* `-trace_after_instrs <N>`: Fast-forward natively through the first $N$ instructions (skip startup/loading).
* `-trace_for_instrs <M>`: Simulate only the subsequent $M$ instructions (ROI window), then terminate.

```bash
# Skip first 100M instructions of setup, simulate next 20M instructions of ROI:
./build/bin64/drrun -t drmemtrace \
  -trace_after_instrs 100000000 \
  -trace_for_instrs 20000000 \
  -simulator_type cpu \
  -cpu_config configs/o3_server.ini \
  -stats_file results/stats_roi.log \
  -- ./apps/gups 1
```

---

### Method 4: Simulating Specific Target Functions

To simulate only specific routines (e.g. `btree_search` and `btree_insert`):

```c
#include "dr_api.h"

__attribute__((noinline)) int btree_search(btree_t *tree, int key) {
    dr_app_start();
    int res = internal_search(tree, key);
    dr_app_stop();
    return res;
}

__attribute__((noinline)) void btree_insert(btree_t *tree, int key, int val) {
    dr_app_start();
    internal_insert(tree, key, val);
    dr_app_stop();
}
```

Or via CLI symbol interception without editing source code:
```bash
./build/bin64/drrun -t drmemtrace \
  -record_function "btree_search|1&btree_insert|2" \
  -simulator_type cpu \
  -cpu_config configs/o3_server.ini \
  -- ./apps/BTree 100000 1000
```

---

### Behavior with Child Functions & Libraries

* **Dynamic Scope**: `dr_app_start()` and `dr_app_stop()` act like a stopwatch. **Any function called between start and stop (child functions, math libraries, `memcpy`, etc.) is fully traced down the entire call tree.**
* **Pausing Simulation**: If you need to exclude an expensive debug print or disk write inside your ROI, you can pause simulation by nesting stop and start calls:
  ```c
  dr_app_start();
  step_1();

  // Temporarily pause simulation for non-critical logging:
  dr_app_stop();
  printf("Finished step 1\n");
  dr_app_start();

  step_2();
  dr_app_stop();
  ```

---

### ROI Options Comparison Table

| Approach | Code Modification | Compilation / Linker Flags | `drrun` CLI Flags | Overhead Outside ROI |
| :--- | :--- | :--- | :--- | :--- |
| **`roi_begin()` / `roi_end()`** | Minimal (2 empty functions) | Standard `gcc` (none) | `-record_function "roi_begin\|0&roi_end\|0"` | Trace marker inserted |
| **`dr_app_start()` / `dr_app_stop()`** | `#include "dr_api.h"` | `-I.../build/include -L.../build/lib64/release -ldynamorio` | None (standard `drrun` command) | **0%** (full native hardware speed) |
| **Instruction Window** | **None** | Standard `gcc` (none) | `-trace_after_instrs <N> -trace_for_instrs <M>` | Minimal fast-forward |

