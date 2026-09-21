# RFC-0048: Linux Unwinding Abstraction Layer for traced_perf and heapprofd

**Authors:** @safayat-google  
**Status:** In Review / Updated Draft  
**Discussion:** https://github.com/google/perfetto/discussions/7283  

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

### 4.3 High-Throughput Rust FFI Prototype: `framehop`

Located in `src/profiling/unwind/framehop/` and `src/profiling/unwind/framehop/framehop_ffi/`:
- `class framehop::Unwinder : public unwind::Unwinder`
- `class framehop::Context : public unwind::ProcessUnwindContext`
- `framehop_ffi/src/lib.rs` (Rust FFI library)

`framehop` is Mozilla's stack unwinder developed for the Firefox Profiler. It achieves exceptional throughput (~8.5 µs/sample in CPU profiling) by pre-indexing module `.eh_frame` unwind tables and operating exclusively on stack snapshots.

#### 1. FFI Architecture
The Rust FFI crate (`framehop_ffi`) exposes a C-compatible interface:
- `framehop_context_create()` / `framehop_context_destroy()`
- `framehop_context_clear_cache()`
- `framehop_add_module_raw()`: Registers `.eh_frame` / `.eh_frame_hdr` section data.
- `framehop_unwind()`: Performs stack unwinding using `UnwinderX86_64` and `CacheX86_64`.

#### 2. Module Indexing & Remote Handling (`Context::LoadModules`)
Because `framehop` was designed for in-process unwinding, remote unwinding requires custom translation:
- Parses `/proc/<pid>/maps` to extract loaded libraries and segment boundaries.
- Computes `base_avma` from `PT_LOAD` virtual addresses to align runtime load bias with SVMA.
- Reads module ELF headers from disk or `/proc/<pid>/mem`.
- Reads remote VDSO directly from process memory (`mem_fd_`) via `pread64`.
- Detects Linux signal trampolines (`__restore_rt`) by pattern-matching opcode bytes (`0x0f0000000fc0c748 0x05`) and reading `ucontext` at `sp + 0xa8` (IP), `sp + 0xa0` (SP), `sp + 0x78` (BP).

#### 3. Caching & Eviction
- **`CacheX86_64`**: An internal, bounded direct-mapped cache (512–1024 slots) caching evaluated FDE unwinding rules.
- **Module Retention**: Parsed module indexes remain in memory until the context is destroyed.

#### 4. Architectural Tradeoffs
- **Pros**: Fastest unwinder evaluated in CPU profiling (**~8.5 µs/sample**, ~2x faster than `libunwindstack`).
- **Cons**: 
  - Requires a **Rust compiler (`rustc`, `cargo`)** and FFI bridge, conflicting with Perfetto’s pure GN/Ninja C++ toolchain.
  - Supports only **x86_64 and ARM64** (no ARM32, no RISC-V).
  - Higher memory footprint (+1.2 MB growth in CPU sampling, +2.7 MB in memory profiling).

---

### 4.4 Linux System Integration: `libdw` (elfutils)

Located in `src/profiling/unwind/libdw/`:
- `class libdw::Unwinder : public unwind::Unwinder`
- `class libdw::Context : public unwind::ProcessUnwindContext`
- `libdw_accessor.h` / `libdw_accessor.cc`

Uses `elfutils`'s `libdwfl` (Dwarf Front-end Library), the standard unwinding infrastructure used by Linux `perf`:

#### 1. Session Lifecycle (`Dwfl`)
- Initializes `Dwfl` session per process: `dwfl_begin(&kLibdwCallbacks)`, `dwfl_linux_proc_report(dwfl_, pid_)`, `dwfl_report_end()`.
- Unwinds via `dwfl_thread_getframes()` with a frame callback extracting PC and SP (`dwfl_frame_pc`, `dwfl_frame_reg`).
- Memory reads use `PidMemoryRead`, checking the sampled stack buffer before falling back to `pread64` on `/proc/<pid>/mem`.

#### 2. Caching & Eviction
- `libdw` caches decoded CFI intervals and mapped ELF segments internally within `Dwfl`.
- Flushed by tearing down and recreating the `Dwfl` handle (`TeardownDwfl() / SetupDwfl()`).

#### 3. Architectural Tradeoffs
- **Pros**: Solid performance in CPU sampling (**16.2 µs/sample**), robust Linux kernel/system integration.
- **Cons**: 
  - **Licensing barrier**: `elfutils` is LGPLv3+ / GPLv2, which is incompatible with Perfetto's Apache 2.0 core upstream license.
  - High latency in memory profiling (**916 µs/sample**) due to heavy per-unwind session teardown/recreation overhead.

---

### 4.5 Built-in Frame Pointer Unwinder: Zero External Library Dependencies (`frame_pointer`)

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
│ - Reads only stack buffer │     ┌─────────────────────────┼─────────────────────────┐
│ - Needs only (fp, sp, pc) │     ▼                         ▼                         ▼
│ - No mem_fd required      │ ┌───────────────────────┐ ┌───────────────────────┐ ┌───────────────────────┐
└───────────────────────────┘ │ LibunwindstackUnwinder│ │   LibunwindUnwinder   │ │    FramehopUnwinder   │
                              │ (Android + ART/Dex)   │ │ (Linux Standalone)    │ │ (High-Throughput FFI) │
                              └───────────────────────┘ └───────────────────────┘ └───────────────────────┘
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

### 4.6 End-to-End Unified Architecture & Data Flow Diagram

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

### 4.7 Comprehensive Tool Comparison Matrix

| Dimension | `libunwindstack` (Baseline) | `libunwind` (Optimized) | `framehop` | `libdw` (`elfutils`) | `frame_pointer` |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Origin / Motivation** | Android OS platform unwinder | Linux system standard (`perf`, GNU) | Firefox Profiler (Mozilla) | Linux system toolchain (`perf`) | Architecture standard (ABI) |
| **Language & Integration** | C++ (Checked in `buildtools/`) | C / C++ (Modular C callbacks) | **Rust** (Requires FFI bridge & `cargo`) | C (Dynamic library) | Pure C++ (Zero external deps) |
| **Maintenance Overhead** | High on Linux (frequent sync breakages) | **Low** (Stable standard Linux API) | Medium (Dual C++/Rust build system) | High (Distribution packaging) | **Lowest** (Self-contained in Perfetto) |
| **License** | Apache 2.0 | MIT / LLVM | MIT / Apache 2.0 | **LGPLv3+ / GPLv2 (Incompatible)** | Apache 2.0 |
| **Arch Support** | ARM, ARM64, x86, x86_64, RISC-V | ARM, ARM64, x86, x86_64, RISC-V | x86_64, ARM64 only | ARM, ARM64, x86, x86_64 | Universal (x86_64, ARM64, RISC-V) |
| **Android ART / Dex / JIT** | **Yes (Full native support)** | No | No | No | No |
| **Speed: Latency / Sample** | 10.46 µs (Memory) / 16.83 µs (CPU) | **5.64 µs** (Memory) / 20.00 µs (CPU) | 34.26 µs (Memory) / **8.53 µs** (CPU) | 916.43 µs (Memory) / 16.16 µs (CPU) | **~1.5–3.4 µs** (Overall) |
| **Memory: Peak RSS** | 12.1 MB (Memory) / 5.98 MB (CPU) | 13.6 MB (Memory) / **5.87 MB** (CPU) | 14.8 MB (Memory) / 6.32 MB (CPU) | 33.7 MB (Memory) / 6.19 MB (CPU) | **7.42 MB** (Micro) / 8.5 MB (Live) |
| **RSS Growth (Live Load)** | 600 KB (CPU) / **0 KB** (Memory) | **296 KB** (CPU) / **0 KB** (Memory) | 1,276 KB (CPU) / +2.7 MB (Memory) | 896 KB (CPU) / +21.6 MB (Memory) | **0 KB** |
| **ELF Caching Mechanism** | Global in-memory `Elf` object cache | Mmap whole-file cache (`base::ReadMmapWholeFile`) | Module `.eh_frame` pre-parsed index | Internal `Dwfl` module cache | **None required** |
| **`pread` / Syscall Overhead** | Stack overlay; `pread64` on miss | **Zero syscalls** (Mmap + Stack overlay) | `pread64` on remote /proc/mem | Frequent `pread64` per frame | **Zero syscalls** (Stack snapshot only) |
| **Default Gaps vs. Baseline** | *Baseline reference* | Lacks ELF cache & rule cache by default; resolved via Phase 1–3 optimizations | Lacks remote unwinder, ARM32, and C++ toolchain compatibility | Lacks Apache 2.0 license; heavy session teardown overhead | Cannot unwind frames compiled with `-fomit-frame-pointer` |

---

## 5. Benchmark Numbers & Validation

### 5.1 Native Memory Profiling Benchmark (`heapprofd`)
Evaluated on a real multi-threaded memory allocation workload (804 samples, 7,636 frames unwound, deep callstacks):

| Unwinder Backend | Samples | Frames Unwound | Error Count | Latency / Sample | Latency / Frame | Total Unwind Time | Peak RSS | Symbols Resolved |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`libunwind` (Optimized)** | 804 | 7,636 | **0** | **5.64 µs** | **0.59 µs** | **36.9 ms** | 13.6 MB | 7,636 (100%) |
| **`libunwindstack` (Base)** | 804 | 7,636 | **0** | 10.46 µs | 1.10 µs | 68.3 ms | **12.1 MB** | 7,636 (100%) |
| **`framehop`** | 804 | 7,636 | **0** | 34.26 µs | 3.61 µs | 224.4 ms | 14.8 MB | 7,636 (100%) |
| **`libdw`** | 804 | 7,636 | **0** | 916.43 µs | 96.53 µs | 6003.5 ms | 33.7 MB | 7,636 (100%) |

*Key Takeaways:*
- `libunwind` with Evaluated Rule Cache and Mmap ELF caching is **1.85x faster (85% speedup)** than `libunwindstack` baseline.
- Exact symbol and frame parity across all backends (7,636 frames, 0 errors).

---

### 5.2 Continuous Multi-Process Stress Test (`heapprofd`)
Continuous profiling of 3 concurrent target processes running diverse workloads (deep recursion, libc `strdup` allocation churn, and rapid thread/object creation):

| Metric | `libunwind` (Optimized) | `libunwindstack` (Baseline) | Difference |
| :--- | :---: | :---: | :---: |
| **Total Unwind Samples** | 8,974 | 9,339 | — |
| **Total Unwind Time** | **60.96 ms** | 117.32 ms | **-48.0% CPU Time** |
| **Latency per Sample** | **6.79 µs** | 12.56 µs | **1.85x Faster** |
| **Unwind Error Count** | **0** | **0** | Parity |
| **Initial RSS** | 3,612 KB (3.53 MB) | 3,756 KB (3.67 MB) | -144 KB |
| **Peak RSS** | **3,612 KB (3.53 MB)** | 3,756 KB (3.67 MB) | **-144 KB** |
| **RSS Growth Under Load** | **0 KB** | **0 KB** | Zero Leak / Bounded |

---

### 5.3 Live CPU Profiling Benchmark (`traced_perf`)
10-second live CPU sampling (200 Hz, `UNWIND_DWARF`) on dedicated physical cores profiling concurrent C recursion and Python interpreter workloads:

| Unwinder Backend | Samples | Frames | Avg Frames / Sample | Total CPU Time | Latency / Sample | Peak RSS | RSS Growth |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`framehop`** | 1,999 | 41,193 | 20.61 | **17.05 ms** | **8.53 µs** | 6.33 MB | 1,276 KB |
| **`libdw`** | 1,995 | 41,153 | 20.63 | 32.25 ms | 16.16 µs | 6.19 MB | 896 KB |
| **`libunwindstack`** | 1,999 | 41,170 | 20.60 | 33.64 ms | 16.83 µs | 5.98 MB | 600 KB |
| **`libunwind`** | 1,999 | 41,086 | 20.55 | 39.97 ms | 20.00 µs | **5.88 MB** | **296 KB** |

---

### 5.4 Micro-benchmarks (`perfetto_benchmarks`)
Evaluated with Google Benchmark on Linux `x86_64` (Release build, `benchmark::DoNotOptimize`):

| Benchmark Case | `libunwindstack` (Base)<br>CPU / Peak RSS | `libunwind`<br>CPU / Peak RSS | `framehop`<br>CPU / Peak RSS | `frame_pointer`<br>CPU / Peak RSS |
| :--- | :---: | :---: | :---: | :---: |
| **DWARF Depth 10 (Local)** | 4,744 ns / 8.5 MB | **3,517 ns (-25.9%)** / 8.3 MB | 4,818 ns (+1.6%) / 9.4 MB | **207 ns (-95.6%)** / 7.4 MB |
| **DWARF Depth 30 (Local)** | 9,285 ns / 8.5 MB | 5,408 ns (-41.8%) / 8.3 MB | **5,067 ns (-45.4%)** / 9.4 MB | **400 ns (-95.7%)** / 7.4 MB |
| **DWARF Depth 20 (SharedLib DSO)** | 8,618 ns / 8.5 MB | **5,043 ns (-41.5%)** / 8.4 MB | 6,469 ns (-24.9%) / 9.4 MB | 60 ns (Partial) / 7.4 MB |
| **DWARF Depth 20 (Signal Handler)**| 8,322 ns / 8.7 MB | 4,855 ns (-41.7%) / 8.4 MB | **4,129 ns (-50.4%)** / 9.4 MB | N/A |
| **DWARF Depth 20 (Cold Start)** | 259,266 ns / 9.6 MB | 49,873 ns (-80.8%) / 8.5 MB | **23,177 ns (-91.1%)** / 10.4 MB | **45 ns** / 7.4 MB |

#### FramePointer Dedicated Micro-benchmarks
*Pure frame pointer chain traversal without DWARF CFI parsing.*

| Benchmark Case | Frames Unwound | CPU Time (ns) | Real Time (ns) | Latency per Frame | Peak RSS | Net RSS Growth |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **`Depth10_Local`** | 19 frames | **206.8 ns** | 206.8 ns | **10.8 ns / frame** | **7,424 KB** | **0 KB** |
| **`Depth20_O2_WithFP`** | 29 frames | **301.9 ns** | 301.9 ns | **10.4 ns / frame** | **7,424 KB** | **0 KB** |
| **`Depth30_Local`** | 39 frames | **400.4 ns** | 400.4 ns | **10.2 ns / frame** | **7,424 KB** | **0 KB** |
| **`Depth20_O2_OmitFP`** | 1 frame | **44.7 ns** | 44.7 ns | — | **7,424 KB** | **0 KB** |
| **`Depth20_SharedLibDso`** | 3 frames | **60.1 ns** | 60.1 ns | — | **7,424 KB** | **0 KB** |

---

## 6. Implementation & PR Breakdown

1. **PR #1: Core Abstraction Layer (`src/profiling/unwind/`)**
   - Pure interfaces: `CpuRegisters`, `Unwinder`, `ProcessUnwindContext`, `UnwindFrame`, `UnwindResult`.
   - Factory pattern: `CreateUnwinder(UnwinderType)`.
2. **PR #2: `libunwindstack` Backend Refactoring & Native `FramePointerUnwinder`**
   - Wrap existing `libunwindstack` implementation into `libunwindstack::Unwinder` and `libunwindstack::Context`.
   - Integrate in-place register recycling (`GetOrCreateRegs`).
   - Implement native `FramePointerUnwinder` with **zero external library support** (pure C++ / STL, no `libunwindstack`/`libunwind` dependencies).
3. **PR #3: Migrate `traced_perf` to Abstraction Layer**
   - Populate `unwind::CpuRegisters` directly from kernel ring buffers.
   - Remove `<unwindstack/*.h>` dependencies from `src/profiling/perf/`.
4. **PR #4: `libunwind` Remote Backend for Standalone Linux**
   - Implement `src/profiling/unwind/libunwind/` with Mmap ELF caching, zero-syscall memory reads, Evaluated Rule Cache, and bounded memory limits.
   - Set as default unwinder for standalone Linux builds.
5. **PR #5: Migrate `heapprofd` (Memory Profiling)**
   - Backend-agnostic client (`heapprofd_client`): write directly to `AllocMetadata::cpu_regs` without `libunwindstack` headers.
   - Backend-agnostic daemon: replace direct `unwindstack::Unwinder` with `unwind::Unwinder::Unwind()`.
