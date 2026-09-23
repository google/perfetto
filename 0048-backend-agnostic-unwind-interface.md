# RFC-0048: Linux Unwinding Abstraction Layer for traced_perf and heapprofd

**Authors:** @safayat-google\
**Status:** In Review\
**Discussion:** https://github.com/google/perfetto/discussions/7283\
**Proof of concept:** [patch](https://paste.googleplex.com/4707995955625984)

---

## 1. Problem Statement & Motivation

Currently, callstack unwinding in Perfetto (`traced_perf` and `heapprofd`) is tightly coupled to Android's `libunwindstack` library (`buildtools/android-unwinding`).

On Android `libunwindstack` is essential due to its support for ART Dex files, ART JIT debug frames (`JitDebug`), and Bionic integration. However, for standalone Linux builds, coupling to `libunwindstack` introduces several significant drawbacks:

1. **Build Fragility & Out-of-Tree Maintenance**: Upstream API changes in `libunwindstack` frequently cause build breakages in Perfetto. Maintaining a standalone copy of `libunwindstack` for Linux host/target builds is costly.
2. **Inability to Leverage Linux-Native Tooling**: Upstream Linux profiling tools (e.g. Linux `perf`) rely on `libunwind` or `libdw`, which are standard across Linux distributions, actively maintained by the Linux toolchain community, and well-integrated with distribution debug packages and remote unwinding.
3. **Tight Architectural Coupling**: Core profiling components like `traced_perf` directly include `<unwindstack/*.h>`.

### Scope
- **Android**: Continues to use `libunwindstack` by default. Full support for ART Dex files and JIT frames is preserved without regression.
- **Standalone Linux**: Decoupled from `libunwindstack`. The abstraction allows Linux builds to use alternative unwinding backends without depending on Android-specific libraries.

---

## 2. Proposed Unwinding Libraries for Standalone Builds

For standalone Linux builds, we propose two complementary unwinding options:

### 2.1 Primary DWARF Unwinder: `libunwind`
- It is the standard unwinding engine used by Linux `perf` and GNU tools. It provides a stable C API (`unw_create_addr_space`, `unw_init_remote`, `unw_step`), has an MIT/LLVM-compatible license, and supports remote unwinding across all standard Linux architectures (x86_64, ARM64, ARM32, RISC-V).

### 2.2 Built-in Zero-Dependency Frame Pointer Unwinder: `FramePointerUnwinder`
- It is a native, self-contained implementation with **zero external library dependencies**. It traverses frame pointer chains directly on the stack buffer, providing ultralow-overhead unwinding without parsing DWARF.

---

## 3. Proposed Interface Types

We introduce a high-level, backend-agnostic unwinding interface that encapsulates the entire unwinding and process-context lifecycle.

```
┌────────────────────────────────────────────────────────────────────────┐
│                        Sampling Producer                               │
│        traced_perf (CPU sampling) / heapprofd (Memory profiling)       │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ UnwindInputSample (regs + stack)
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│               Common Unwinding Interface (unwind::Unwinder)            │
│                                                                        │
│   - CreateProcessContext(pid, maps_fd, mem_fd) -> ProcessUnwindContext │
│   - Unwind(sample, proc_ctx, options)          -> UnwindResult         │
│   - SupportsLiveSymbolization()                -> bool                 │
└───────────────────┬────────────────────────────────┬───────────────────┘
                    │                                │
        ┌───────────┴───────────┐        ┌───────────┴───────────┐
        ▼                       ▼        ▼                       ▼
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│ libunwindstack   │  │ libunwind        │  │ frame_pointer    │
│ (Android + ART)  │  │ (Linux DWARF)    │  │ (Pure C++, 0 dep)│
└──────────────────┘  └──────────────────┘  └──────────────────┘
```

### 3.1 `CpuRegisters` (Zero-Allocation Register Representation)
Previously, `traced_perf` allocated `std::unique_ptr<unwindstack::Regs>` on the heap for every sample in the queue. In `cpu_registers.h`, this is replaced by a flat, stack-allocated POD struct:

```cpp
struct CpuRegisters {
  CpuArch arch = CpuArch::kUnknown;
  uint64_t pc = 0;
  uint64_t sp = 0;
  uint64_t regs[64] = {};  // Flat register array covering all architectures

  bool is_valid() const { return arch != CpuArch::kUnknown && pc != 0; }
  void Reset() { arch = CpuArch::kUnknown; pc = 0; sp = 0; }
};
```
- Carried by value in sample queues without dynamic heap allocations.
- Architecture-aware mappings for different platforms.

### 3.2 `Unwinder` & `ProcessUnwindContext`

```cpp
// src/profiling/unwind/unwinder.h
// Encapsulates per-process resources (procfs fds, memory maps, DWARF tables).
class ProcessUnwindContext {
 public:
  virtual ~ProcessUnwindContext() = default;
  virtual pid_t pid() const = 0;
  virtual void ReparseMaps() = 0;
  virtual base::StringView GetBuildId(uint64_t pc) = 0;
  virtual std::optional<UnwindFrame> BuildFrameFromPc(uint64_t pc) = 0;
};

// Abstract unwinder engine.
class Unwinder {
 public:
  virtual ~Unwinder() = default;
  virtual const char* Name() const = 0;

  virtual std::unique_ptr<ProcessUnwindContext> CreateProcessContext(
      pid_t pid,
      base::ScopedFile maps_fd,
      base::ScopedFile mem_fd) = 0;

  virtual UnwindResult Unwind(
      const UnwindInputSample& sample,
      ProcessUnwindContext* context,
      const UnwindOptions& options = {}) = 0;

  virtual bool SupportsLiveSymbolization() const { return false; }

  // Session lifecycle & maintenance hooks: backends manage their own resources/state
  virtual void OnTracingSessionStarted() {}
  virtual void OnTracingSessionEnded() {}
  virtual void OnPeriodicCleanup() {}
};
```

### 3.3 Output Types (`UnwindFrame` & `UnwindResult`)

```cpp
// src/profiling/unwind/unwind_types.h
enum class SymbolStatus : uint8_t {
  kNotAttempted = 0,  // Symbolization was not requested or not supported by backend
  kResolved = 1,      // function_name was resolved
  kFailed = 2,        // Symbolization was requested and attempted, but not found in ELF/symtab
};

struct UnwindFrame {
  uint64_t pc = 0;
  uint64_t rel_pc = 0;
  uint64_t sp = 0;

  // Module / Mapping information (zero-copy string views)
  base::StringView map_name;
  uint64_t map_start = 0;
  uint64_t map_end = 0;
  uint64_t map_offset = 0;
  uint64_t map_elf_start_offset = 0;
  uint64_t map_load_bias = 0;
  base::StringView build_id;

  // Symbol resolution
  base::StringView function_name;
  uint64_t function_offset = 0;
  SymbolStatus symbol_status = SymbolStatus::kNotAttempted;
};

struct UnwindResult {
  UnwindErrorCode error_code = UnwindErrorCode::kNone;
  uint64_t warnings = 0;
  std::vector<UnwindFrame> frames;

  bool success() const { return error_code == UnwindErrorCode::kNone; }
};
```

---

## 4. Other Considerations

### 4.1 Making "Live" Symbolization Optional
- **`traced_perf`**: Does **not** perform live symbolization during sampling. It emits raw PCs and mapping information (`build_id`, `rel_pc`), leaving symbolization to Trace Processor offline.
- **`heapprofd`**: Optionally performs live symbolization when using `libunwindstack`.

### 4.2 Handling Library Divergences (File Opening & DWARF Discovery)
Unwinding libraries differ in how they locate DWARF unwind tables and access process memory:
- `libunwindstack` opens files, parses `/proc/<pid>/maps`, and loads DWARF internally via its own `Memory` and `Elf` classes.
- `libunwind` uses callback hooks (`access_mem`, `access_reg`) and `unw_create_addr_space` operating over stack snapshots and `/proc/<pid>/mem`.
- `frame_pointer` requires zero ELF or DWARF discovery, operating purely on the stack buffer.

These differences are completely encapsulated within each backend's `ProcessUnwindContext` implementation. The caller only provides the sampled stack buffer and register state to unwind.

### 4.3 Dwarf table Caching on abstract API
- Some backend supports dwarf caching and other does not. So, instead of polluting the api, we decided to leave it to the backends to implement their own dwarf caching policy. However Tracing lifecycle and periodic clear callback are provided for the backend to manage their caches.

---

## 5. Proof of Concept & Other References

- **Proof of concept patch (Unwind Abstraction & Backends)**:
  - [[1](https://paste.googleplex.com/4707995955625984)] - Working implementation of the abstraction layer, `libunwindstack` migration, zero-dependency `frame_pointer`, and `libunwind` backend.
  - [[2](https://paste.googleplex.com/6226625105100800)] – Standalone microbenchmark harnesses for `traced_perf` and `heapprofd`.
  - [[3](https://paste.googleplex.com/6128470002892800)] - Add ons + other unwinder backend
  - [[4](https://paste.googleplex.com/5329222021808128)] - Benchmark numbers (micro-benchmarks, live CPU profiling, memory stress tests), tool comparison matrix, and alternative backend evaluation.
