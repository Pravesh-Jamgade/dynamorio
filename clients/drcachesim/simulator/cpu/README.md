# Out-of-Order (OoO) CPU Core & 3-Level Cache Hierarchy Simulator (`drcachesim/simulator/cpu`)

## 1. Overview

This directory (`clients/drcachesim/simulator/cpu/`) provides a full cycle-level **Out-of-Order (OoO) superscalar CPU core pipeline**, **Front-End (BTB + Branch Predictor + RAS)**, **Reorder Buffer (ROB)**, **Register Dependency Scoreboard**, **Load-Store Queue (LSQ)** with **Store-to-Load Forwarding**, and a **Unified 3-Level Data Cache Hierarchy (`L1D -> L2 -> L3/LLC -> DRAM`)** through which both application demand accesses and hardware Page Table Walks (PTW on Page Walk Cache misses) flow.

---

## 2. Component Architecture

```mermaid
flowchart TD
    subgraph FrontEnd["Front-End"]
        Fetch["Instruction Fetch"] --> BTB["BTB & Branch Predictor (btb_t)\n- 2-bit Bimodal Saturating Counters\n- Branch Target Buffer\n- Return Address Stack (RAS)"]
        BTB -->|"Misprediction"| Squash["Pipeline Flush\n(Squash ROB, RegDeps, LSQ)"]
    end

    subgraph Dispatch["Dispatch & Renaming"]
        BTB --> Decode["Decode / Rename"]
        Decode --> DispatchUnit["Dispatch Stage"]
        DispatchUnit --> ROB["Reorder Buffer (rob_t)\n- 256 entries\n- In-Order Commit\n- Head-of-line stall modeling"]
        DispatchUnit --> Scoreboard["Register Scoreboard (register_scoreboard_t)\n- RAW data hazard tracking\n- Operand ready cycle calculation"]
        DispatchUnit --> LSQ["Load-Store Queue (lsq_t)\n- Load Queue (LQ: 72 entries)\n- Store Queue (SQ: 56 entries)"]
    end

    subgraph MemoryHierarchy["Memory Subsystem & 3-Level Cache (three_level_cache_t)"]
        LSQ -->|"Store Forwarding Hit"| FwdHit["1-Cycle Forwarding Bypass"]
        LSQ -->|"Forwarding Miss"| DTLB["L1 D-TLB / L2 TLB"]
        DTLB -->|"TLB Miss"| PTW["Hardware Page Table Walker (page_table_walker_t)"]
        
        LSQ -->|"Demand Load/Store"| L1D["L1 Data Cache (32KB, 8-way, 4 cycles)"]
        PTW -->|"8-byte PTE Read"| L1D
        
        L1D -->|"L1D Miss"| L2C["L2 Cache (256KB, 8-way, 12 cycles)"]
        L2C -->|"L2 Miss"| L3C["Shared L3/LLC (8MB, 16-way, 35 cycles)"]
        L3C -->|"L3 Miss"| DRAM["Main Memory DRAM (150 cycles)"]
    end

    FwdHit --> Writeback["Writeback Stage"]
    MemoryHierarchy --> Writeback
    Writeback -->|"Mark Ready"| Scoreboard
    Writeback -->|"Mark Ready"| ROB
    ROB -->|"In-Order Retire"| Commit["Commit Stage"]
    Commit -->|"Drain Committed Stores"| L1D
```

---

## 3. Class Hierarchy & Source Files

```text
clients/drcachesim/simulator/cpu/
├── cpu_types.h          # cpu_op_type_t, pipeline_stage_t, branch_prediction_t, dyn_instruction_t
├── three_level_cache.h  # 3-level data cache hierarchy declaration (L1D -> L2 -> L3/LLC)
├── three_level_cache.cpp# Cascading cache lookups, PTW PTE routing, and access latency evaluation
├── btb.h                # btb_t, return_address_stack_t, and branch_predictor_t declarations
├── btb.cpp              # 2-bit saturating counter branch direction and target prediction
├── reg_deps.h           # register_scoreboard_t declaration
├── reg_deps.cpp         # Register dependency tracking, RAW hazard detection, and writeback publishing
├── rob.h                # rob_t declaration
├── rob.cpp              # Reorder Buffer: circular FIFO, in-order commit, head-of-line stalls, squashes
├── lsq.h                # lsq_t declaration
├── lsq.cpp              # Load-Store Queue: store-to-load forwarding and store buffering until commit
├── wrong_path.h         # Speculative wrong-path execution engine declaration
├── wrong_path.cpp       # Wrong-path instruction stream synthesis, cache/TLB pollution, and squashes
├── cpu_core.h           # cpu_core_t declaration
├── cpu_core.cpp         # Complete out-of-order execution pipeline engine
├── cpu_simulator.h      # cpu_simulator_t declaration
├── cpu_simulator.cpp    # Multi-core CPU simulator tool
└── README.md            # Architecture documentation (this file)
```

---

## 4. Key Mechanisms

### 4.1 Store-to-Load Forwarding & Disambiguation (`lsq_t`)
- When a load executes, `lsq_t::check_store_forwarding()` scans the Store Queue backwards for preceding in-flight stores (`store.seq_id < load.seq_id`).
- If an exact address match occurs, the store forwards its data directly to the load in **1 cycle**, completely bypassing DTLB, L1D, and main memory!
- Stores buffer in the SQ and **only drain to L1D cache at in-order commit time**, preserving memory consistency.

### 4.2 Reorder Buffer Head-of-Line Stalls (`rob_t`)
- Instructions dispatch in program order into the ROB.
- Execution happens out-of-order as register and memory operands become ready.
- At commit time, retirement is strictly in program order from the head of the ROB.
- If the instruction at the head is a Load waiting on an L1D miss, L2 miss, or Page Table Walk, the ROB head blocks, preventing younger ready instructions from committing.

### 4.3 Unified 3-Level Cache Hierarchy (`three_level_cache_t`)
- **Split L1 Harvard Architecture**: Each core features a private **L1 Instruction Cache (`L1I`)** and private **L1 Data Cache (`L1D`)**.
- **Unified Lower Levels**: Both `L1I` and `L1D` miss into the core's private unified **L2 Cache** (`L1I.parent_ = L2`, `L1D.parent_ = L2`). All per-core L2 caches miss into the shared unified **L3 / LLC Cache** (`L2.parent_ = L3`), which connects to main memory DRAM (`L3.parent_ = nullptr`).
- **Instruction Fetch Stalls**: Instruction fetches flow through `L1I`. On an L1I miss, the CPU Front-End incurs fetch stall cycles (`icache_res.latency_cycles`) while the line is filled from L2, L3, or DRAM.
- **PTW and Demand Traffic**: Both demand memory accesses (`TRACE_TYPE_READ` / `TRACE_TYPE_WRITE`) and hardware Page Table Walker 8-byte PTE memory reads (`ptw_request()`) traverse the exact same cache sets in `L1D`, `L2`, and `L3`.
- Models cache contention and cache pollution caused by page table walks upon Page Walk Cache misses.

### 4.4 Speculative Wrong-Path Execution (`speculative_wrong_path_engine_t`)
- When a branch mispredicts in `cpu_core_t`, the processor does **not** simply stall idle.
- The pipeline fetches, decodes, and dispatches instructions along the **mispredicted path** (starting at the wrong target or fallthrough PC) for the duration of the branch resolution penalty (default: 14 cycles).
- **Buffer Occupancy**: Wrong-path instructions consume physical ROB slots, scoreboard registers, and Load/Store Queue entries.
- **Speculative Cache & TLB Pollution**: Speculative loads access L1D, L2, L3, and the DTLB. Cold cache lines are brought from DRAM into the hierarchy.
- **Architectural Store Safety**: Speculative stores buffer in the SQ, but **never commit to cache** because commit is strictly in program order.
- **Squash on Resolution**: When the branch resolves, `cpu_core_t` calls `rob_.squash_after()`, `scoreboard_.squash_after()`, and `lsq_.squash_after()`. All wrong-path instructions are discarded, but the **cache lines and TLB translations they loaded remain in hardware** (faithfully modeling Spectre-style speculative side effects).

### 4.5 ChampSim & CloudSuite Google Trace Reader (`champsim_trace_reader_t`)
- **Standard ChampSim Format (64-byte records)**:
  - Binary instruction record: `uint64_t ip`, `uint8_t is_branch`, `uint8_t branch_taken`, `uint8_t destination_registers[2]`, `uint8_t source_registers[4]`, `uint64_t destination_memory[2]`, `uint64_t source_memory[4]`.
  - Used by Google Datacenter benchmark callsets (e.g., `bc-0`, `bfs-10`, `cc-13`, `sssp-10`, `nutch_phase0_core1`).
- **CloudSuite Format (96-byte records)**:
  - Extended binary record with `destination_registers[4]`, `destination_memory[4]` (up to 4 memory stores per instruction), `source_memory[4]`, and `asid[2]` (Address Space Identifier).
  - Used by CloudSuite server and multi-process workloads.
- **Decompression**: Automatic streaming decompression via parallel pipelines for `.gz`, `.xz`, and `.bz2` traces, as well as uncompressed `.trace` binary files.
- **Trace Format Flags**:
  - `-champsim_format <default|standard|cloudsuite|auto>`: Explicit command-line flag indicating trace layout.
    - `default` / `standard` / `64`: Standard 64-byte ChampSim format (2 stores, 4 loads).
    - `cloudsuite` / `cs` / `96`: CloudSuite 96-byte format (4 stores, 4 loads, ASID).
    - `auto`: Heuristic auto-detection based on sequential instruction stride scoring.
  - `-is_cloudsuite`: Convenient boolean shortcut flag indicating CloudSuite 96-byte format.
- **DynamoRIO Unpacking**: Maps binary records into program-order `TRACE_TYPE_INSTR` fetch events, followed by all valid `TRACE_TYPE_READ` loads and `TRACE_TYPE_WRITE` stores, allowing seamless trace playback directly through `cpu_simulator_t::process_memref()`.

---

## 5. How to Run Complete Core Simulation

### 5.1 Using the Standalone Trace Runner (`bin/run_cpu_sim`)

The fastest way to simulate Google datacenter traces and ChampSim traces directly is using the standalone `run_cpu_sim` driver:

#### Compile the Driver:
```bash
mkdir -p bin
g++ -std=c++14 -O2 -DX86_64 -DLINUX -DARC_64 \
  -Iclients/drcachesim -Iclients/drcachesim/common -Iclients/drcachesim/reader \
  -Iclients/drcachesim/simulator -Iclients/drcachesim/tests -Iext/droption \
  clients/drcachesim/common/trace_entry.cpp \
  clients/drcachesim/simulator/tlb.cpp \
  clients/drcachesim/simulator/tlb_simulator.cpp \
  clients/drcachesim/simulator/simulator.cpp \
  clients/drcachesim/simulator/caching_device.cpp \
  clients/drcachesim/simulator/caching_device_stats.cpp \
  clients/drcachesim/simulator/cache.cpp \
  clients/drcachesim/simulator/cache_lru.cpp \
  clients/drcachesim/simulator/cache_fifo.cpp \
  clients/drcachesim/simulator/cache_stats.cpp \
  clients/drcachesim/simulator/prefetcher.cpp \
  clients/drcachesim/simulator/snoop_filter.cpp \
  clients/drcachesim/simulator/mmu/physical_frame_allocator.cpp \
  clients/drcachesim/simulator/mmu/huge_page_detector.cpp \
  clients/drcachesim/simulator/mmu/page_table.cpp \
  clients/drcachesim/simulator/mmu/page_walk_cache.cpp \
  clients/drcachesim/simulator/mmu/mmu_stats.cpp \
  clients/drcachesim/simulator/mmu/page_table_walker.cpp \
  clients/drcachesim/simulator/mmu/performance_model.cpp \
  clients/drcachesim/simulator/cpu/three_level_cache.cpp \
  clients/drcachesim/simulator/cpu/btb.cpp \
  clients/drcachesim/simulator/cpu/reg_deps.cpp \
  clients/drcachesim/simulator/cpu/rob.cpp \
  clients/drcachesim/simulator/cpu/lsq.cpp \
  clients/drcachesim/simulator/cpu/wrong_path.cpp \
  clients/drcachesim/simulator/cpu/cpu_core.cpp \
  clients/drcachesim/simulator/cpu/cpu_simulator.cpp \
  clients/drcachesim/reader/v2p_reader.cpp \
  clients/drcachesim/reader/champsim_reader.cpp \
  clients/drcachesim/simulator/cpu/run_cpu_sim.cpp \
  -o bin/run_cpu_sim
```

#### Run a Standard ChampSim Trace (64-byte records):
```bash
./bin/run_cpu_sim \
  --trace /media/pravesh/Storage/ALL_TRACES/bc-0.trace.gz \
  --format standard \
  --max_instrs 1000000 \
  --rob_size 256 \
  --lq_size 72 \
  --sq_size 56
```

#### Run a CloudSuite Trace (96-byte records, 4 stores, ASID):
```bash
./bin/run_cpu_sim \
  --trace /media/pravesh/Storage/ALL_TRACES/nutch_phase0_core1.trace.xz \
  --format cloudsuite \
  --max_instrs 1000000 \
  --cores 1
```

#### Run with Automatic Trace Detection:
```bash
./bin/run_cpu_sim \
  --trace /media/pravesh/Storage/ALL_TRACES/llama2.c-stories110M.1.champsimtrace.gz \
  --max_instrs 500000
```

---

### 5.2 Online Simulation on Live Applications (`drrun`)

To trace and simulate a live Linux binary online using DynamoRIO:

```bash
# 1. Collect offline trace of application with physical addresses:
bin64/drrun -t drcachesim -offline -record_replace_retaddr -use_physical \
  -outdir /tmp/my_trace -- ./my_application arg1 arg2

# 2. Simulate complete OoO CPU core and memory hierarchy:
bin64/drrun -t drcachesim \
  -indir /tmp/my_trace/drmemtrace.my_application.*.dir \
  -v2p_file /tmp/my_trace/drmemtrace.my_application.*.dir/v2p.log \
  -TLB_enable_ptw true \
  -TLB_enable_huge_pages true \
  -TLB_pwc_entries 32 \
  -TLB_couple_ptw_dcache true \
  -TLB_enable_performance_model true
```

---

### 5.3 Programmatic C++ Simulation Driver

You can also embed the complete simulator in any C++ tool or benchmark suite:

```cpp
#include "cpu/cpu_simulator.h"
#include "reader/champsim_reader.h"
#include <iostream>

using namespace dynamorio::drmemtrace;

int main() {
    // 1. Configure CPU pipeline and cache hierarchy knobs:
    cpu_simulator_knobs_t knobs;
    knobs.num_cores = 1;
    knobs.fetch_width = 4;
    knobs.commit_width = 4;
    knobs.rob_capacity = 256;
    knobs.lq_capacity = 72;
    knobs.sq_capacity = 56;
    knobs.enable_wrong_path = true;
    knobs.branch_mispredict_penalty = 14;
    knobs.l1i_size = 32 * 1024; // 32 KB private L1I
    knobs.l1d_size = 32 * 1024; // 32 KB private L1D
    knobs.l2_size = 256 * 1024; // 256 KB private L2
    knobs.l3_size = 8 * 1024 * 1024; // 8 MB shared L3

    // 2. Instantiate simulator:
    cpu_simulator_t sim(knobs);

    // 3. Open trace (auto-detects .gz/.xz compression and 64B/96B layout):
    champsim_trace_reader_t reader(
        "/media/pravesh/Storage/ALL_TRACES/nutch_phase0_core1.trace.xz",
        champsim_trace_format_t::FORMAT_CLOUDSUITE);
    
    if (!reader.open()) {
        std::cerr << "Failed to open trace\n";
        return 1;
    }

    // 4. Run 1,000,000 instructions:
    reader.run_simulation(sim, /*max_instructions=*/1000000, /*print_interval=*/100000);

    // 5. Print comprehensive statistics:
    sim.print_results();
    return 0;
}
```

---

## 6. Simulation Knobs & Options Reference

| Option | Flag in `run_cpu_sim` | drcachesim Flag | Default | Description |
|---|---|---|---|---|
| Trace Path | `--trace <path>` | `-champsim_trace` | `""` | Path to trace file (`.trace`, `.gz`, `.xz`). |
| Trace Format | `--format <fmt>` | `-champsim_format` | `auto` | `default`, `standard`, `cloudsuite`, or `auto`. |
| CloudSuite Shortcut | `--is_cloudsuite` | `-is_cloudsuite` | `false` | Force 96-byte CloudSuite record layout. |
| Max Instructions | `--max_instrs <N>` | `-max_instructions` | `1000000` | Stop simulation after N instructions (0 = all). |
| Cores | `--cores <N>` | `-num_cores` | `1` | Number of independent CPU cores. |
| ROB Size | `--rob_size <N>` | `-rob_size` | `256` | Reorder Buffer capacity in entries. |
| Load Queue Size | `--lq_size <N>` | `-lq_size` | `72` | Load Queue capacity in entries. |
| Store Queue Size | `--sq_size <N>` | `-sq_size` | `56` | Store Queue capacity in entries. |
| BTB Size | `--btb_entries <N>` | `-btb_entries` | `4096` | Branch Target Buffer entries. |
| Mispredict Penalty | `--mispredict_penalty <N>` | `-branch_penalty` | `14` | Flush penalty cycles for branch mispredicts. |
| Wrong-Path Engine | `--enable_wrong_path <0\|1>`| `-enable_wrong_path` | `1` | Enable speculative wrong-path execution. |
| Speculative Depth | `--speculative_depth <N>`| `-speculative_depth` | `32` | Maximum wrong-path instructions fetched. |
| L1I Cache Size | `--l1i_size <bytes>` | `-L1I_size` | `32768` (32KB)| Private L1 Instruction Cache size. |
| L1D Cache Size | `--l1d_size <bytes>` | `-L1D_size` | `32768` (32KB)| Private L1 Data Cache size. |
| L2 Cache Size | `--l2_size <bytes>` | `-LL_size` | `262144` (256KB)| Private unified L2 Cache size. |
| L3 / LLC Size | `--l3_size <bytes>` | `-L3_size` | `8388608` (8MB)| Shared unified Last-Level Cache size. |
| PWC Entries | `--pwc_entries <N>` | `-TLB_pwc_entries` | `32` | Page Walk Cache entries per core. |

---

## 7. Understanding Simulation Statistics

When simulation finishes, `print_results()` displays a full breakdown across four core dimensions:

```text
===================================================================
       OUT-OF-ORDER CPU, MMU & 3-LEVEL CACHE SIMULATION RESULTS     
===================================================================
  Core #0 Out-of-Order CPU Pipeline stats:
    Total instructions fetched:            5000
    Total instructions retired:            6739
    Total loads:                           1355
    Total stores:                           384
    Branch squash penalty cycles:           798
    L1I cache miss stall cycles:          15048
    Total execution cycles:               22637
    Overall CPI:                         3.3591
    Overall IPC:                         0.2977
```
1. **Total execution cycles & IPC**:
   - Accurately tracks out-of-order execution overlap, structural stalls, head-of-line commit stalls, L1I fetch stalls, and branch misprediction flushes.
2. **Branch Predictor & BTB**:
   - Direction predictions via 2-bit bimodal saturating counters, target predictions via BTB, subroutine call/return pairing via RAS.
3. **Reorder Buffer (ROB)**:
   - Tracks dispatch saturation (`ROB full dispatch stalls`), in-order commits, and `Head-of-line commit stalls` (younger ready instructions blocked waiting for an older load to return from cache/memory).
4. **Load-Store Queue (LSQ)**:
   - Disambiguates memory operations, buffers speculative stores, and counts `Store-to-Load forwardings` (where load retrieves data from an in-flight store in 1 cycle, bypassing cache and TLB entirely).
5. **Speculative Wrong-Path Execution**:
   - Shows instructions fetched along the wrong path upon misprediction, speculative loads/stores issued, and **speculative cache and L1I pollution** (lines brought into cache before the branch was resolved and squashed).
6. **3-Level Cache Hierarchy**:
   - Hits, misses, compulsory misses, and local/total miss rates across `L1I`, `L1D`, `L2`, and shared `L3/LLC`.
7. **MMU, PTW & Page Walk Cache**:
   - Total 4KB / 2MB / 1GB page table walks, demand page faults, PWC hits/misses, and PTW memory traffic hitting or missing in the data cache hierarchy.

