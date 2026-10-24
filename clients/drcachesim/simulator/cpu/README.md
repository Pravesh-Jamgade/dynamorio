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
- Chains `L1D.parent_ = L2`, `L2.parent_ = L3`, `L3.parent_ = nullptr` (DRAM).
- Both demand memory accesses (`TRACE_TYPE_READ` / `TRACE_TYPE_WRITE`) and hardware Page Table Walker 8-byte PTE memory reads (`ptw_request()`) traverse the exact same cache sets.
- Models cache contention and cache pollution caused by page table walks upon Page Walk Cache misses.
