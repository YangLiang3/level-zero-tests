# ze_peer Design Document and Usage Guide

> Copyright (C) 2019-2026 Intel Corporation · SPDX-License-Identifier: MIT

---

## Table of Contents

1. [Tool Overview](#1-tool-overview)
2. [Code Structure](#2-code-structure)
3. [Test Mode Taxonomy](#3-test-mode-taxonomy)
4. [Architecture Design](#4-architecture-design)
   - 4.1 [Single-Process Direct P2P Test](#41-single-process-direct-p2p-test)
   - 4.2 [Same-Host IPC Test (--ipc)](#42-same-host-ipc-test---ipc)
   - 4.3 [Cross-Node MPI IPC Test (--ipc-mpi)](#43-cross-node-mpi-ipc-test---ipc-mpi)
   - 4.4 [Cross-Node Sequence Diagram (Latest Implementation)](#44-cross-node-sequence-diagram-latest-implementation)
5. [Device-Pair Generation Rules](#5-device-pair-generation-rules)
6. [Engine Selection Model](#6-engine-selection-model)
7. [Global State Flags](#7-global-state-flags)
8. [Build](#8-build)
9. [Parameter Reference](#9-parameter-reference)
10. [Usage Examples](#10-usage-examples)
    - 10.1 [Query Engines](#101-query-engines)
    - 10.2 [Single-Process P2P Test](#102-single-process-p2p-test)
    - 10.3 [Parallel Copy Test (Single Node)](#103-parallel-copy-test-single-node)
    - 10.4 [Same-Host IPC Test](#104-same-host-ipc-test)
    - 10.5 [Cross-Node MPI IPC Test](#105-cross-node-mpi-ipc-test)
11. [Limitations and Known Issues](#11-limitations-and-known-issues)

---

## 1. Tool Overview

`ze_peer` is a multi-GPU P2P (Peer-to-Peer) performance micro-benchmark based on the Intel Level Zero API. It is used to measure the following in multi-GPU systems:

- **Bandwidth**: Throughput (GB/s) across data sizes from 8 B to 256 MB
- **Latency**: Round-trip latency (us) for fixed-size transfers

It supports three major test scenarios:

| Scenario | Mechanism | Scope |
|------|------|----------|
| Single-process direct P2P | Direct Level Zero API access | Multi-GPU on the same host |
| Same-host IPC | Unix domain socket + `zeMemOpenIpcHandle` | Cross-process on the same host |
| Cross-node MPI IPC | `/dev/vmem` ioctl + `MPI_Sendrecv` + `zeMemOpenIpcHandle` | Across physical hosts |

---

## 2. Code Structure

```text
perf_tests/ze_peer/
├── CMakeLists.txt                       # Build config; auto-detects MPI and defines ZE_PEER_ENABLE_MPI
├── include/
│   └── ze_peer.h                        # Class declarations, enums, usage_str help text
├── src/
│   ├── ze_peer.cpp                      # main(), arg parsing, run_ipc_test(), run_test(), ZePeer ctor/dtor
│   ├── ze_peer_ipc.cpp                  # IPC path: same-host fork/socket + cross-node vmem/MPI exchange
│   ├── ze_peer_unidirectional.cpp       # Unidirectional bandwidth/latency implementation
│   ├── ze_peer_bidirectional.cpp        # Bidirectional bandwidth/latency implementation
│   ├── ze_peer_parallel_single_target.cpp   # Parallel multi-engine copy to single target
│   ├── ze_peer_parallel_multiple_targets.cpp # Parallel multi-engine copy to multiple targets
│   ├── ze_peer_parallel_pair_targets.cpp    # Pair-based parallel multi-engine copy
│   └── ze_peer_common.cpp               # Buffer init and common perf utilities
└── DESIGN.md                            # Chinese version
```

### Main Class

```text
ZePeer
├── static members (global option flags, shared after parsing)
│   ├── use_queue_in_destination
│   ├── bidirectional
│   ├── run_continuously
│   ├── validate_results
│   ├── parallel_copy_to_single_target
│   ├── parallel_copy_to_multiple_targets
│   ├── parallel_copy_to_pair_targets
│   ├── parallel_divide_buffers
│   ├── ipc_mpi_mode
│   └── number_iterations
├── instance members
│   ├── benchmark          -> ZeApp (Level Zero device/context management)
│   ├── ze_peer_devices[]  -> engines[] per device (command_queue + command_list pairs)
│   ├── ze_buffers[]       -> per-device buffer pointers
│   ├── ze_ipc_buffers[]   -> mapped remote buffers in IPC mode
│   └── queues             -> active engine index list
└── methods
    ├── bandwidth_latency()                    Unidirectional/bidirectional direct path
    ├── bandwidth_latency_ipc()                IPC path entry
    ├── bandwidth_latency_parallel_to_*()      Parallel path entries
    └── query_engines()                        Print engine table
```

---

## 3. Test Mode Taxonomy

```text
ze_peer test modes
│
├─ Single-process direct P2P (default)
│   ├─ Unidirectional (default)   bandwidth_latency()
│   └─ Bidirectional (-b)         bidirectional_bandwidth_latency()
│
├─ Parallel-engine P2P (single node)
│   ├─ --parallel_single_target    one src -> one dst, multi-engine shard copy
│   ├─ --parallel_multiple_targets one src -> many dst, one engine per dst
│   └─ --parallel_pair_targets     multiple (src:dst) pairs, one engine per pair
│
└─ IPC (cross-process / cross-node)
    ├─ --ipc                 same-host fork + Unix socket
    └─ --ipc-mpi             cross-node /dev/vmem + MPI_Sendrecv
```

---

## 4. Architecture Design

### 4.1 Single-Process Direct P2P Test

```text
┌──────────────────────────────────────────────────────┐
│                  Single process                      │
│                                                      │
│  ZeApp (Level Zero context)                          │
│  ┌──────────┐   ze_copy_engine   ┌──────────┐        │
│  │ Device A │ ─────────────────► │ Device B │        │
│  │ buffer   │  zeCommandListAppendMemoryCopy │       │
│  └──────────┘                   └──────────┘        │
└──────────────────────────────────────────────────────┘
```

- All devices are initialized in the same Level Zero context.
- Copy direction is controlled by `-o read/write`:
  - `write`: source engine pushes data to remote buffer
  - `read`: source engine pulls data from remote buffer
- Queue placement is controlled by `-x src/dst` (default: `src`).
- Performance metrics: `ze_event` timestamps are used to compute bandwidth (GB/s) or latency (us).

### 4.2 Same-Host IPC Test (`--ipc`)

```text
┌────────────────────────────────────────────────────────────────┐
│                        Parent process                          │
│  fork()                                                        │
│  ┌─────────────────────┐      socketpair()     ┌────────────┐  │
│  │ Child (client)      │ ◄──────────────────► │ Grandchild │  │
│  │ Device A (local)    │   sendmsg(fd)         │ (server)   │  │
│  │ bandwidth_latency_  │                       │ Device B   │  │
│  │ ipc(is_server=false)│                       │ Open IPC   │  │
│  └─────────────────────┘                       │ Handle     │  │
│                                                └────────────┘  │
└────────────────────────────────────────────────────────────────┘
```

**Flow:**

1. Parent creates `socketpair(PF_UNIX)`, then forks a child process.
2. Child forks again into a grandchild (server). They hold `sv[0]` and `sv[1]` respectively.
3. Server allocates buffer on its local device, calls `zeMemGetIpcHandle()`, and passes the fd via `sendmsg()`.
4. Client receives fd via `recvmsg_fd()`, calls `zeMemOpenIpcHandle()` to map remote buffer, then runs copy and timing.
5. A fresh process pair is forked for each transfer size to keep each measurement isolated.

### 4.3 Cross-Node MPI IPC Test (`--ipc-mpi`)

Requires `/dev/vmem` driver support. MPI mode is enabled automatically when MPI is found at build time.

Key points in the latest implementation (`src/ze_peer.cpp` + `src/ze_peer_ipc.cpp`):

1. Only **2 MPI ranks** are supported (`MPI_Comm_size == 2`), otherwise it fails fast.
2. Each pair uses an isolated tag space:
   - `pair_tag_base = 1024 + pair_index * 8`
   - `metadata_tag = pair_tag_base + 0`
   - `copy_done_tag = pair_tag_base + 1`
   - `cleanup_tag = pair_tag_base + 2`
3. Multi-pair concurrency uses multiple threads (one thread per pair), requiring `MPI_THREAD_MULTIPLE`.
4. For full-size sweep mode (when `-z` is not specified) with multi-pair concurrency, an in-process `MpiSizeStepBarrier` aligns each size step to prevent pair drift.
5. Current unidirectional behavior: **rank 1 executes copy, rank 0 only imports and synchronizes**. With `-b`, both ranks execute copy.

```text
Rank 0                                         Rank 1
┌─────────────────────────────────────────┐     ┌─────────────────────────────────────────┐
│ set_up_ipc(local_device=dst)            │     │ set_up_ipc(local_device=src)            │
│ open("/dev/vmem")                       │     │ open("/dev/vmem")                       │
│ zeMemGetIpcHandle(local buffer)         │     │ zeMemGetIpcHandle(local buffer)         │
│ VMEM_IOCTL_GET_PFN_LIST                 │     │ VMEM_IOCTL_GET_PFN_LIST                 │
│ MPI_Sendrecv(metadata_tag)  ◄──────────►│     │ MPI_Sendrecv(metadata_tag)              │
│ VMEM_IOCTL_GET_IPC_HANDLE               │     │ VMEM_IOCTL_GET_IPC_HANDLE               │
│ zeMemOpenIpcHandle(peer)                │     │ zeMemOpenIpcHandle(peer)                │
│                                          │     │ Unidirectional: execute copy            │
│ Unidirectional: no copy                  │     │ Bidirectional (-b): execute copy        │
│ Bidirectional (-b): execute copy         │     │                                         │
│ MPI_Sendrecv(copy_done_tag) ◄──────────►│     │ MPI_Sendrecv(copy_done_tag)             │
│ zeMemCloseIpcHandle(peer)               │     │ zeMemCloseIpcHandle(peer)               │
│ MPI_Sendrecv(cleanup_tag)   ◄──────────►│     │ MPI_Sendrecv(cleanup_tag)               │
└─────────────────────────────────────────┘     └─────────────────────────────────────────┘
```

**Key data structure:**

```c
struct mpi_exchange_data {
    int rank;
    int device_id;       // zeDeviceGetProperties().deviceId
    int dma_buf_fd;      // first 4 bytes of ze_ipc_mem_handle_t
    struct vmem_pfn_list pfn_list;  // physical addr+len, up to 8 PFN segments
};
```

**ioctl call chain (latest implementation):**

```text
On both ends:
  open("/dev/vmem", O_RDWR)
  zeMemGetIpcHandle() -> ze_ipc_mem_handle_t (first 4 bytes carry dma_buf_fd)
  VMEM_IOCTL_GET_PFN_LIST -> pfn_list (physical page list)
  (on failure with EBADF, retry up to 3 times)

On each rank (after receiving peer pfn_list):
  VMEM_IOCTL_GET_IPC_HANDLE -> reconstruct peer dma_buf_fd
  zeMemOpenIpcHandle() -> map into local virtual address space
```

**Rank roles:**

| Rank | Role | Notes |
|------|------|------|
| 0 / 1 | MPI endpoint | Device indices are selected by `-s/-d`; current unidirectional execution is on rank 1, both execute in `-b` mode |

> **Note**: `--ipc-mpi` does not use `MPI_Barrier`; it uses three-step `MPI_Sendrecv` synchronization (metadata / copy_done / cleanup).
>
> **Semantic convention**: `-s` / `-d` still denote source/destination device indices, but current code fixes unidirectional execution to rank 1.

### 4.4 Cross-Node Sequence Diagram (Latest Implementation)

The latest sequence diagram is in:

- `perf_tests/ze_peer/ze_peer_ipc_mpi_sequence.mmd`

---

## 5. Device-Pair Generation Rules

Under `--ipc-mpi`, the final execution list `ipc_mpi_pair_device_ids` is generated with the following precedence (higher priority overrides lower):

```text
Priority 1 (highest): --parallel_pair_targets <pairs>
  -> directly copy the explicit pair list into ipc_mpi_pair_device_ids
  -> clear parallel_copy_to_pair_targets flag (avoid local parallel-engine path)

Priority 2: --ipc-mpi-pairs <pairs> (compat option)
  -> directly set ipc_mpi_pair_device_ids

Priority 3: --parallel_single_target / --parallel_multiple_targets (topology selector semantics)
  --parallel_single_target:
    src = -s (default 0), dst = -d (default same index as src)
    generate 1 pair: [(src, dst)]
  --parallel_multiple_targets:
    src = first of -s (default 0)
    dst = all entries in -d (default = all local device indices)
    generate 1xN pairs: [(src, dst0), (src, dst1), ...]

Priority 4 (default): Cartesian product of -s / -d
  each local_id x remote_id -> ipc_mpi_pair_device_ids
```

Execution semantics: in `--ipc-mpi`, each pair in `ipc_mpi_pair_device_ids` runs concurrently (one execution thread per pair).

In **non-MPI modes**, `--parallel_single_target` / `--parallel_multiple_targets` / `--parallel_pair_targets` are true parallel-engine controls:

| Mode | Meaning |
|------|------|
| `--parallel_single_target` | Buffer is split by engine count, copied concurrently to one target |
| `--parallel_multiple_targets` | Dedicated buffer per target device, copied concurrently with one engine per target |
| `--parallel_pair_targets` | Dedicated buffer per (src:dst) pair, copied concurrently with one engine per pair |

---

## 6. Engine Selection Model

```text
Device
└── Command Queue Groups (enumerated via zeDeviceGetCommandQueueGroupProperties)
    ├── Group 0: numQueues=4, flags=COMPUTE|COPY
    │   └── Queue 0~3 -> engine index 0~3
    └── Group 1: numQueues=1, flags=COPY
        └── Queue 0 -> engine index 4
```

- `-q` prints all device engine tables; numbers in the `-u` column are valid indices for `-u`.
- If `-u` is not provided, the default is the first engine in a queue group with `COMPUTE` flag (index 0).
- `-u 0,1,2,3` means using 4 engines concurrently (effective in parallel modes only).
- IPC tests (`--ipc` / `--ipc-mpi`) always use `engines[0]` (single engine).

---

## 7. Global State Flags

All `ZePeer::` static members are globally read-only after argument parsing in `main()`, and shared by all instances.

| Static Member | Default | Description |
|---------|--------|------|
| `use_queue_in_destination` | `false` | True when `-x dst`; queue is placed on destination device |
| `run_continuously` | `false` | `-c`; run continuously until SIGINT |
| `bidirectional` | `false` | `-b`; bidirectional mode |
| `validate_results` | `false` | `-v`; verify copied data consistency |
| `parallel_copy_to_single_target` | `false` | `--parallel_single_target` |
| `parallel_copy_to_multiple_targets` | `false` | `--parallel_multiple_targets` |
| `parallel_copy_to_pair_targets` | `false` | `--parallel_pair_targets` (cleared after parsing in MPI mode) |
| `parallel_divide_buffers` | `false` | `--divide_buffers` |
| `ipc_mpi_mode` | `false` | `--ipc-mpi` |
| `number_iterations` | `50` | `-i <N>` |

---

## 8. Build

### Dependencies

| Dependency | Required | Purpose |
|------|---------|------|
| Intel Level Zero SDK | Required | GPU management and P2P copy |
| CMake >= 3.16 | Required | Build system |
| MPI (OpenMPI / IMPI recommended) | Optional | Enable cross-node `--ipc-mpi` path |
| `/dev/vmem` driver | Required for `--ipc-mpi` | PFN extraction and cross-node IPC handle reconstruction |

### Build Steps

```bash
# from level-zero-tests root
mkdir build && cd build

# without MPI
cmake .. -DCMAKE_BUILD_TYPE=Release
make ze_peer -j$(nproc)

# with MPI (auto-detected; can also be explicitly specified)
cmake .. -DCMAKE_BUILD_TYPE=Release -DMPI_CXX_COMPILER=$(which mpicxx)
make ze_peer -j$(nproc)
```

CMake auto-detection logic:

```cmake
find_package(MPI QUIET COMPONENTS CXX)
if(MPI_CXX_FOUND)
    target_compile_definitions(ze_peer PRIVATE ZE_PEER_ENABLE_MPI=1)
else()
    target_compile_definitions(ze_peer PRIVATE ZE_PEER_ENABLE_MPI=0)
endif()
```

---

## 9. Parameter Reference

### General Controls

| Parameter | Default | Description |
|------|--------|------|
| `-b` | off | Bidirectional mode (currently not supported by same-host `--ipc`) |
| `-c` | off | Continuous run until CTRL+C |
| `-i <N>` | 50 | Iterations per data size |
| `-z <bytes>` | full sweep 8->256M | Test one fixed size; max 268435456 (256 MB) |
| `-v` | off | Validate data; auto-sets iterations to 1 |
| `-t transfer_bw|latency` | run both | Select test type |
| `-o read|write` | run both | Select transfer direction |

### Engine and Device Selection

| Parameter | Description |
|------|------|
| `-q` | Print engine table and exit |
| `-u <idx[,idx...]>` | Select engines (comma-separated; for parallel modes) |
| `-s <id[,id...]>` | Source device list (comma-separated) |
| `-d <id[,id...]>` | Destination device list (comma-separated) |

### Test Modes

| Parameter | Scenario | Description |
|------|---------|------|
| *(no special flag)* | Single-process P2P | Default tests all src x dst pairs |
| `--parallel_single_target` | Single node | Buffer sharding, multi-engine concurrent copy to one target |
| `--parallel_multiple_targets` | Single node | Dedicated buffer per target, one engine per target |
| `--parallel_pair_targets [pairs]` | Single node / MPI topology selection | Explicit `src:dst,...` pairs, one engine per pair; acts as cross-node pair selector in `--ipc-mpi` mode |
| `--divide_buffers` | with `--parallel_multiple/pair_targets` | Split each buffer across engines |
| `-x src|dst` | Parallel modes | Place queue on source (default) or destination |
| `--ipc` | Same-host cross-process | fork + Unix socket IPC |
| `--ipc-mpi` | Cross-node | MPI + /dev/vmem IPC; requires exactly 2 MPI ranks |
| `--ipc-mpi-pairs <pairs>` | with `--ipc-mpi` | Compatibility option to explicitly set pairs; `--parallel_pair_targets` is preferred |

### Topology Selector Semantics in `--ipc-mpi`

| Parameter | Meaning under `--ipc-mpi` |
|------|---------------------|
| `-s A` `-d B` | Cartesian product pairs: `[(A,B)]` or multiple pairs |
| `--parallel_single_target -s A -d B` | Generate one pair `(A, B)`; only one source and one destination allowed |
| `--parallel_multiple_targets -s A -d B,C,D` | Generate `[(A,B),(A,C),(A,D)]`; default `-d` is all local devices |
| `--parallel_pair_targets A:B,C:D` | Generate `[(A,B),(C,D)]`; fully overrides `-s/-d` |

---

## 10. Usage Examples

### 10.1 Query Engines

```bash
./ze_peer -q
```

Sample output:

```text
====================================================
 Device 0  1400 MHz
----------------------------------------------------
| Group | Queues | Compute | Copy |  -u |
|  0    |   4    |    X    |   X  | 0~3 |
|  1    |   1    |         |   X  |  4  |
====================================================
```

---

### 10.2 Single-Process P2P Test

```bash
# Full default suite (all device pairs x both test types x both directions)
./ze_peer

# Bandwidth only, fixed at 256 MB
./ze_peer -t transfer_bw -z 268435456

# Read latency for device 1 -> device 3 at 64 B
./ze_peer -t latency -o read -z 64 -s 1 -d 3

# Bidirectional bandwidth, device 0 <-> device 1
./ze_peer -b -t transfer_bw -s 0 -d 1
```

---

### 10.3 Parallel Copy Test (Single Node)

#### `--parallel_single_target`: multi-engine shard copy to one target

```bash
# device 0 -> device 1, engines 2 and 3, total 256 MB (128 MB per engine)
./ze_peer --parallel_single_target -t transfer_bw -z 268435456 -s 0 -d 1 -u 2,3

# Place queues on destination side
./ze_peer --parallel_single_target -t transfer_bw -z 268435456 -s 0 -d 1 -u 2,3 -x dst
```

#### `--parallel_multiple_targets`: one source to multiple targets in parallel

```bash
# device 1 -> devices 2 and 3, using engines 0 and 1 respectively
./ze_peer --parallel_multiple_targets -t transfer_bw -z 268435456 -s 1 -d 2,3 -u 0,1

# Bidirectional
./ze_peer --parallel_multiple_targets -t transfer_bw -z 268435456 -s 1 -d 2,3 -u 0,1 -b

# --divide_buffers: each target buffer is also split across engines
./ze_peer --parallel_multiple_targets --divide_buffers -t transfer_bw -z 268435456 -s 0 -d 1,2 -u 0,1,2,3
```

#### `--parallel_pair_targets`: pairwise parallel mode

```bash
# Default pairs (device 0 -> all other devices), using all compute engines
./ze_peer --parallel_pair_targets -t transfer_bw -z 268435456

# Explicit pair list
./ze_peer --parallel_pair_targets 0:1,2:3 -t transfer_bw -z 268435456
```

---

### 10.4 Same-Host IPC Test

```bash
# Allocate on device 0, copy on device 1 (write), fixed 256 MB
./ze_peer --ipc -s 0 -d 1 -t transfer_bw -o write -z 268435456

# Full-size sweep with read operation
./ze_peer --ipc -s 0 -d 1 -t transfer_bw -o read
```

---

### 10.5 Cross-Node MPI IPC Test

#### Prerequisites

- `/dev/vmem` kernel module installed on both hosts
- MPI runtime available (`mpirun` / `mpiexec`)
- `ze_peer` built with `ZE_PEER_ENABLE_MPI=1`
- Inter-host GPU connectivity through PCIe switch or NVLink (P2P)

#### Basic 1:1

```bash
# 1:1 (-s 0 -> -d 0), write bandwidth
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0 -d 0 -t transfer_bw -o write -z 268435456
```

> Two MPI ranks run on hostA/hostB. `-s/-d` define source/destination GPU indices.
> They are not statically bound to rank 0/1 in CLI semantics.
> In unidirectional mode, destination metadata is imported to source side first,
> then source side initiates P2P copy.
> In bidirectional mode (`-b`), both sides import peer metadata and execute copy.

#### Parameter Variants

```bash
# 1:1 asymmetric: source-side GPU 1 -> destination-side GPU 0
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 1 -d 0 -t transfer_bw -o write

# 1:N: source-side GPU 0 against destination-side GPU 0,1,2 (Cartesian)
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0 -d 0,1,2 -t transfer_bw -o write

# N:1: multiple source GPUs against destination-side GPU 0
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0,1,2 -d 0 -t transfer_bw -o write

# N:N Cartesian product
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0,1 -d 2,3 -t transfer_bw -o write

# Bidirectional (-b) also applies to 1:1 / 1:N / N:1 / N:N
# Add -b to commands above so both ends execute copies.

# --parallel_single_target topology selector: one pair
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi --parallel_single_target -s 0 -d 0 -t transfer_bw -o write

# --parallel_multiple_targets topology selector: source-side GPU 0 -> all destination-side GPUs
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi --parallel_multiple_targets -s 0 -t transfer_bw -o write

# Explicit pair list (no Cartesian expansion)
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi --parallel_pair_targets 0:0,1:1,2:2 -t transfer_bw -o write

# Asymmetric explicit pairs
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi --parallel_pair_targets 0:2,1:3,2:1 -t transfer_bw -o write -z 268435456

# Latency test
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0 -d 0 -t latency -o write -z 64

# Full type/direction sweep (omit -t / -o)
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0 -d 0
```

#### Diagnostic Output

When successful, both sides print diagnostics (example from one side):

```text
MPI cross-node local rank=1, local_device_idx=0, local_device_id=0x1234, local_dma_buf_fd=5
MPI cross-node local PFN nents=1 [0:addr=0x7f00000000,len=268435456]
MPI cross-node received peer rank=0, remote_device_id=0x5678, remote_dma_buf_fd=6
MPI cross-node remote PFN nents=1 [0:addr=0x7e00000000,len=268435456]
MPI cross-node reconstructed remote dma_buf_fd=7 for local device idx 0
MPI cross-node opened remote handle on local device idx 0, mapped pointer=0x...
```

---

## 11. Limitations and Known Issues

| Limitation | Description |
|------|------|
| `--ipc-mpi` supports only 2 MPI ranks | `MPI_Sendrecv` uses fixed `peer_rank = 1 - mpi_rank` |
| Multi-pair concurrency requires `MPI_THREAD_MULTIPLE` | Otherwise exits with error at startup |
| Insufficient MPI tag space causes failure | `MPI_TAG_UB` is checked before run |
| `--ipc` does not support bidirectional mode (`-b`) | Bidirectional is only supported by `--ipc-mpi`, where both ends execute copy |
| Under `--ipc-mpi`, `--parallel_single/multiple_targets` are topology selectors, not true local engine-sharding controls | But generated pairs are executed concurrently (one thread per pair) |
| `--parallel_pair_targets` + `--ipc-mpi-pairs` cannot both provide explicit pair lists | Fails with `[ERROR] Do not use both...` |
| `/dev/vmem` supports at most 8 PFN segments | `vmem_pfn_list.nents` upper bound is 8 |
| IPC tests (`--ipc` and `--ipc-mpi`) always use `engines[0]` | Not affected by `-u` |
| Non-MPI `--parallel_*` tests are not supported with `--ipc` | Not explicitly blocked, behavior undefined |
