# Independent unwinding library in Perfetto without external dependency on android/libunwindstack

**Authors:** @safayat-google

**Status:** Draft

## Problem

Currently, unwinding in Perfetto is based on the `android-unwinding/libunwindstack` library. This is a dependency that we want to remove for several reasons:
- Changes to the `libunwindstack` API break our builds.
- Extra memory overhead in `libunwindstack`'s implementation does not benefit us.

## Decision

Pending

## Design

* Start with the same architecture and memory layout as `libunwindstack`.
* Use the same names for the types/classes; however, only keep the APIs that we are currently using.
* Use the `perfetto::profiling` namespace for the new types/classes.
   * Note: Before the final migration, use the `perfetto::profiling::unwinding` namespace to avoid build breaks.
* Optimize the memory usage of the unwinding implementation based on `heapprofd` and `traced_perf` usage patterns.

### Folder Structure
```text
repo-root/
└── src/
    └── profiling/
        └── common/
            └── unwinding/
                ├── Regs.h
                ├── Memory.h
                ├── Maps.h
                ├── Elf.h
                └── Unwinder.h
```

### Splitting the changes into multiple PRs
#### Phase #1: Remove libunwindstack headers while keeping the logic as is
    1.1 Implement Regs.h API
    1.2 Implement Memory & Maps API
    1.3 Implement Elf.h API
    1.4 Implement the Unwinder API
#### Phase #2: Migrate usage to the new API
    2.1 Migrate heapprofd unwinding usage
    2.2 Migrate traced_perf unwinding usage
#### Phase #3: Refactor / optimize implementation based on our needs while optimizing memory usage (TBD)
    3.1 Memory usage optimization
        - Remove any ELF-related memory overhead that we do not need.
    3.2 Other feature or CPU/memory optimizations

### Core APIs we currently use from libunwindstack
- `unwindstack/Unwinder.h`
```cpp
  struct FrameData { num, rel_pc, pc, sp, function_name, offset, map_info }
  ...

  class Unwinder {
    Unwinder(size_t max_frames, Maps* maps, Regs* regs, std::shared_ptr<Memory> process_memory);
    ...
    virtual void Unwind(initial_map_names_to_skip, map_suffixes_to_ignore);
    ...
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

  // local unwinding
  CreateProcessMemoryCached(pid);
  // remote unwinding
  CreateProcessMemoryThreadCached(pid);
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

- `unwindstack/JitDebug.h` and `unwindstack/DexFiles.h`
```cpp
CreateJitDebug(arch, memory, search_libs = {});
CreateDexFiles(arch, memory, search_libs = {});
```

## Alternatives considered

### A wrapper around libunwindstack

* An abstraction over `libunwindstack` that internally depends on `libunwindstack`.
* The rest of our codebase calls this abstraction instead of `libunwindstack` directly.

Pro:

* We still benefit from `libunwindstack`'s active development.
* If an API changes in `libunwindstack`, we only need to update our abstraction layer.

Con:

* It won't fix the memory overhead of `libunwindstack`.
* We still need to deal with build breaks on upstream API changes from `libunwindstack`.

### Write everything from scratch

Pro:

* We can optimize and apply best practices from the start.

Con:

* It will require more design, review, and implementation effort.
* Higher risk of introducing regressions in production or requiring longer timelines with complex feature flag maintenance.

