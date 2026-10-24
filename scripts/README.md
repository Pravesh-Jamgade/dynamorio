# Scripts Directory: Tooling & Benchmark Execution Guide

This directory houses automation, orchestration, and build scripts for the DynamoRIO Out-of-Order (O3) CPU, MMU/Page Table Walker, and Multi-Level Cache simulator framework.

---

## 1. Directory Structure & Root Symlinks

For maximum convenience when working from the repository root, top-level symlinks are provided for the primary execution scripts:

```text
dynamorio/
├── build.sh                                        # Unified parallel build & test script
├── run_jobfile.sh    -> scripts/run_jobfile.sh     # Batch parallel job executor
├── run_benchmarks.sh -> scripts/run_benchmarks.sh  # Parametric single/suite runner
├── scripts/
│   ├── run_jobfile.sh        # Implementation of batch job runner
│   ├── run_benchmarks.sh     # Implementation of benchmark runner
│   ├── build_from_config.py  # INI parser and simulator builder
│   └── README.md             # This documentation
├── apps/
│   └── compile_apps.sh       # Compiles benchmark binaries (x86 / arm)
└── jobs/
    ├── jobfile.txt           # Clean executable command list
    └── jobfile.csv           # Experiment matrix metadata
```

Because the root files are symbolic links (`ln -s scripts/...`), you can run them either as `./run_jobfile.sh` or `./scripts/run_jobfile.sh` with identical behavior.

---

## 2. Core Build Automation Script (`build.sh`)

### Purpose
[`build.sh`](file:///media/pravesh/Storage/code/dyn/dynamorio/build.sh) is the central build script for compiling the Out-of-Order CPU engine, MMU Page Table Walker, and 3-Level Cache hierarchy. It auto-detects system CPU core counts (`nproc`) to parallelize builds across all available cores.

### Syntax
```bash
./build.sh [TARGET]
```

### Supported Targets
| Target | Command | Description | Output |
| :--- | :--- | :--- | :--- |
| `sim` | `./build.sh sim` *(or `./build.sh`)* | Fast parallel compilation of the standalone trace simulation engine. | [`bin/run_cpu_sim`](file:///media/pravesh/Storage/code/dyn/dynamorio/bin/run_cpu_sim) |
| `drrun` | `./build.sh drrun` | Builds DynamoRIO's live execution engine and drcachesim tracer client. Auto-detects Ninja/Make and creates `bin64 -> build/bin64` symlink. | [`build/bin64/drrun`](file:///media/pravesh/Storage/code/dyn/dynamorio/build/bin64/drrun) |
| `test` | `./build.sh test` | Compiles and executes the full 14-suite regression test suite across all cores. | [`bin/unit_tests`](file:///media/pravesh/Storage/code/dyn/dynamorio/bin/unit_tests) |
| `clean` | `./build.sh clean` | Removes all compiled objects and binary directories (`build_sim/`, `bin/`). | — |

### Examples
```bash
# 1. Build standalone trace simulator:
./build.sh sim

# 2. Build live application engine and tracer:
./build.sh drrun

# 3. Run regression unit tests:
./build.sh test

# 4. Clean compiled binaries:
./build.sh clean
```

---

## 3. Batch Parallel Job Runner (`run_jobfile.sh`)

### Purpose
[`scripts/run_jobfile.sh`](file:///media/pravesh/Storage/code/dyn/dynamorio/scripts/run_jobfile.sh) reads [`jobs/jobfile.txt`](file:///media/pravesh/Storage/code/dyn/dynamorio/jobs/jobfile.txt) (or any custom command list) line-by-line, pools processes, and executes up to **$N$ jobs concurrently**. It records independent console logs for every job, captures per-simulation CSV statistics, and prints a formatted comparative metrics table at the conclusion of all jobs.

### Syntax
```bash
./run_jobfile.sh [OPTIONS]
# or
./scripts/run_jobfile.sh [OPTIONS]
```

### Options Reference
| Flag | Description | Default |
| :--- | :--- | :--- |
| `-n`, `-j`, `--parallel <N>` | Number of simulation jobs to run concurrently | `1` |
| `--lines`, `--jobs <RANGES>` | Filter specific command line numbers (e.g. `1`, `1-4`, `1,3,5-8`) | All lines |
| `--file <PATH>` | Path to job command file | `jobs/jobfile.txt` |
| `--outdir <DIR>` | Directory for console logs and stats files | `results` |
| `--dry-run` | Print commands that would be executed without running | Disabled |
| `--stats-only` | Skip execution and parse existing stats into the summary table | Disabled |
| `-h`, `--help` | Display usage and help message | — |

### Examples

#### Example 1: Run dry-run to preview commands
```bash
./run_jobfile.sh --dry-run
```

#### Example 2: Run 4 jobs concurrently across lines 1 to 4
```bash
./run_jobfile.sh -n 4 --lines 1-4
```

#### Example 3: Run specific jobs in parallel (e.g. hello server, btree medium, and gups smoke)
```bash
./run_jobfile.sh -n 3 --lines 2,5,8
```

#### Example 4: Run all 21 jobs with 8 concurrent workers
```bash
./run_jobfile.sh -n 8
```

#### Example 5: Reparse summary table from completed logs without re-executing
```bash
./run_jobfile.sh --stats-only
```

---

## 4. Parametric Benchmark Runner (`run_benchmarks.sh`)

### Purpose
[`scripts/run_benchmarks.sh`](file:///media/pravesh/Storage/code/dyn/dynamorio/scripts/run_benchmarks.sh) provides a high-level parametric interface to launch individual benchmarks (`hello`, `btree`, `gups`, `hashjoin`, `xsbench`) or the entire suite. It dynamically handles CPU configuration profiles (`configs/o3_default.ini`, `configs/o3_server.ini`), simulation limits, and huge memory allocations.

### Syntax
```bash
./run_benchmarks.sh [OPTIONS]
# or
./scripts/run_benchmarks.sh [OPTIONS]
```

### Options Reference
| Flag | Description | Default |
| :--- | :--- | :--- |
| `--app <NAME>` | Target workload: `hello`, `btree`, `gups`, `hashjoin`, `xsbench`, `all` | `all` |
| `--mode <MODE>` | Scale mode: `smoke` (quick test) or `large` (full memory footprint) | `smoke` |
| `--config <FILE>` | Microarchitecture INI configuration file | `configs/o3_server.ini` |
| `--heartbeat <N>` | Heartbeat instruction progress interval | `10000000` (10M) |
| `--sim-refs <N>` | Cap maximum simulated memory references (0 = unlimited) | `0` |
| `--gups-mem <GB>` | Working set size for GUPS in GB (e.g. `64` for 512GB RAM servers) | `0` (256 MB) |
| `--outdir <DIR>` | Directory for output console logs and stats files | `results` |
| `--dry-run` | Display the generated command without executing | Disabled |
| `--list` | Print the exact invocation command for each benchmark | — |
| `-h`, `--help` | Display usage and help message | — |

### Examples

#### Example 1: Run a quick smoke test on BTree
```bash
./run_benchmarks.sh --app btree --mode smoke
```

#### Example 2: Run GUPS with a 64 GB table on a high-memory server (capped at 50M memory refs)
```bash
./run_benchmarks.sh --app gups --mode large --gups-mem 64 --sim-refs 50000000
```

#### Example 3: Run HashJoin Query 10 with custom desktop configuration
```bash
./run_benchmarks.sh --app hashjoin --config configs/o3_default.ini --mode smoke
```

#### Example 4: List all benchmark commands and their configurations
```bash
./run_benchmarks.sh --list
```

---

## 5. INI Config Parser & Simulator Builder (`build_from_config.py`)

### Purpose
[`scripts/build_from_config.py`](file:///media/pravesh/Storage/code/dyn/dynamorio/scripts/build_from_config.py) resolves nested INI configuration files (handling recursive `include = ...` statements), validates microarchitectural parameters, triggers parallel compilation of the simulator, and can immediately execute an instruction trace through the compiled simulator.

### Syntax
```bash
python3 scripts/build_from_config.py [OPTIONS]
```

### Options Reference
| Flag | Description | Default |
| :--- | :--- | :--- |
| `-c`, `--config <PATH>` | Path to primary INI configuration file | `configs/o3_default.ini` |
| `--target <TARGET>` | Makefile target (`sim`, `test`, `all`) | `sim` |
| `-j`, `--jobs <N>` | Number of parallel compilation processes | CPU core count |
| `--trace <PATH>` | Optional trace file to simulate immediately after build | None |
| `--max_instrs <N>` | Maximum instruction count for trace simulation | `1000000` |

### Examples

#### Example 1: Validate INI config and build the simulator
```bash
python3 scripts/build_from_config.py --config configs/o3_server.ini --target sim -j 8
```

#### Example 2: Build and run an offline memory trace file
```bash
python3 scripts/build_from_config.py \
  --config configs/o3_server.ini \
  --trace /path/to/trace.drmemtrace \
  --max_instrs 10000000
```

---

## 6. Benchmark Application Compiler (`apps/compile_apps.sh`)

### Purpose
[`apps/compile_apps.sh`](file:///media/pravesh/Storage/code/dyn/dynamorio/apps/compile_apps.sh) compiles all application source code into standalone executable binaries in the [`apps/`](file:///media/pravesh/Storage/code/dyn/dynamorio/apps/) directory.

### Syntax
```bash
./apps/compile_apps.sh [x86|arm] [clean]
```

### Modes
* `x86`: Builds native 64-bit binaries using host `gcc` / `make`.
* `arm`: Cross-compiles for AArch64 using `aarch64-linux-gnu-gcc`.
* `clean`: Removes all compiled binaries, intermediate `.o` object files, and generated data tables.

### Examples

#### Example 1: Compile all benchmarks for x86-64 host
```bash
./apps/compile_apps.sh x86
```

#### Example 2: Clean build artifacts
```bash
./apps/compile_apps.sh clean
```

---

## 7. Output Files & Results Structure

When running simulations via [`run_jobfile.sh`](file:///media/pravesh/Storage/code/dyn/dynamorio/run_jobfile.sh) or [`run_benchmarks.sh`](file:///media/pravesh/Storage/code/dyn/dynamorio/run_benchmarks.sh), results are saved to the `results/` directory:

| File Pattern | Description |
| :--- | :--- |
| `results/console_lineXX.log` | Complete stdout/stderr log for command line XX in `jobfile.txt`. |
| `results/console_<app>.log` | Complete stdout/stderr log for benchmark executed via `run_benchmarks.sh`. |
| `results/stats_<name>.log` | Machine-readable CSV statistics file containing IPC, cache hit/miss rates, ROB stalls, LSQ forwarding, and MMU/PTW level-by-level cache hit breakdown. |
