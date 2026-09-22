# RFC-0048: Linux Unwinding Abstraction Layer for traced_perf and heapprofd

**Authors:** @safayat-google
**Status:** In Review / Updated Draft
**Discussion:** https://github.com/google/perfetto/discussions/7283
**Proof of Concept (PoC) Patches:**
- [0001-add-benchmarks-for-traced-perf-and-heapprofd.patch](https://paste.googleplex.com/6226625105100800) (Benchmark suite)
- [0002-draft-unwind-all.patch](https://paste.googleplex.com/4707995955625984) (Full unwinding abstraction & backends)

---

## 1. Summary & Motivation

Currently, callstack unwinding in Perfetto (`traced_perf` and `heapprofd`) is tightly coupled to Android's `libunwindstack` library (`buildtools/android-unwinding`).

While `libunwindstack` is essential on Android due to its support for ART Dex files, ART JIT debug frames (`JitDebug`), and Bionic integration, coupling standalone Linux builds to it introduces several significant drawbacks:

1. **Build Fragility & Out-of-Tree Maintenance**: Upstream API changes in `libunwindstack` frequently cause build breakages in Perfetto. Maintaining a standalone copy of `libunwindstack` for Linux host/target builds is costly.
2. **Inability to Leverage Linux-Native Tooling**: Upstream Linux profiling tools (e.g. Linux `perf`) rely on `libunwind` or `libdw`, which are standard across Linux distributions, actively maintained by the Linux toolchain community, and well-integrated with distribution debug packages and remote unwinding.
3. **Tight Architectural Coupling**: Core profiling components (`traced_perf`, `callstack_trie`, `event_reader`, `unwinding.cc`) directly include `<unwindstack/*.h>` and allocate heap objects (such as `std::unique_ptr<unwindstack::Regs>`) directly into sample queues.

### Scope Clarification
- **Android**: Continues to use `libunwindstack` by default. Full support for ART Dex files and JIT frames is preserved without regression.
- **Standalone Linux**: Decoupled from `libunwindstack`. The abstraction allows Linux builds to use alternative unwinding backends (such as `libunwind`, `framehop`, `libdw`, or fast frame pointer unwinding) without depending on Android-specific libraries.
- **Built-in Zero-Dependency Frame Pointer Unwinder**: Adds a native, self-contained `FramePointerUnwinder` with **0 external library dependencies (pure C++ / STL)**, providing ultralow-overhead unwinding (~10 ns/frame) on systems and binaries compiled with frame pointers enabled (`-fno-omit-frame-pointer`).
- **Unified Subsystem**: Placed under `src/profiling/unwind/` so that both `traced_perf` (CPU sampling) and `heapprofd` (native memory profiling) share the same unwinding infrastructure.

---

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
│         - Extracts raw kernel register values                                    │
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
│   │   - OnTracingSessionStarted() / OnTracingSessionEnded()                  │   │
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
│         - PerfProducer::EmitSample / heapprofd Bookkeeping                       │
└──────────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Core Abstraction Interface

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
- **Zero Allocations**: Carried by value in `ParsedSample` and queued directly in `UnwindEntry` or `AllocMetadata` without dynamic memory allocations.
- **Architecture Aware**: Provides mapping functions for ARM, ARM64, x86, x86_64, and RISC-V registers.

### 3.2 `Unwinder` & `ProcessUnwindContext`
The core interface is defined in `unwinder.h`:

```cpp
class ProcessUnwindContext {
 public:
  virtual ~ProcessUnwindContext() = default;
  virtual pid_t pid() const = 0;
  virtual void ReparseMaps() = 0;
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

  // Session lifecycle hooks: backends manage their own internal caches/lifecycles
  virtual void OnTracingSessionStarted() {}
  virtual void OnTracingSessionEnded() {}
};
```
- **Autonomous Cache Management**: Callers (`traced_perf`, `heapprofd`) do not imperatively manage backend caches via `ClearCache()`. Each backend is responsible for bounding, evicting, and managing its own caches internally.
- **Session Lifecycle Hooks**: `OnTracingSessionStarted()` and `OnTracingSessionEnded()` allow backends to initialize or release session-scoped resources cleanly.

### 3.3 Zero-Copy Output Types (`base::StringView`)
`UnwindFrame` uses `perfetto::base::StringView` instead of `std::string` to point directly into the metadata/cache storage, avoiding string copies and heap churn across frames:

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

The abstraction layer isolates all backend-specific types and dependencies within `src/profiling/unwind/<backend>/`. Downstream callers in `traced_perf` and `heapprofd` interact exclusively with `unwind::Unwinder`, `unwind::ProcessUnwindContext`, `unwind::CpuRegisters`, and `unwind::UnwindResult`.

---

### 4.1 Android Implementation: Extending the Interface via `libunwindstack`

Located in `src/profiling/unwind/libunwindstack/`:
- `class libunwindstack::Unwinder : public unwind::Unwinder`
- `class libunwindstack::Context : public unwind::ProcessUnwindContext`

This backend extends the abstraction interface to preserve full Android platform support (including ART Dex and JIT frames) while eliminating per-sample heap allocations:

#### 1. In-Place Register Ingestion (`GetOrCreateRegs`)
The backend reuses a thread-local `cached_regs_` instance and updates its internal buffer directly via `RawData()`, eliminating per-sample `new unwindstack::RegsArm64` or `regs->Clone()` allocations:

```cpp
unwindstack::Regs* Unwinder::GetOrCreateRegs(const CpuRegisters& raw_regs) {
  if (!cached_regs_ || cached_regs_->Arch() != target_arch) {
    cached_regs_ = ToBackendRegs(raw_regs);
    return cached_regs_.get();
  }

  // In-place 1-to-1 memory overwrite of GP registers + SP + PC (~3 ns, 0 allocs)
  uint64_t* data = reinterpret_cast<uint64_t*>(cached_regs_->RawData());
  std::memcpy(data, &raw_regs.regs[0], sizeof(uint64_t) * (PERF_REG_ARM64_PC + 1));
  return cached_regs_.get();
}
```

#### 2. Implementing `Unwinder::CreateProcessContext`
```cpp
std::unique_ptr<ProcessUnwindContext> Unwinder::CreateProcessContext(
    pid_t pid, base::ScopedFile maps_fd, base::ScopedFile mem_fd) {
  return std::make_unique<Context>(pid, std::move(maps_fd), std::move(mem_fd));
}
```
- Wraps `maps_fd` in `FDMaps : public unwindstack::Maps` to parse `/proc/<pid>/maps` directly from the file descriptor.
- Wraps `mem_fd` in `FDMemory : public unwindstack::Memory` to read memory via `pread64`.
- On Android, initializes `unwindstack::JitDebug` and `unwindstack::DexFiles` for ART runtime unwinding.

#### 3. Implementing `Unwinder::Unwind`
```cpp
UnwindResult Unwinder::Unwind(const UnwindInputSample& sample,
                              ProcessUnwindContext* context) {
  auto* ctx = static_cast<Context*>(context);

  // 1. Stack memory virtualization (zero-alloc stack overlay)
  StackOverlayMemory overlay_memory(ctx->metadata().fd_mem, sample.regs->sp,
                                     sample.stack_data, sample.stack_size);
  std::shared_ptr<unwindstack::Memory> overlay_ptr(&overlay_memory, [](unwindstack::Memory*) {});

  // 2. Register ingestion (in-place update)
  auto* regs = GetOrCreateRegs(*sample.regs);

  // 3. Attach Android-specific ART runtime handlers
  unwindstack::Unwinder unwinder(kMaxFrames, &ctx->metadata().fd_maps, regs, overlay_ptr);
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  unwinder.SetJitDebug(ctx->metadata().GetJitDebug(regs->Arch()));
  unwinder.SetDexFiles(ctx->metadata().GetDexFiles(regs->Arch()));
#endif
  unwinder.SetResolveNames(false);
  unwinder.Unwind();

  // 4. Retry on map desynchronization (e.g. dynamic dlopen / dex loading)
  if (unwinder.LastErrorCode() == unwindstack::ERROR_INVALID_MAP ||
      (unwinder.warnings() & unwindstack::WARNING_DEX_PC_NOT_IN_MAP)) {
    ctx->ReparseMaps();
    unwinder.Unwind();
  }

  // 5. Convert native FrameData to generic UnwindFrame (zero-copy StringView)
  UnwindResult result;
  for (const auto& frame : unwinder.ConsumeFrames()) {
    UnwindFrame f;
    f.pc = frame.pc;
    f.rel_pc = frame.rel_pc;
    f.sp = frame.sp;
    f.function_name = base::StringView(frame.function_name.c_str());
    f.function_offset = frame.function_offset;
    if (frame.map_info != nullptr) {
      f.map_name = base::StringView(frame.map_info->name().c_str());
      f.map_start = frame.map_info->start();
      f.map_end = frame.map_info->end();
      f.map_offset = frame.map_info->offset();
      f.map_load_bias = frame.map_info->GetLoadBias();
      f.build_id = ctx->metadata().GetBuildId(frame);
    }
    result.frames.push_back(std::move(f));
  }
  return result;
}
```

#### 4. Lifecycle Management
```cpp
void Unwinder::OnTracingSessionEnded() {
  cached_regs_.reset();
  unwindstack::Elf::SetCachingEnabled(false);
  unwindstack::Elf::SetCachingEnabled(true);
}
```
Purges the global `Elf` object cache across all threads and processes when a tracing session completes.

---

### 4.2 Linux Standalone Implementation: Extending the Interface via `libunwind`

Located in `src/profiling/unwind/libunwind/`:
- `class libunwind::Unwinder : public unwind::Unwinder`
- `class libunwind::Context : public unwind::ProcessUnwindContext`
- `class libunwind::PageCache` (`page_cache.h` / `page_cache.cc`)
- `libunwind_accessor.h` / `libunwind_accessor.cc`

#### 1. Zero-Allocation Register Ingestion (`AccessReg`)
Instead of allocating native register objects, `libunwind` reads register values on demand directly from `CpuRegisters` during CFI evaluation:

```cpp
int AccessReg(unw_addr_space_t as, unw_regnum_t regnum, unw_word_t* valp,
              int write, void* arg) {
  if (write) return -UNW_EINVAL;
  auto* state = static_cast<CursorState*>(arg);
  const CpuRegisters& r = *state->regs;
  switch (regnum) {
    case UNW_X86_64_RAX: *valp = r.regs[PERF_REG_X86_AX]; return 0;
    case UNW_X86_64_RBX: *valp = r.regs[PERF_REG_X86_BX]; return 0;
    case UNW_X86_64_RCX: *valp = r.regs[PERF_REG_X86_CX]; return 0;
    case UNW_X86_64_RDX: *valp = r.regs[PERF_REG_X86_DX]; return 0;
    case UNW_X86_64_RSI: *valp = r.regs[PERF_REG_X86_SI]; return 0;
    case UNW_X86_64_RDI: *valp = r.regs[PERF_REG_X86_DI]; return 0;
    case UNW_X86_64_RBP: *valp = r.regs[PERF_REG_X86_BP]; return 0;
    case UNW_X86_64_RSP: *valp = r.regs[PERF_REG_X86_SP]; return 0;
    case UNW_X86_64_RIP: *valp = r.regs[PERF_REG_X86_IP]; return 0;
    default: return -UNW_EBADREG;
  }
}
```

#### 2. Fast Memory Accessor with Mmap & PageCache (`AccessMem`)
Memory reads check the sampled stack buffer first, then cached mmapped ELF files (**zero syscalls**), and fallback to `PageCache` (`pread64`):

```cpp
int AccessMem(unw_addr_space_t as, unw_word_t addr, unw_word_t* valp,
              int write, void* arg) {
  if (write) return -UNW_EINVAL;
  auto* state = static_cast<CursorState*>(arg);

  // 1. Fast-path: read directly from sampled stack buffer in memory
  uint64_t stack_start = state->regs->sp;
  uint64_t stack_end = stack_start + state->stack_size;
  if (addr >= stack_start && addr + sizeof(unw_word_t) <= stack_end) {
    std::memcpy(valp, state->stack_data + (addr - stack_start), sizeof(unw_word_t));
    return 0;
  }

  // 2. Read from mmapped ELF files (zero syscalls) or fallback PageCache
  if (state->context) {
    return state->context->ReadMem(addr, valp) ? 0 : -UNW_EUNSPEC;
  }
  return -UNW_EUNSPEC;
}
```

#### 3. Remote Unwind Table Lookup (`FindProcInfo`)
Uses `dwarf_search_unwind_table` with `UNW_INFO_FORMAT_REMOTE_TABLE` directly referencing mmapped ELF sections without heap copying:

```cpp
int FindProcInfo(unw_addr_space_t as, unw_word_t ip, unw_proc_info_t* pip,
                 int need_unwind_info, void* arg) {
  auto* state = static_cast<CursorState*>(arg);
  unw_dyn_info_t di;
  if (state->context->FindUnwindTable(ip, &di) != 0) {
    return -UNW_ENOINFO;
  }
  return dwarf_search_unwind_table(as, ip, &di, pip, need_unwind_info, arg);
}
```

#### 4. Evaluated DWARF Rule Cache & `Unwind` Fast-Path
Bypasses `unw_step` DWARF bytecode interpreter on cache hits (~15 ns/frame, >99.5% hit rate):

```cpp
struct UnwindRule {
  enum class Type : uint8_t { kSpOffset, kFpOffset, kEndOfStack, kCantUnwind };
  Type type = Type::kCantUnwind;
  int32_t sp_offset = 0;
  int32_t fp_offset = 0;
  int32_t ra_offset = 0;
};

UnwindResult Unwinder::Unwind(const UnwindInputSample& sample,
                              ProcessUnwindContext* context) {
  auto* ctx = static_cast<Context*>(context);
  UnwindResult result;
  uint64_t cur_ip = sample.regs->pc;
  uint64_t cur_sp = sample.regs->sp;
  uint64_t cur_fp = sample.regs->regs[PERF_REG_X86_BP];

  while (result.frames.size() < kMaxFrames && cur_ip != 0) {
    UnwindFrame frame;
    frame.pc = cur_ip;
    frame.sp = cur_sp;
    if (ctx) frame.build_id = ctx->GetBuildId(cur_ip);
    result.frames.push_back(std::move(frame));

    // Fast-path: Evaluated DWARF Rule Cache (~15 ns, >99.5% hit rate)
    const UnwindRule* rule = ctx ? ctx->FindRule(cur_ip) : nullptr;
    if (rule && rule->type == UnwindRule::Type::kSpOffset) {
      uint64_t ra_addr = cur_sp + rule->ra_offset;
      uint64_t next_ip = 0;
      if (ReadStackWord(sample, ra_addr, &next_ip) && next_ip != 0) {
        cur_sp += rule->sp_offset;
        cur_ip = next_ip;
        continue;
      }
    }

    // Slow-path: libunwind unw_step DWARF bytecode evaluation
    // Step cursor, deduce UnwindRule from (delta_sp, delta_fp, ra_offset), and cache it.
    CursorState state{sample.regs, sample.stack_data, sample.stack_size,
                      ctx ? ctx->mem_fd() : -1, ctx ? ctx->pid() : 0, ctx};
    unw_cursor_t cursor;
    if (unw_init_remote(&cursor, addr_space_, &state) < 0) break;
    // ... unwind one step, record rule, update cur_ip / cur_sp ...
    break;
  }
  return result;
}
```

#### 5. Bounded Memory Limits
- `kMaxRuleCacheSize = 2048` entries (~32 KB).
- `kMaxSymbolCacheSize = 4096` entries (~128 KB).
- `unw_set_cache_size(addr_space_, 256, 0)`.
- Eliminating heap vectors for `.eh_frame_hdr` ensures **0 KB RSS growth** under continuous load.

---

### 4.3 Built-in Frame Pointer Unwinder: Zero External Library Dependencies (`frame_pointer`)

Located in `src/profiling/unwind/frame_pointer/`:
- `class FramePointerUnwinder : public Unwinder`

We implement a native `FramePointerUnwinder` with **zero external library dependencies** (pure C++ / STL without `libunwindstack`, `libunwind`, `libdw`, or Rust toolchains). It is completely self-contained within Perfetto, relying strictly on standard architectural calling conventions (EBP/RBP on x86, X29/FP on ARM64, S0/FP on RISC-V).

Frame pointer walking does not evaluate DWARF CFI, parse ELF sections, or require remote memory reads:

```
                         ┌────────────────────────┐
                         │    unwind::Unwinder    │
                         │   (Abstract Base)      │
                         └───────────┬────────────┘
                                     │
              ┌──────────────────────┴──────────────────────┐
              ▼                                             ▼
┌───────────────────────────┐                 ┌───────────────────────────┐
│   FramePointerUnwinder    │                 │       DwarfUnwinder       │
│ (Backend-Agnostic, Pure)  │                 │  (Remote DWARF/CFI Base)  │
│                           │                 └─────────────┬─────────────┘
│ - ZERO external deps      │                               │
│ - Reads only stack buffer │     ┌─────────────────────────┴─────────────────────────┐
│ - Needs only (fp, sp, pc) │     ▼                                                   ▼
│ - No mem_fd required      │ ┌───────────────────────┐                   ┌───────────────────────┐
└───────────────────────────┘ │ LibunwindstackUnwinder│                   │   LibunwindUnwinder   │
                              │ (Android + ART/Dex)   │                   │ (Linux Standalone)    │
                              └───────────────────────┘                   └───────────────────────┘
```

#### 1. What `FramePointerUnwinder` Needs vs. Drops
| Capability | `FramePointerUnwinder` | `DwarfUnwinder` (`libunwindstack` / `libunwind`) |
| :--- | :--- | :--- |
| **Stack Traversal** | Direct `[fp]` / `[fp+8]` pointer dereference | DWARF state machine (`.eh_frame` / `.debug_frame`) |
| **External Dependencies** | **None** (Pure C++ / STL) | `libunwindstack` or `libunwind` |
| **Required Registers** | Only `(fp, sp, pc, arch)` | Full architecture register array (for callee-saved restoration) |
| **Process Memory (`mem_fd`)**| **Not needed** (all frames in sampled stack buffer) | **Required** (reading ELF headers, CFI tables, global variables) |
| **Process Maps** | Only map/build-id resolution (`GetBuildId`) | Full ELF/Map objects with load bias calculation |
| **Caches** | None | CIE/FDE caches, ELF caches, address space cache |

#### 2. Implementation (`src/profiling/unwind/frame_pointer/frame_pointer_unwinder.cc`)
`FramePointerUnwinder` implements `unwind::Unwinder` directly without any `libunwindstack` or `libunwind` headers:

```cpp
class FramePointerUnwinder : public Unwinder {
 public:
  std::unique_ptr<ProcessUnwindContext> CreateProcessContext(
      pid_t pid, base::ScopedFile maps_fd, base::ScopedFile /*mem_fd*/) override {
    return std::make_unique<MapsContext>(pid, std::move(maps_fd));
  }

  UnwindResult Unwind(const UnwindInputSample& sample,
                      ProcessUnwindContext* context) override {
    UnwindResult result;
    uint64_t fp = GetFp(*sample.regs);
    uint64_t sp = sample.regs->sp;
    uint64_t pc = sample.regs->pc;
    const uint8_t* stack_start = sample.stack_data;
    uint64_t stack_base = sp;
    uint64_t stack_limit = stack_base + sample.stack_size;

    while (result.frames.size() < kMaxFrames) {
      UnwindFrame frame;
      frame.pc = pc;
      frame.sp = sp;
      if (context) frame.build_id = context->GetBuildId(pc);
      result.frames.push_back(std::move(frame));

      // Validate frame pointer alignment and bounds within sampled stack buffer:
      if (fp < stack_base || fp + 16 > stack_limit || (fp & 0x7) != 0) break;

      size_t offset = static_cast<size_t>(fp - stack_base);
      uint64_t next_fp = *reinterpret_cast<const uint64_t*>(stack_start + offset);
      uint64_t next_pc = *reinterpret_cast<const uint64_t*>(stack_start + offset + 8);

      if (next_fp <= fp) break;  // Prevent loops
      sp = fp + 16;
      fp = next_fp;
      pc = next_pc;
    }
    return result;
  }
};
```

---

### 4.4 End-to-End Unified Architecture & Data Flow Diagram

The unwinding abstraction unifies data ingestion and callstack unwinding across both `traced_perf` (CPU sampling) and `heapprofd` (native memory profiling) into a shared unwinding pipeline:

```
┌──────────────────────────────────────────────┐  ┌──────────────────────────────────────────────┐
│         traced_perf (CPU Sampling)           │  │       heapprofd (Native Memory Profiling)    │
│                                              │  │                                              │
│  [ Kernel Perf Ring Buffer ]                 │  │  [ Client Application Process ]             │
│        │ (PERF_RECORD_SAMPLE)                │  │        │ malloc(), calloc(), free()          │
│        ▼                                     │  │        ▼                                     │
│  EventReader / Register Parsing              │  │  heapprofd_client Hook                       │
│    - ReadPerfUserRegsData()                  │  │    - unwind::GetLocalCpuRegisters(&cpu_regs) │
│    - Populates flat unwind::CpuRegisters     │  │    - Snapshots caller stack into shmem       │
│        │                                     │  │        │                                     │
│        ▼ (ParsedSample: CpuRegisters by val) │  │        ▼ (SharedRingBuffer: AllocMetadata)   │
│  UnwindQueue (Sample Queue)                  │  │  heapprofd Daemon Worker                     │
│    - Queues UnwindEntry without heap alloc   │  │    - Dequeues UnwindingRecord from shmem     │
└──────────────────────┬───────────────────────┘  └──────────────────────┬───────────────────────┘
                       │                                                 │
                       │ sample = UnwindInputSample{&cpu_regs, ...}      │
                       └───────────────────────┬─────────────────────────┘
                                               │
                                               ▼
┌────────────────────────────────────────────────────────────────────────────────────────────────┐
│                       Common Unwinding Abstraction Layer (unwind::Unwinder)                    │
│                                                                                                │
│   - proc_ctx = unwinder->CreateProcessContext(pid, maps_fd, mem_fd)                            │
│   - result = unwinder->Unwind(sample, proc_ctx)                                                │
│                                                                                                │
│    ┌───────────────────────────────────────┐      ┌───────────────────────────────────────┐    │
│    │           libunwind Backend           │      │        libunwindstack Backend         │    │
│    │  - Ingests sample regs via AccessReg  │      │  - Ingests sample regs into Regs obj  │    │
│    │  - Ingests stack via AccessMem        │      │  - Ingests stack via memory overlay   │    │
│    │  - Mmap ELF Cache (add-on)            │      │  - GetOrCreateRegs in-place (add-on)  │    │
│    │  - Evaluated DWARF Rule Cache (add-on)│      │  - StackOverlayMemory (add-on)        │    │
│    │  - unw_step DWARF engine (native lib) │      │  - unwinder.Unwind() (native lib)     │    │
│    │                                       │      │  - ART Dex / JIT Handlers (native lib)│    │
│    └───────────────────┬───────────────────┘      └───────────────────┬───────────────────┘    │
│                        └───────────────────┬──────────────────────────┘                        │
│                                            │ UnwindResult (frames: std::vector<UnwindFrame>)   │
│                                            ▼                                                   │
│                        GlobalCallstackTrie::CreateCallsite(frames)                             │
└───────────────────────────────────────────┬────────────────────────────────────────────────────┘
                                            │
                    ┌───────────────────────┴───────────────────────┐
                    ▼                                               ▼
┌───────────────────────────────────────┐       ┌───────────────────────────────────────┐
│        traced_perf Emission           │       │           heapprofd Emission          │
│  - PerfProducer::EmitSample           │       │  - Bookkeeping updates live bytes/cnt │
│  - Writes PerfSample proto to trace   │       │  - Writes ProfilePacket to trace      │
└───────────────────────────────────────┘       └───────────────────────────────────────┘
```

---

### 4.5 Comprehensive Tool Comparison Matrix

| Dimension | `libunwindstack` (Baseline) | `libunwind` (Optimized) | `framehop` | `libdw` (`elfutils`) | `frame_pointer` |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Origin / Motivation** | Android OS platform unwinder | Linux system standard (`perf`, GNU) | Firefox Profiler (Mozilla) | Linux system toolchain (`perf`) | Architecture standard (ABI) |
| **Maintenance Overhead** | High on Linux (frequent sync breakages) | **Low** (Stable standard Linux API) | Medium (Dual C++/Rust build system) | High (Distribution packaging) | **Lowest** (Self-contained in Perfetto) |
| **License** | Apache 2.0 | MIT / LLVM | MIT / Apache 2.0 | **LGPLv3+ / GPLv2 (Incompatible)** | Apache 2.0 |
| **Speed: Time / Frame Unwound** | 670 ns (CPU) / 0.84 µs (Memory) | **425 ns** (CPU) / **0.51 µs** (Memory) | 473 ns (CPU) / 3.07 µs (Memory) | 811 ns (CPU) / 124.82 µs (Memory) | **~15 ns** (Direct FP) |
| **Speed: Latency / Sample** | 13.75 µs (CPU) / 9.56 µs (Memory) | **8.74 µs** (CPU) / **5.99 µs** (Memory) | 9.73 µs (CPU) / 34.26 µs (Memory) | 16.65 µs (CPU) / 1,580.64 µs (Memory) | **~0.3–0.6 µs** (Overall) |
| **Memory: Peak RSS** | **5.78 MB** (CPU) / **10.3 MB** (Memory) | 6.42 MB (CPU) / 12.1 MB (Memory) | 6.99 MB (CPU) / 13.0 MB (Memory) | 6.08 MB (CPU) / 28.9 MB (Memory) | **7.42 MB** (Micro) / 8.5 MB (Live) |
| **RSS Growth (Live Load)** | 444 KB (CPU) / **0 KB** (Memory) | **0 KB** (CPU) / **0 KB** (Memory) | 1,704 KB (CPU) / +2,700 KB (Memory) | 852 KB (CPU) / +21,600 KB (Memory) | **0 KB** |
| **ELF Caching Mechanism** | Global in-memory `Elf` object cache | Mmap whole-file cache (`base::ReadMmapWholeFile`) | Module `.eh_frame` pre-parsed index | Internal `Dwfl` module cache | **None required** |
| **`pread` / Syscall Overhead** | Stack overlay; `pread64` on miss | **Zero syscalls** (Mmap + Stack overlay) | `pread64` on remote /proc/mem | Frequent `pread64` per frame | **Zero syscalls** (Stack snapshot only) |
| **Default Gaps vs. Baseline** | *Baseline reference* | Lacks ELF cache & rule cache by default; resolved via Phase 1–3 optimizations | Lacks remote unwinder, ARM32, and C++ toolchain compatibility | Lacks Apache 2.0 license; heavy session teardown overhead | Cannot unwind frames compiled with `-fomit-frame-pointer` |

---

## 5. Benchmark Numbers & Validation

### 5.1 Native Memory Profiling Benchmark (`heapprofd`)
Evaluated on a real multi-threaded memory allocation workload (804 samples, 7,636 frames unwound, deep callstacks):

| Unwinder Backend | Samples | Frames Unwound | Error Count | Latency / Sample | Time / Frame Unwound | Total Unwind Time | Peak RSS | Symbols Resolved |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`libunwindstack` (Base)** | 804 | 7,636 | **0** | 9.56 µs | 0.84 µs | 63.9 ms | **10.3 MB** | 7,636 (100%) |
| **`libunwind` (Optimized)** | 804 | 7,636 | **0** | **5.99 µs** | **0.51 µs** | **39.3 ms** | 12.1 MB | 7,636 (100%) |
| **`framehop`** | 804 | 7,636 | **0** | 34.26 µs | 3.07 µs | 234.2 ms | 13.0 MB | 7,636 (100%) |
| **`libdw`** | 804 | 7,636 | **0** | 1,580.64 µs | 124.82 µs | 9,531.2 ms | 28.9 MB | 7,636 (100%) |

*Key Takeaways:*
- `libunwind` with Evaluated Rule Cache and Mmap ELF caching is **1.63x faster (38.5% lower latency)** than `libunwindstack` baseline, and **5.7x faster** than `framehop`.
- Exact symbol and frame parity across all backends (7,636 frames, 0 errors).

---

### 5.2 Continuous Multi-Process Stress Test (`heapprofd`)
Continuous profiling of 3 concurrent target processes running diverse workloads (deep recursion, libc `strdup` allocation churn, and rapid thread/object creation):

| Metric | `libunwindstack` (Baseline) | `libunwind` (Optimized) |
| :--- | :---: | :---: |
| **Total Unwind Samples** | 9,591 | 9,276 |
| **Total Unwind Time** | 110.98 ms | **64.80 ms** |
| **Latency per Sample** | 11.57 µs | **6.99 µs** (-39.6%) |
| **Unwind Error Count** | **0** | **0** |
| **Initial RSS** | 3.64 MB | 3.55 MB |
| **Peak RSS** | 3.64 MB | **3.55 MB** |
| **RSS Growth Under Load** | **0 KB** | **0 KB** |

---

### 5.3 Live CPU Profiling Benchmark (`traced_perf`)
10-second live CPU sampling (200 Hz, `UNWIND_DWARF`) on dedicated physical core (CPU 4) profiling concurrent C recursion (`perf_workload`) and Python interpreter workloads:

| Unwinder Backend | Samples Unwound | Total Frames Unwound | Unwind Errors | Avg Frames / Sample | Total CPU Time | Latency / Sample | Time / Frame Unwound | Peak RSS | RSS Growth |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`libunwindstack` (Base)** | 1,999 | 41,014 | 3 | 20.52 | 27.49 ms | 13.75 µs | 670 ns | **5.78 MB** | 444 KB |
| **`libunwind` (Optimized)** | 1,999 | 41,165 | **0** | 20.59 | **17.48 ms** | **8.74 µs** | **425 ns** | 6.42 MB | **0 KB** |
| **`framehop`** | 1,997 | 41,046 | **0** | 20.55 | 19.43 ms | 9.73 µs | 473 ns | 6.99 MB | 1,704 KB |
| **`libdw`** | 1,999 | 41,046 | **0** | 20.53 | 33.29 ms | 16.65 µs | 811 ns | 6.08 MB | 852 KB |

*Key Takeaways:*
- **Highest Unwinding Throughput**: `libunwind` achieves **425 ns per frame unwound** (8.74 µs per sample), making it **1.58x faster** than `libunwindstack` (670 ns/frame) and **11.3% faster than `framehop`** (473 ns/frame).
- **Unwind Reliability**: `libunwind`, `framehop`, and `libdw` achieved 0 unwind errors across ~41,000 frames, whereas `libunwindstack` encountered **3 unwind errors** during the 10-second sampling session.
- **`framehop` Re-evaluation**: While `framehop` performs significantly better than `libunwindstack` (473 ns vs 670 ns per frame), it is slower than optimized `libunwind` and suffers from **1,704 KB RSS growth** (+1.7 MB under live sampling) due to copying and parsing full ELF `.eh_frame` tables into Rust memory structures.
- **Zero Leak / Bounded Memory**: `libunwind` demonstrates **0 KB RSS growth** under continuous live CPU sampling due to bounded Evaluated Rule Cache and Mmap ELF file sharing.

---

### 5.4 Micro-benchmarks (`perfetto_benchmarks`)
Evaluated with Google Benchmark on Linux `x86_64` (Release build, `benchmark::DoNotOptimize`):

#### Cross-Backend Micro-benchmark Comparison
Full callstack unwinding across all four backends in controlled benchmark harnesses:

| Profiling Subsystem & Workload | `libunwindstack` (Base)<br>Time / Frame (Sample) | `libunwind` (Optimized)<br>Time / Frame (Sample) | `framehop`<br>Time / Frame (Sample) | `libdw`<br>Time / Frame (Sample) |
| :--- | :---: | :---: | :---: | :---: |
| **`traced_perf` (Depth 37)** | 251.7 ns (9.31 µs, baseline) | **58.7 ns** (2.17 µs, -76.7%) | 149.6 ns (5.54 µs, -40.6%) | 579.7 ns (21.4 µs, +130.3%) |
| **`heapprofd` (Depth 38)** | 304.1 ns (11.6 µs, baseline) | **114.3 ns** (4.35 µs, -62.4%) | 208.6 ns (7.93 µs, -31.4%) | 680.2 ns (25.8 µs, +123.7%) |

#### Detailed DWARF Micro-benchmarks Across Workloads (`traced_perf`)
| Benchmark Case | Baseline | `libunwindstack` | `libunwind` | `libdw` | `framehop` |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Depth10_Local** | 5.24 µs (baseline) | 5.30 µs (+1.0%) | 1.64 µs (-68.7%) | 12.2 µs (+132.1%) | 4.32 µs (-17.7%) |
| **Depth20_SharedLibDso** | 9.34 µs (baseline) | 9.21 µs (-1.4%) | 2.10 µs (-77.5%) | 21.6 µs (+130.9%) | 5.54 µs (-40.7%) |
| **Depth30_Local** | 9.93 µs (baseline) | 10.1 µs (+2.1%) | 2.12 µs (-78.7%) | 22.9 µs (+131.0%) | 5.62 µs (-43.4%) |
| **ColdStart (Depth 20)** | 2.11 ms (baseline) | 271.7 µs (-87.1%) | 4.39 µs (-99.8%) | 101.5 µs (-95.2%) | 73.4 µs (-96.5%) |
| **MemoryCache_Churn** | 7.07 µs (baseline) | 7.41 µs (+4.9%) | 1.77 µs (-75.0%) | 17.6 µs (+149.4%) | 4.59 µs (-35.0%) |

#### Detailed Native Memory Profiling Micro-benchmarks (`heapprofd`)
| Benchmark Case | Baseline | `libunwindstack` | `libunwind` | `libdw` | `framehop` |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Depth10_Local** | 5.18 µs (baseline) | 5.98 µs (+15.4%) | 2.82 µs (-45.6%) | 14.6 µs (+182.4%) | 5.65 µs (+9.1%) |
| **Depth20_SharedLibDso** | 8.93 µs (baseline) | 10.24 µs (+14.8%) | 4.57 µs (-48.8%) | 25.5 µs (+185.5%) | 7.71 µs (-13.7%) |
| **Depth30_Local** | 10.1 µs (baseline) | 11.34 µs (+12.3%) | 4.63 µs (-54.2%) | 27.9 µs (+175.9%) | 9.19 µs (-9.1%) |
| **ColdStart (Depth 20)** | 2.22 ms (baseline) | 2.17 ms (-2.5%) | 370.8 µs (-83.3%) | 26.63 ms (+1099.2%) | 57.9 µs (-97.4%) |
| **MemoryCache_Churn** | 9.06 µs (baseline) | 11.6 µs (+27.5%) | 4.94 µs (-45.5%) | 30.7 µs (+238.6%) | 7.76 µs (-14.4%) |

#### Dedicated FramePointer Micro-benchmarks
*Pure frame pointer chain traversal without DWARF CFI parsing.*

| Benchmark Case | Frames | Baseline (Before) | Abstraction (After) | Latency per Frame | Peak RSS | Net RSS Growth |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **`Depth10_Local`** | 19 frames | 1,318.1 ns (baseline) | **326.9 ns** (-75.2%) | **15.4 ns / frame** | **27.5 MB** | **0 KB** |
| **`Depth20_O2_WithFP`** | 29 frames | 2,115.7 ns (baseline) | **484.5 ns** (-77.1%) | **15.2 ns / frame** | **27.5 MB** | **0 KB** |
| **`Depth30_Local`** | 39 frames | 3,052.1 ns (baseline) | **645.1 ns** (-78.9%) | **15.0 ns / frame** | **27.5 MB** | **0 KB** |
| **`Depth20_O2_OmitFP`** | 1 frame | 158.3 ns (baseline) | **39.6 ns** (-75.0%) | — | **27.5 MB** | **0 KB** |
| **`Depth20_SharedLibDso`** | 3 frames | 273.1 ns (baseline) | **71.7 ns** (-73.7%) | — | **27.5 MB** | **0 KB** |

---

## 6. Implementation & PR Breakdown

### Proof of Concept (PoC) Reference Patches
Complete, working implementations and benchmark suites are provided via the following patches:
- **Patch 1 (Benchmarks)**: [`0001-add-benchmarks-for-traced-perf-and-heapprofd.patch`](https://paste.googleplex.com/6226625105100800) – Standalone microbenchmark harnesses and multi-backend benchmarks for `traced_perf` and `heapprofd`.
- **Patch 2 (Unwind Abstraction & Backends)**: [`0002-draft-unwind-all.patch`](https://paste.googleplex.com/4707995955625984) – Full unwinding abstraction layer (`src/profiling/unwind/`), `libunwindstack` migration, zero-dependency `frame_pointer`, high-performance `libunwind`, `framehop` Rust FFI prototype, and `libdw` backends.

### Upstream PR Breakdown
1. **PR_1: Core Abstraction Layer (`src/profiling/unwind/`)**
   - Pure interfaces: `CpuRegisters`, `Unwinder`, `ProcessUnwindContext`, `UnwindFrame`, `UnwindResult`.
   - Factory pattern: `CreateUnwinder(UnwinderType)`.
2. **PR_2: `libunwindstack` Backend Refactoring & Native `FramePointerUnwinder`**
   - Wrap existing `libunwindstack` implementation into `libunwindstack::Unwinder` and `libunwindstack::Context`.
   - Integrate in-place register recycling (`GetOrCreateRegs`).
   - Implement native `FramePointerUnwinder` with **zero external library support** (pure C++ / STL, no `libunwindstack`/`libunwind` dependencies).
3. **PR_3: Migrate `traced_perf` to Abstraction Layer**
   - Populate `unwind::CpuRegisters` directly from kernel ring buffers.
   - Remove `<unwindstack/*.h>` dependencies from `src/profiling/perf/`.
4. **PR_4: `libunwind` Remote Backend for Standalone Linux**
   - Implement `src/profiling/unwind/libunwind/` with Mmap ELF caching, zero-syscall memory reads, Evaluated Rule Cache, and bounded memory limits.
   - Set as default unwinder for standalone Linux builds.
5. **PR_5: Migrate `heapprofd` (Memory Profiling)**
   - Backend-agnostic client (`heapprofd_client`): write directly to `AllocMetadata::cpu_regs` without `libunwindstack` headers.
   - Backend-agnostic daemon: replace direct `unwindstack::Unwinder` with `unwind::Unwinder::Unwind()`.

---

## Appendix A: High-Throughput Rust FFI Prototype (`framehop`)

Located in `src/profiling/unwind/framehop/` and `src/profiling/unwind/framehop/framehop_ffi/`:
- `class framehop::Unwinder : public unwind::Unwinder`
- `class framehop::Context : public unwind::ProcessUnwindContext`
- `framehop_ffi/src/lib.rs` (Rust FFI library)

`framehop` is Mozilla's stack unwinder developed for the Firefox Profiler. It achieves high throughput (9.73 µs/sample, 473 ns/frame in live CPU profiling on dedicated CPU cores) by pre-indexing module `.eh_frame` unwind tables and operating exclusively on stack snapshots.

### 1. FFI Architecture
The Rust FFI crate (`framehop_ffi`) exposes a C-compatible interface:
- `framehop_context_create()` / `framehop_context_destroy()`
- `framehop_context_clear_cache()`
- `framehop_add_module_raw()`: Registers `.eh_frame` / `.eh_frame_hdr` section data.
- `framehop_unwind()`: Performs stack unwinding using `UnwinderX86_64` and `CacheX86_64`.

### 2. Module Indexing & Remote Handling (`Context::LoadModules`)
Because `framehop` was designed for in-process unwinding, remote unwinding requires custom translation:
- Parses `/proc/<pid>/maps` to extract loaded libraries and segment boundaries.
- Computes `base_avma` from `PT_LOAD` virtual addresses to align runtime load bias with SVMA.
- Reads module ELF headers from disk or `/proc/<pid>/mem`.
- Reads remote VDSO directly from process memory (`mem_fd_`) via `pread64`.
- Detects Linux signal trampolines (`__restore_rt`) by pattern-matching opcode bytes (`0x0f0000000fc0c748 0x05`) and reading `ucontext` at `sp + 0xa8` (IP), `sp + 0xa0` (SP), `sp + 0x78` (BP).

### 3. Caching & Eviction
- **`CacheX86_64`**: An internal, bounded direct-mapped cache (512–1024 slots) caching evaluated FDE unwinding rules.
- **Module Retention**: Parsed module indexes remain in memory until the context is destroyed.

### 4. Architectural Tradeoffs
- **Pros**: High unwinding throughput in CPU profiling (**9.73 µs/sample, 473 ns/frame**, ~1.4x faster than `libunwindstack`).
- **Cons**:
  - 11.3% slower than optimized `libunwind` (8.74 µs/sample, 425 ns/frame) in real-world dedicated-CPU `traced_perf` profiling.
  - Requires a **Rust compiler (`rustc`, `cargo`)** and FFI bridge, conflicting with Perfetto’s pure GN/Ninja C++ toolchain.
  - Supports only **x86_64 and ARM64** (no ARM32, no RISC-V).
  - Significantly higher memory footprint and RSS growth (+1.7 MB in CPU sampling vs 0 KB for `libunwind`; +2.7 MB in memory profiling).

---

## Appendix B: Linux System Integration (`libdw` / `elfutils`)

Located in `src/profiling/unwind/libdw/`:
- `class libdw::Unwinder : public unwind::Unwinder`
- `class libdw::Context : public unwind::ProcessUnwindContext`
- `libdw_accessor.h` / `libdw_accessor.cc`

Uses `elfutils`'s `libdwfl` (Dwarf Front-end Library), the standard unwinding infrastructure used by Linux `perf`:

### 1. Session Lifecycle (`Dwfl`)
- Initializes `Dwfl` session per process: `dwfl_begin(&kLibdwCallbacks)`, `dwfl_linux_proc_report(dwfl_, pid_)`, `dwfl_report_end()`.
- Unwinds via `dwfl_thread_getframes()` with a frame callback extracting PC and SP (`dwfl_frame_pc`, `dwfl_frame_reg`).
- Memory reads use `PidMemoryRead`, checking the sampled stack buffer before falling back to `pread64` on `/proc/<pid>/mem`.

### 2. Caching & Eviction
- `libdw` caches decoded CFI intervals and mapped ELF segments internally within `Dwfl`.
- Flushed by tearing down and recreating the `Dwfl` handle (`TeardownDwfl() / SetupDwfl()`).

### 3. Architectural Tradeoffs
- **Pros**: Solid performance in CPU sampling (**16.2 µs/sample**), robust Linux kernel/system integration.
- **Cons**:
  - **Licensing barrier**: `elfutils` is LGPLv3+ / GPLv2, which is incompatible with Perfetto's Apache 2.0 core upstream license.
  - High latency in memory profiling (**916 µs/sample**) due to heavy per-unwind session teardown/recreation overhead.
