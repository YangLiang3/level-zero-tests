# ze_peer 设计文档与使用说明

> Copyright (C) 2019–2026 Intel Corporation · SPDX-License-Identifier: MIT

---

## 目录

1. [工具概述](#1-工具概述)
2. [代码结构](#2-代码结构)
3. [测试模式分类](#3-测试模式分类)
4. [架构设计](#4-架构设计)
  - 4.1 [单进程直连 P2P 测试](#41-单进程直连-p2p-测试)
  - 4.2 [同主机 IPC 测试（--ipc）](#42-同主机-ipc-测试ipc)
  - 4.3 [跨节点 MPI IPC 测试（--ipc-mpi）](#43-跨节点-mpi-ipc-测试ipc-mpi)
  - 4.4 [跨节点时序图（最新实现）](#44-跨节点时序图最新实现)
5. [设备对生成规则](#5-设备对生成规则)
6. [引擎（Engine）选择模型](#6-引擎engine选择模型)
7. [全局状态标志说明](#7-全局状态标志说明)
8. [编译](#8-编译)
9. [参数参考](#9-参数参考)
10. [使用场景示例](#10-使用场景示例)
    - 10.1 [查询引擎](#101-查询引擎)
    - 10.2 [单进程 P2P 测试](#102-单进程-p2p-测试)
    - 10.3 [并行拷贝测试（单节点）](#103-并行拷贝测试单节点)
    - 10.4 [同主机 IPC 测试](#104-同主机-ipc-测试)
    - 10.5 [跨节点 MPI IPC 测试](#105-跨节点-mpi-ipc-测试)
11. [限制与已知问题](#11-限制与已知问题)

---

## 1. 工具概述

`ze_peer` 是基于 Intel Level Zero API 的多 GPU P2P（Peer-to-Peer）性能微基准测试工具，用于在多 GPU 系统中测量：

- **带宽（Bandwidth）**：连续传输不同尺寸数据（8 B → 256 MB）时的吞吐量（GB/s）
- **延迟（Latency）**：单次传输一个固定大小数据的往返时延（μs）

支持三大测试场景：

| 场景 | 机制 | 适用范围 |
|------|------|----------|
| 单进程直连 P2P | Level Zero API 直接访问 | 同主机多 GPU |
| 同主机 IPC | Unix 域套接字 + `zeMemOpenIpcHandle` | 同主机跨进程 |
| 跨节点 MPI IPC | `/dev/vmem` ioctl + `MPI_Sendrecv` + `zeMemOpenIpcHandle` | 跨物理主机 |

---

## 2. 代码结构

```
perf_tests/ze_peer/
├── CMakeLists.txt                       # 构建配置；自动检测 MPI 并定义 ZE_PEER_ENABLE_MPI
├── include/
│   └── ze_peer.h                        # 类声明、枚举、usage_str 帮助文本
├── src/
│   ├── ze_peer.cpp                      # main()、参数解析、run_ipc_test()、run_test()、ZePeer 构造/析构
│   ├── ze_peer_ipc.cpp                  # IPC 路径：同主机 fork/socket + 跨节点 vmem/MPI exchange
│   ├── ze_peer_unidirectional.cpp       # 单向带宽/延迟实现
│   ├── ze_peer_bidirectional.cpp        # 双向带宽/延迟实现
│   ├── ze_peer_parallel_single_target.cpp   # 单目标并行引擎拷贝
│   ├── ze_peer_parallel_multiple_targets.cpp # 多目标并行引擎拷贝
│   ├── ze_peer_parallel_pair_targets.cpp    # 成对并行引擎拷贝
│   └── ze_peer_common.cpp               # buffer 初始化、性能计算公用函数
└── DESIGN.md                            # 本文档
```

### 主要类

```
ZePeer
├── static 成员（全局选项标志，解析后共享）
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
├── 实例成员
│   ├── benchmark          → ZeApp（Level Zero 设备/上下文管理）
│   ├── ze_peer_devices[]  → 每个设备的 engines[]（command_queue + command_list 对）
│   ├── ze_buffers[]       → 每个设备的 device buffer 指针
│   ├── ze_ipc_buffers[]   → IPC 模式下映射的远端 buffer 指针
│   └── queues             → 当前激活的引擎索引列表
└── 方法
    ├── bandwidth_latency()                    单向/双向直连
    ├── bandwidth_latency_ipc()                IPC 路径入口
    ├── bandwidth_latency_parallel_to_*()      并行路径入口
    └── query_engines()                        打印设备引擎表
```

---

## 3. 测试模式分类

```
ze_peer 测试模式
│
├─ 单进程直连 P2P（默认）
│   ├─ 单向（默认）          bandwidth_latency()
│   └─ 双向（-b）           bidirectional_bandwidth_latency()
│
├─ 并行引擎 P2P（单节点）
│   ├─ --parallel_single_target    一个 src → 一个 dst，多引擎并行分片
│   ├─ --parallel_multiple_targets 一个 src → 多个 dst，每 dst 独占一个引擎
│   └─ --parallel_pair_targets     若干 (src:dst) 对，每对独占一个引擎
│
└─ IPC（跨进程 / 跨节点）
    ├─ --ipc                 同主机 fork + Unix socket
    └─ --ipc-mpi             跨节点 /dev/vmem + MPI_Sendrecv
```

---

## 4. 架构设计

### 4.1 单进程直连 P2P 测试

```
┌──────────────────────────────────────────────────────┐
│                    进程（单个）                         │
│                                                      │
│  ZeApp (Level Zero context)                          │
│  ┌──────────┐   ze_copy_engine   ┌──────────┐        │
│  │ Device A │ ─────────────────► │ Device B │        │
│  │ buffer   │  zeCommandListAppendMemoryCopy │        │
│  └──────────┘                   └──────────┘        │
└──────────────────────────────────────────────────────┘
```

- 所有设备在同一 Level Zero context 中初始化。
- 拷贝方向由 `-o read/write` 指定：
  - `write`：source engine 将数据推送到 remote buffer
  - `read`：source engine 从 remote buffer 拉取数据
- 队列位置由 `-x src/dst` 指定（默认 `src`）。
- 性能计算：提交命令时用 `ze_event` 记录时间戳，计算带宽（GB/s）或延迟（μs）。

### 4.2 同主机 IPC 测试（`--ipc`）

```
┌────────────────────────────────────────────────────────────────┐
│                         主进程                                   │
│  fork()                                                        │
│  ┌─────────────────────┐      socketpair()     ┌────────────┐  │
│  │  子进程（client）     │ ◄──────────────────► │ 孙进程(srv)│  │
│  │  Device A（local）   │   sendmsg(fd)         │ Device B   │  │
│  │  bandwidth_latency_  │                      │ Open IPC   │  │
│  │  ipc(is_server=false)│                      │ Handle     │  │
│  └─────────────────────┘                      └────────────┘  │
└────────────────────────────────────────────────────────────────┘
```

**流程：**

1. 主进程创建 `socketpair(PF_UNIX)`，然后 `fork()` 出子进程。
2. 子进程再次 `fork()` 出孙进程（server 进程），两者分别持有 `sv[0]` 和 `sv[1]`。
3. Server 端：在本设备分配 buffer，调用 `zeMemGetIpcHandle()` 获取 IPC handle，通过 `sendmsg()` 将 fd 传递给 client 端。
4. Client 端：通过 `recvmsg_fd()` 接收 fd，调用 `zeMemOpenIpcHandle()` 映射远端 buffer，执行拷贝并计时。
5. 每个数据尺寸均 fork 一次新的进程对（确保每次测量相互独立）。

### 4.3 跨节点 MPI IPC 测试（`--ipc-mpi`）

需要 `/dev/vmem` 驱动支持，编译时检测到 MPI 会自动启用。

最新实现（`src/ze_peer.cpp` + `src/ze_peer_ipc.cpp`）的关键点：

1. 仅支持 **2 个 MPI rank**（`MPI_Comm_size == 2`），否则立即报错。
2. 每个 pair 通过独立 tag 空间通信：
  - `pair_tag_base = 1024 + pair_index * 8`
  - `metadata_tag = pair_tag_base + 0`
  - `copy_done_tag = pair_tag_base + 1`
  - `cleanup_tag = pair_tag_base + 2`
3. 多 pair 并发时会创建多线程（每 pair 一个线程）；要求 MPI 提供 `MPI_THREAD_MULTIPLE`。
4. 若执行全尺寸扫描（`-z` 未指定）且多 pair 并发，使用进程内 `MpiSizeStepBarrier` 对齐每个 size 步进，避免 pair 间尺寸漂移。
5. 当前代码中的单向执行侧判定为：**rank 1 执行 copy，rank 0 仅导入并参与同步**；`-b` 时两侧都执行 copy。

```
Rank 0                                         Rank 1
┌─────────────────────────────────────────┐     ┌─────────────────────────────────────────┐
│ set_up_ipc(local_device=dst)            │     │ set_up_ipc(local_device=src)            │
│ open("/dev/vmem")                       │     │ open("/dev/vmem")                       │
│ zeMemGetIpcHandle(local buffer)         │     │ zeMemGetIpcHandle(local buffer)         │
│ VMEM_IOCTL_GET_PFN_LIST                 │     │ VMEM_IOCTL_GET_PFN_LIST                 │
│ MPI_Sendrecv(metadata_tag)  ◄──────────►│     │ MPI_Sendrecv(metadata_tag)              │
│ VMEM_IOCTL_GET_IPC_HANDLE               │     │ VMEM_IOCTL_GET_IPC_HANDLE               │
│ zeMemOpenIpcHandle(peer)                │     │ zeMemOpenIpcHandle(peer)                │
│                                          │     │ 单向: 执行 copy                          │
│ 单向: 不执行 copy                        │     │ 双向(-b): 执行 copy                     │
│ 双向(-b): 执行 copy                      │     │                                         │
│ MPI_Sendrecv(copy_done_tag) ◄──────────►│     │ MPI_Sendrecv(copy_done_tag)             │
│ zeMemCloseIpcHandle(peer)               │     │ zeMemCloseIpcHandle(peer)               │
│ MPI_Sendrecv(cleanup_tag)   ◄──────────►│     │ MPI_Sendrecv(cleanup_tag)               │
└─────────────────────────────────────────┘     └─────────────────────────────────────────┘
```

**关键数据结构：**

```c
struct mpi_exchange_data {
    int rank;
    int device_id;       // zeDeviceGetProperties().deviceId
    int dma_buf_fd;      // ze_ipc_mem_handle_t 的前 4 字节
    struct vmem_pfn_list pfn_list;  // 最多 8 个 PFN 段的物理地址+长度
};
```

**ioctl 调用链（最新实现）：**

```
两端各自：
  open("/dev/vmem", O_RDWR)
  zeMemGetIpcHandle() → ze_ipc_mem_handle_t (前 4 字节承载 dma_buf_fd)
  VMEM_IOCTL_GET_PFN_LIST → pfn_list（物理页号列表）
  （失败时对 EBADF 最多重试 3 次）

每个 rank（在收到对端 pfn_list 之后）：
  VMEM_IOCTL_GET_IPC_HANDLE → 重建对端 dma_buf_fd
  zeMemOpenIpcHandle() → 映射到本地虚地址空间
```

**Rank 角色：**

| Rank | 角色 | 说明 |
|------|------|------|
| 0 / 1 | MPI 端点 | 设备索引由 `-s/-d` 指定；当前实现中单向由 rank1 执行，`-b` 时两侧执行 |

> **注意**：`--ipc-mpi` 中并不使用 `MPI_Barrier`，而是使用 `MPI_Sendrecv` 三段同步（metadata / copy_done / cleanup）。
>
> **语义约定**：`-s`/`-d` 仍表示源/目的设备索引；但当前代码将单向执行侧固定在 rank1。

### 4.4 跨节点时序图（最新实现）

最新时序图见：

- `perf_tests/ze_peer/ze_peer_ipc_mpi_sequence.mmd`

---

## 5. 设备对生成规则

`--ipc-mpi` 模式下，最终执行列表 `ipc_mpi_pair_device_ids` 的生成优先级（高优先级覆盖低优先级）：

```
优先级 1（最高）：--parallel_pair_targets <pairs>
  → 直接把 pairs 列表复制为 ipc_mpi_pair_device_ids
  → 清除 parallel_copy_to_pair_targets 标志（不触发本地并行路径）

优先级 2：--ipc-mpi-pairs <pairs>（兼容选项）
  → 直接设置 ipc_mpi_pair_device_ids

优先级 3：--parallel_single_target / --parallel_multiple_targets（拓扑选择器语义）
  --parallel_single_target:
    src = -s（默认 0），dst = -d（默认与 src 相同索引）
    生成 1 对：[(src, dst)]
  --parallel_multiple_targets:
    src = -s 的第一个（默认 0）
    dst = -d 所有（默认 = 本地全部设备索引）
    生成 1×N 对：[(src, dst0), (src, dst1), ...]

优先级 4（默认）：-s / -d
  每个 local_id × remote_id → ipc_mpi_pair_device_ids
```

执行语义：在 `--ipc-mpi` 下，`ipc_mpi_pair_device_ids` 中的每个 pair 会并发执行（每 pair 一个执行线程）。

**非 MPI 模式**下，`--parallel_single_target` / `--parallel_multiple_targets` / `--parallel_pair_targets` 是真正的并行引擎控制：

| 模式 | 含义 |
|------|------|
| `--parallel_single_target` | buffer 按引擎数均分，多引擎同时拷贝到同一目标 |
| `--parallel_multiple_targets` | 为每个目标设备分配独立 buffer，每个目标用一个引擎并发拷贝 |
| `--parallel_pair_targets` | 每对 (src:dst) 分配独立 buffer，每对用一个引擎并发拷贝 |

---

## 6. 引擎（Engine）选择模型

```
Device
└── Command Queue Groups (通过 zeDeviceGetCommandQueueGroupProperties 枚举)
    ├── Group 0: numQueues=4, flags=COMPUTE|COPY
    │   └── Queue 0~3 → engine index 0~3
    └── Group 1: numQueues=1, flags=COPY
        └── Queue 0 → engine index 4
```

- `-q` 命令打印所有设备的引擎表，`-u` 列的数字即为传给 `-u` 的 engine index。
- 不指定 `-u` 时，默认选取所有 `COMPUTE` 标志的引擎组中的第一个引擎（index 0）。
- `-u 0,1,2,3` 表示同时使用 4 个引擎（仅对并行模式有效）。
- IPC 测试（`--ipc` / `--ipc-mpi`）始终使用 `engines[0]`（单引擎）。

---

## 7. 全局状态标志说明

所有 `ZePeer::` 静态成员在 `main()` 解析参数后是全局只读的，被所有实例共享。

| 静态成员 | 默认值 | 说明 |
|---------|--------|------|
| `use_queue_in_destination` | `false` | `-x dst` 时为 true；队列放在目标设备 |
| `run_continuously` | `false` | `-c`；循环运行直到 SIGINT |
| `bidirectional` | `false` | `-b`；双向模式 |
| `validate_results` | `false` | `-v`；拷贝后校验数据一致性 |
| `parallel_copy_to_single_target` | `false` | `--parallel_single_target` |
| `parallel_copy_to_multiple_targets` | `false` | `--parallel_multiple_targets` |
| `parallel_copy_to_pair_targets` | `false` | `--parallel_pair_targets`（MPI 模式下解析后会被清除） |
| `parallel_divide_buffers` | `false` | `--divide_buffers` |
| `ipc_mpi_mode` | `false` | `--ipc-mpi` |
| `number_iterations` | `50` | `-i <N>` |

---

## 8. 编译

### 依赖

| 依赖 | 是否必须 | 用途 |
|------|---------|------|
| Intel Level Zero SDK | 必须 | GPU 管理与 P2P 拷贝 |
| CMake ≥ 3.16 | 必须 | 构建系统 |
| MPI (推荐 OpenMPI / IMPI) | 可选 | 启用 `--ipc-mpi` 跨节点路径 |
| `/dev/vmem` 驱动 | `--ipc-mpi` 时必须 | 物理页号（PFN）和跨节点 IPC handle 重建 |

### 构建步骤

```bash
# 在 level-zero-tests 根目录
mkdir build && cd build

# 不带 MPI
cmake .. -DCMAKE_BUILD_TYPE=Release
make ze_peer -j$(nproc)

# 带 MPI（自动检测；也可手动指定）
cmake .. -DCMAKE_BUILD_TYPE=Release -DMPI_CXX_COMPILER=$(which mpicxx)
make ze_peer -j$(nproc)
```

CMakeLists.txt 自动逻辑：
```cmake
find_package(MPI QUIET COMPONENTS CXX)
if(MPI_CXX_FOUND)
    target_compile_definitions(ze_peer PRIVATE ZE_PEER_ENABLE_MPI=1)
else()
    target_compile_definitions(ze_peer PRIVATE ZE_PEER_ENABLE_MPI=0)
endif()
```

---

## 9. 参数参考

### 通用控制

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-b` | off | 双向模式（IPC 路径暂不支持） |
| `-c` | off | 持续运行，CTRL+C 退出 |
| `-i <N>` | 50 | 每个尺寸的测量迭代次数 |
| `-z <bytes>` | 8→256M 全扫描 | 固定测试单个尺寸；最大 268435456（256 MB） |
| `-v` | off | 校验数据，自动将迭代次数设为 1 |
| `-t transfer_bw\|latency` | 两者都跑 | 指定测试类型 |
| `-o read\|write` | 两者都跑 | 指定传输方向 |

### 引擎与设备选择

| 参数 | 说明 |
|------|------|
| `-q` | 打印引擎表后退出 |
| `-u <idx[,idx...]>` | 选择引擎（逗号分隔多个，用于并行模式） |
| `-s <id[,id...]>` | 源设备列表（逗号分隔） |
| `-d <id[,id...]>` | 目标设备列表（逗号分隔） |

### 测试模式

| 参数 | 适用场景 | 说明 |
|------|---------|------|
| *(无特殊标志)* | 单进程 P2P | 默认逐对测试所有 src×dst 设备组合 |
| `--parallel_single_target` | 单节点 | buffer 分片，多引擎并发拷到单目标 |
| `--parallel_multiple_targets` | 单节点 | 每目标独立 buffer，各自用独立引擎并行拷贝 |
| `--parallel_pair_targets [pairs]` | 单节点 / MPI 拓扑选择 | 按 `src:dst,...` 对列表，每对独立引擎；在 `--ipc-mpi` 模式下用作跨节点设备对选择器 |
| `--divide_buffers` | 与 `--parallel_multiple/pair_targets` 组合 | buffer 在引擎间均分 |
| `-x src\|dst` | 并行模式 | 队列放在源端（默认）或目标端 |
| `--ipc` | 同主机跨进程 | fork + Unix socket IPC |
| `--ipc-mpi` | 跨节点 | MPI + /dev/vmem IPC；需要恰好 2 个 MPI rank |
| `--ipc-mpi-pairs <pairs>` | `--ipc-mpi` 配合使用 | 兼容选项，显式指定设备对；推荐用 `--parallel_pair_targets` 代替 |

### `--ipc-mpi` 模式下的拓扑选择器语义

| 参数 | `--ipc-mpi` 下的含义 |
|------|---------------------|
| `-s A` `-d B` | `[(A,B)]` 或多对 |
| `--parallel_single_target -s A -d B` | 生成 1 对 `(A, B)`；仅允许各一个设备 |
| `--parallel_multiple_targets -s A -d B,C,D` | 生成 `[(A,B),(A,C),(A,D)]`；-d 默认 = 全部本地设备 |
| `--parallel_pair_targets A:B,C:D` | 生成 `[(A,B),(C,D)]`；完全覆盖 -s/-d |

---

## 10. 使用场景示例

### 10.1 查询引擎

```bash
./ze_peer -q
```

输出示例：
```
====================================================
 Device 0  1400 MHz
----------------------------------------------------
| Group | Queues | Compute | Copy |  -u |
|  0    |   4    |    X    |   X  |  0  |
|  1    |   1    |         |   X  |  1  |
====================================================
```

---

### 10.2 单进程 P2P 测试

```bash
# 全量默认测试（所有设备对 × 两种测试类型 × 两个方向）
./ze_peer

# 仅测带宽，固定 256 MB
./ze_peer -t transfer_bw -z 268435456

# 仅测 device 1 → device 3 的读延迟，64 B
./ze_peer -t latency -o read -z 64 -s 1 -d 3

# 双向带宽，device 0 ↔ device 1
./ze_peer -b -t transfer_bw -s 0 -d 1
```

---

### 10.3 并行拷贝测试（单节点）

#### `--parallel_single_target`：多引擎并发分片拷到单目标

```bash
# device 0 → device 1，使用引擎 0 活 1，buffer 256MB（每引擎 128MB）
./ze_peer --parallel_single_target -t transfer_bw -z 268435456 -s 0 -d 1 -u 1

# 队列放在目标端
./ze_peer --parallel_single_target -t transfer_bw -z 268435456 -s 0 -d 1 -u 1 -x dst
```

#### `--parallel_multiple_targets`：一个源并发拷到多个目标

```bash
# device 1 → device 2 和 device 3，各用引擎 0 和 1
./ze_peer --parallel_multiple_targets -t transfer_bw -z 268435456 -s 1 -d 2,3 -u 0,1

# 双向
./ze_peer --parallel_multiple_targets -t transfer_bw -z 268435456 -s 1 -d 2,3 -u 0,1 -b

# --divide_buffers：每目标 buffer 也按引擎均分
./ze_peer --parallel_multiple_targets --divide_buffers -t transfer_bw -z 268435456 -s 0 -d 1,2 -u 0,1,2,3
```

#### `--parallel_pair_targets`：成对并行

```bash
# 默认对（device 0 → 所有其他设备），使用所有计算引擎
./ze_peer --parallel_pair_targets -t transfer_bw -z 268435456

# 显式指定对
./ze_peer --parallel_pair_targets 0:1,2:3 -t transfer_bw -z 268435456
```

---

### 10.4 同主机 IPC 测试

```bash
# device 0 分配、device 1 拷贝（write），固定 256 MB
./ze_peer --ipc -s 0 -d 1 -t transfer_bw -o write -z 268435456

# 全量尺寸扫描，read 操作
./ze_peer --ipc -s 0 -d 1 -t transfer_bw -o read
```

---

### 10.5 跨节点 MPI IPC 测试

#### 前置条件

- 两台主机均安装 `/dev/vmem` 内核模块
- MPI 环境可用（`mpirun` / `mpiexec`）
- `ze_peer` 以 `ZE_PEER_ENABLE_MPI=1` 编译
- 两主机间 GPU 通过 PCIe Switch互联（P2P）

#### 基础 1:1

```bash
# 1:1（-s 0 -> -d 0），写带宽
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0 -d 0 -t transfer_bw -o write -z 268435456
```

> 两个 MPI rank 分别运行在 hostA/hostB，`-s/-d` 定义源/目的 GPU 索引，
> 不固定绑定 rank 0/1。  
> 单向模式下先把 `-d` 的内存元数据导入到 `-s` 侧，再由 `-s` 发起 P2P copy。  
> 双向模式（`-b`）下两端都会导入对端元数据并执行 copy。

#### 参数变体

```bash
# 1:1，非对称：源侧 GPU 1 → 目的侧 GPU 0
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 1 -d 0 -t transfer_bw -o write

# 1:N：源侧 GPU 0 同时与目的侧 GPU 0、1、2 测试（Cartesian）
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0 -d 0,1,2 -t transfer_bw -o write

# N:1：源侧多 GPU 分别与目的侧 GPU 0 测试
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0,1,2 -d 0 -t transfer_bw -o write

# N:N
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0,1 -d 2,3 -t transfer_bw -o write

# 双向（-b）在 1:1 / 1:N / N:1 / N:N 下同样适用：
# 在上述命令中增加 -b，即两边都做执行端。

# --parallel_single_target 拓扑选择：1 对
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi --parallel_single_target -s 0 -d 0 -t transfer_bw -o write

# --parallel_multiple_targets 拓扑选择：源侧 GPU 0 → 目的侧全部 GPU
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi --parallel_multiple_targets -s 0 -t transfer_bw -o write

# 显式对列表
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi --parallel_pair_targets 0:0,1:1,2:2 -t transfer_bw -o write

# 非对称显式对
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi --parallel_pair_targets 0:2,1:3,2:1 -t transfer_bw -o write -z 268435456

# 测延迟
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0 -d 0 -t latency -o write -z 64

# 全量类型和方向（不指定 -t / -o）
mpirun -n 2 -hosts hostA,hostB ./ze_peer --ipc-mpi -s 0 -d 0
```

## 11. 限制与已知问题

| 限制 | 说明 |
|------|------|
| `--ipc-mpi` 仅支持 2 个 MPI rank | `MPI_Sendrecv` 使用固定的 `peer_rank = 1 - mpi_rank` |
| 多 pair 并发要求 `MPI_THREAD_MULTIPLE` | 否则在启动阶段直接报错退出 |
| MPI tag 空间不足会失败 | 运行前会检查 `MPI_TAG_UB` 与 pair 数量 |
| `--ipc` 不支持双向模式（`-b`） | 双向仅支持 `--ipc-mpi`，且双端都会执行 copy |
| `--parallel_single/multiple_targets` 在 `--ipc-mpi` 下为拓扑选择器，不执行真正的并行引擎分片 | 但由其生成的 pair 会并发执行（每 pair 一条执行线程） |
| `--parallel_pair_targets` + `--ipc-mpi-pairs` 不能同时提供显式对列表 | 会报 `[ERROR] Do not use both...` |
| `/dev/vmem` 支持的 PFN 段数最多 8 个 | `vmem_pfn_list.nents` 上限为 8 |
| IPC 测试（`--ipc` 和 `--ipc-mpi`）始终使用 `engines[0]` | 不受 `-u` 影响 |
| 非 MPI 的 `--parallel_*` 测试不支持 `--ipc` 模式 | 不检查但行为未定义 |
