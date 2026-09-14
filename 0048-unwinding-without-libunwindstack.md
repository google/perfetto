# Independent unwinding library in Perfetto without external dependency on android/libunwindstack

**Authors:** @safayat-google
**Status:** Discussion
**Discussion:** https://github.com/google/perfetto/discussions/7283

## Problem

Currently, callstack unwinding in Perfetto (`traced_perf` and `heapprofd`) is tightly coupled to Android's `libunwindstack` library (`buildtools/android-unwinding`).

While `libunwindstack` is essential on Android due to its support for ART Dex files, ART JIT debug frames (`JitDebug`), and Bionic integration, coupling standalone Linux builds to it introduces several significant drawbacks:

* **Maintenance**: Upstream API changes in `libunwindstack` causes build breakages in Perfetto. Maintaining a standalone copy of `libunwindstack` for Linux host/target builds is costly.
* **Upstream Linux profiling tools**: Upstream Linux profiling tools (e.g. Linux `perf`) rely on `libunwind`, which is standard across Linux distributions, actively maintained, and well-integrated with distribution debug packages and remote unwinding.
* **Dependency on a single unwinder headers**: Core profiling components depends on libunwindstack specific headers.

## Scope
- Android continue to use libunwindstack
- Linux host/target completely decoupled from libunwinstack, will use the new unwinding library (i.e. libunwind)

## Decision

Pending

## 2. Architecture & Design

Rather than duplicating `libunwindstack`'s internal classes (`Regs`, `Memory`, `Maps`, `Elf`), we introduce a high-level, backend-agnostic unwinding interface that encapsulates the entire unwinding and process-context lifecycle.

```
┌──────────────────────────────────────────────────────────────────────────────────┐
│                            Kernel Perf Ring Buffer                               │
│                      (PERF_RECORD_SAMPLE: regs + stack)                          │
└──────────────────────────────────────┬───────────────────────────────────────────┘
                                       │
                                       ▼
┌──────────────────────────────────────────────────────────────────────────────────┐
│                          EventReader / Regs Parsing                              │
│         - Extracts raw kernel register values.                                   │
│         - Produces zero-alloc unwind::CpuRegisters in ParsedSample               │
└──────────────────────────────────────┬───────────────────────────────────────────┘
                                       │ ParsedSample (unwind::CpuRegisters)
                                       ▼
┌──────────────────────────────────────────────────────────────────────────────────┐
│                    Unwinding Abstraction Layer (Unwinder)                        │
│                                                                                  │
│   ┌──────────────────────────┐             ┌─────────────────────────────────┐   │
│   │   CpuRegisters / Arch    │             │      UnwindFrame / Result       │   │
│   │ (Zero-alloc register rep)│             │  (Zero-copy base::StringView)   │   │
│   └──────────────────────────┘             └─────────────────────────────────┘   │
│                                                                                  │
│   ┌──────────────────────────────────────────────────────────────────────────┐   │
│   │                        Unwinder (Abstract Interface)                     │   │
│   │   - CreateProcessContext(pid, maps_fd, mem_fd) -> ProcessUnwindContext   │   │
│   │   - Unwind(input, proc_ctx)                    -> UnwindResult           │   │
│   │   - ClearGlobalCache()                                                   │   │
│   └──────────────────────────────────────────────────────────────────────────┘   │
│                                 ▲                           ▲                    │
│         Factory (UnwinderType)  │                           │                    │
│         ┌───────────────────────┴──────┐      ┌─────────────┴────────────────┐   │
│         │   LibunwindstackUnwinder     │      │       LibunwindUnwinder      │   │
│         │  (Android / Legacy Linux)    │      │     (Linux perf-style)       │   │
│         │   - Maps / Memory overlay    │      │   - unw_accessors_t          │   │
│         │   - Dex & Art JIT support    │      │   - Custom access_mem / reg  │   │
│         │   - FramePointerUnwinder     │      │   - Direct stack memory read │   │
│         └──────────────────────────────┘      └──────────────────────────────┘   │
└──────────────────────────────────────┬───────────────────────────────────────────┘
                                       │ CompletedSample (Generic UnwindFrames)
                                       ▼
┌──────────────────────────────────────────────────────────────────────────────────┐
│                           Downstream Emission                                    │
│         - GlobalCallstackTrie::CreateCallsite(frames, build_ids)                 │
│         - PerfProducer::EmitSample (converts UnwindErrorCode to proto)           │
└──────────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Core Abstraction Interface
### 3.1 `CpuRegisters` (Zero-Allocation Register Representation)
Previously, `traced_perf` allocated `std::unique_ptr<unwindstack::Regs>` on the heap for every sample in the queue.  
In `cpu_registers.h`, this is replaced by a flat, stack-allocated struct:

```cpp
struct CpuRegisters {
  CpuArch arch = CpuArch::kUnknown;
  uint64_t pc = 0;
  uint64_t sp = 0;
  uint64_t regs[64] = {};  // Flat register array covering all architectures
  ...
};
```
- **Zero Allocations**: Carried by value in `ParsedSample` and queued directly in `UnwindEntry` without dynamic memory allocations.
- **Architecture Aware**: Provides mapping functions for ARM, ARM64, x86, x86_64, and RISC-V registers.

### 3.2 `Unwinder` & `ProcessUnwindContext`
The core interface is defined in `unwinder.h`:

```cpp
class ProcessUnwindContext {
 public:
  virtual ~ProcessUnwindContext() = default;
  virtual pid_t pid() const = 0;
  virtual void ReparseMaps() = 0;
  virtual void ClearCache() = 0;
  virtual base::StringView GetBuildId(uint64_t pc) = 0;
  virtual std::optional<UnwindFrame> BuildFrameFromPc(uint64_t pc) = 0;
};

class Unwinder {
 public:
  virtual ~Unwinder() = default;

  virtual std::unique_ptr<ProcessUnwindContext> CreateProcessContext(
      pid_t pid,
      base::ScopedFile maps_fd,
      base::ScopedFile mem_fd) = 0;

  virtual UnwindResult Unwind(
      const UnwindInputSample& sample,
      ProcessUnwindContext* context) = 0;

  virtual void ClearGlobalCache() = 0;
};
```

### 3.3 Zero-Copy Output Types (`base::StringView`)
`UnwindFrame` uses `perfetto::base::StringView` instead of `std::string` to point directly into the metadata/cache storage, avoiding string copies and heap churn across frames:
Just with this, we already see latency improvement of 3% to 4% in the draft `perf` benchmark tests.

```cpp
struct UnwindFrame {
  uint64_t pc = 0;
  uint64_t rel_pc = 0;
  uint64_t sp = 0;

  base::StringView map_name;
  uint64_t map_start = 0;
  uint64_t map_end = 0;
  uint64_t map_offset = 0;
  uint64_t map_elf_start_offset = 0;
  uint64_t map_load_bias = 0;
  base::StringView build_id;

  base::StringView function_name;
  uint64_t function_offset = 0;
};

struct UnwindResult {
  UnwindErrorCode error_code = UnwindErrorCode::kNone;
  uint64_t warnings = 0;
  std::vector<UnwindFrame> frames;

  bool success() const { return error_code == UnwindErrorCode::kNone; }
};
```
---

## 4. Backend Implementations

### 4.1 `libunwindstack` Backend (Android & Linux Legacy)
- Lives in `src/profiling/unwind/libunwindstack/`.
- Wraps `unwindstack::Unwinder`, `unwindstack::Maps`, and `unwindstack::Memory`.
- Preserves full Android support: `SetJitDebug()` and `SetDexFiles()` remain fully intact when building for Android.

### 4.2 `libunwind` Backend (Linux Native Remote Unwinding)
- Lives in `src/profiling/unwind/libunwind/`.
- Uses `libunwind`'s remote unwinding API (`unw_create_addr_space`, `unw_init_remote`, `unw_step`).
- **Remote Accessor Design (`unw_accessors_t`)**:
  - `access_mem`: Intercepts memory reads. For addresses in `[sp, sp + stack_size)`, it reads directly from the sampled stack buffer in memory. For addresses outside the stack, it reads from `/proc/<pid>/mem` via `pread64(mem_fd, ...)`.
  - `access_reg`: Reads registers directly from `sample.regs->regs[regnum]`.
  - `find_proc_info`: Resolves DWARF unwind tables from mapped ELF objects.

### 4.3 Frame Pointer Backend
- Fast userspace frame pointer unwinding for binaries compiled with `-fno-omit-frame-pointer`.
- Accepts `(fp, sp, pc, arch)` directly from `sample.regs`.

---

## 5. Benchmark Results & Live Host Validation

### 5.1 Micro-benchmarks (`perfetto_benchmarks`)
* Note: Only based on the abstraction + libunwindstack. May change in the future when we add libunwind support for linux host/target.

Benchmarked on Linux `x86_64` (Release build `out/linux_clang_release`) with `benchmark::DoNotOptimize(result)` active:

| Benchmark Case | Baseline (CPU) | Abstraction (`base::StringView` + Zero-Alloc Regs) | Latency Delta | Peak RSS (Baseline / Abstraction) | Peak RSS Delta | RSS Growth |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **DWARF Depth 10** | 4,855 ns | **4,728 ns** | **-2.6% faster** (-127 ns) | 9,732 KB / 8,216 KB | **-15.6%** | 1.18 MB |
| **DWARF Depth 30** | 9,585 ns | **9,157 ns** | **-4.5% faster** (-428 ns) | 10,160 KB / 8,484 KB | **-16.5%** | 1.18 MB |
| **DWARF SharedLib DSO (Depth 20)** | 8,707 ns | **8,392 ns** | **-3.6% faster** (-315 ns) | 10,356 KB / 8,680 KB | **-16.2%** | 1.37 MB |
| **DWARF Signal Handler (Depth 20)** | 8,729 ns | **8,180 ns** | **-6.3% faster** (-549 ns) | 10,520 KB / 8,812 KB | **-16.2%** | 1.50 MB |
| **DWARF ColdStart (Depth 20)** | 264,057 ns | **256,092 ns** | **-3.0% faster** (-7,965 ns) | 10,264 KB / 8,500 KB | **-17.2%** | 1.18 MB |
| **FramePointer Depth 10** | 1,114 ns | **1,504 ns** | +35.0% (+390 ns) | 10,264 KB / 8,504 KB | **-17.1%** | **0 KB** |
| **FramePointer Depth 30** | 3,025 ns | **3,428 ns** | +13.3% (+403 ns) | 10,264 KB / 8,504 KB | **-17.1%** | **0 KB** |
| **MemoryCache Churn (5000 iters)** | 7,151 ns | **7,213 ns** | +0.8% (+62 ns) | 10,264 KB / 8,508 KB | **-17.1%** | **0 KB** |

- **Zero Overhead for DWARF**: DWARF unwinding is **2.6% to 6.3% faster** than the baseline because eliminating per-sample heap allocations (`StackOverlayMemory` and `unwindstack::Regs::Clone()`) removes glibc allocator lock contention and preserves CPU cache locality.
- **Reduced Memory Footprint**: Peak RSS is reduced by **15% to 17%** across all workloads.
- **Constant FramePointer Delta**: The +390 ns delta in FramePointer is fixed across all depths (Depth 10: +390 ns, Depth 20: +392 ns, Depth 30: +403 ns), reflecting the one-time cost of resolving `build_id` and populating `UnwindFrame` structs.

---

## 6. Implementation & PR Breakdown

### PR #1: Core Abstraction Types & Interface
- Folder: `src/profiling/unwind/`.
- `unwind_types.h` (`CpuArch`, `UnwindFrame`, `UnwindResult`, `UnwindErrorCode`).
- `cpu_registers.h`
- `unwinder.h` (`Unwinder`, `ProcessUnwindContext` pure virtual interfaces).
- `unwinder_factory.h` (`CreateUnwinder(UnwinderType)`).
- `unwinder_unittest.cc`.

### PR #2: `libunwindstack` Backend & Benchmark Suite
- Implement `libunwindstack_unwinder.cc` and `libunwindstack_context.cc` under `src/profiling/unwind/libunwindstack/`.
- Implement `frame_pointer_unwinder.cc` decoupled from `unwindstack::Regs`.
- Introduce `src/profiling/perf/unwind_benchmark.cc` to continuously track unwinding latency and RSS regressions across backends.

### PR #3: Migrate `traced_perf` to the Abstraction Layer
- Refactor `src/profiling/perf/`:
  - `regs_parsing.cc`: Populate `unwind::CpuRegisters` directly from perf sample buffers.
  - `unwinding.cc`: Use `unwind::Unwinder` and `unwind::ProcessUnwindContext`.
  - `callstack_trie.cc`: Consume `unwind::UnwindFrame`.
- Completely remove `#include <unwindstack/...>` from `src/profiling/perf/`.

### PR #4: `libunwind` Remote Backend for Standalone Linux
- Implement `src/profiling/unwind/libunwind/` (`libunwind_unwinder.cc`, `libunwind_accessor.cc`, `libunwind_context.cc`).
- Add GN configuration to enable `libunwind` backend on standalone Linux builds.

### PR #5: Migrate `heapprofd` (Memory Profiler)
- Transition `heapprofd` from `src/profiling/common/unwind_support` to `src/profiling/unwind/`.
- Consolidate all unwinding across Perfetto under `src/profiling/unwind/`.


## Alternatives considered for the linux unwinding

### libdwfl/elfutils
* Pros:
  * Ideal offline unwinding on linux. used by the linux perf tool.
  * Good elf management
* Cons:
  * LGPL license issue. Doable but we need to link (dlopen) against the systems dynamic .so libraries.
  * Heavier memory footprint compared to other options

### LLVM libunwind
* Not an option since it lacks support for remote unwinding.

### framehop (Rust library for stack unwinding)
* Pros:
  * Optimized for speed and memory footprint.
* Cons:
  * Maintenance concern for a rust library in a cpp project.


## Appendix: Core APIs we currently use from libunwindstack
- `unwindstack/Unwinder.h`
```cpp
  struct FrameData { num, rel_pc, pc, sp, function_name, offset, map_info }
  ...

  class Unwinder {
    Unwinder(size_t max_frames, Maps* maps, Regs* regs, std::shared_ptr<Memory> process_memory);
    ...
    virtual void Unwind(initial_map_names_to_skip, map_suffixes_to_ignore);
    ...
    // For android
    void SetJitDebug(JitDebug* jit_debug);
    void SetDexFiles(DexFiles* dex_files);
    ...
  }
```

- `unwindstack/Memory.h`
```cpp
// abstraction to read a block of memory from file/cache etc
class Memory {
  ...
  virtual size_t Read(addr, dst, size);
}
```

- `unwindstack/Maps.h`
```cpp
// Read /proc/[pid]/maps from an open file descriptor
unwindstack::Maps {
  std::vector<std::shared_ptr<MapInfo>> maps_;
}
```

- `unwindstack/Regs.h`
```cpp
// Core API to capture register snapshots inline
AsmGetRegs(void* regs);

// Different register mappings on different architectures
enum X86Reg : uint16_t {
  X86_REG_EAX = 0,
  ...
}

enum X86_64Reg, ArmReg, Arm64Reg
```

- `unwindstack/Elf.h`
```cpp
class Elf {
 public:
  Elf(std::shared_ptr<Memory>& memory);
  // we call it to reset the elf cache
  static void SetCachingEnabled(bool enable);
}
```
