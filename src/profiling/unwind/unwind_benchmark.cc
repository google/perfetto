/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <benchmark/benchmark.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <unwindstack/Elf.h>
#include <unwindstack/Unwinder.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/common/unwind_support.h"
#include "src/profiling/perf/frame_pointer_unwinder.h"
#include "src/profiling/unwind/cpu_registers.h"
#include "src/profiling/unwind/get_asm_regs.h"

namespace perfetto {
namespace profiling {
namespace {

constexpr size_t kMaxFrames = 64;
constexpr size_t kMaxStackDumpBytes = 65536;  // 64kb

struct MemoryStatus {
  uint64_t vmrss_kb = 0;
  uint64_t vmhwm_kb = 0;  // Peak RSS
};

MemoryStatus ReadMemoryStatus() {
  MemoryStatus status;
  base::ScopedFstream fp(fopen("/proc/self/status", "r"));
  if (!fp)
    return status;
  char line[512];
  int found = 0;
  while (fgets(line, sizeof(line), fp.get())) {
    if (sscanf(line, "VmRSS: %" PRIu64 " kB", &status.vmrss_kb) == 1) {
      if (++found == 2)
        break;
    } else if (sscanf(line, "VmHWM: %" PRIu64 " kB", &status.vmhwm_kb) == 1) {
      if (++found == 2)
        break;
    }
  }
  return status;
}

template <typename T>
PERFETTO_ALWAYS_INLINE void PreventTailCall(T& val) {
  asm volatile("" : "+r"(val));
}

template <size_t Stage = 1>
PERFETTO_NO_INLINE void CallStackStage(size_t target_depth,
                                       const std::function<void()>& cb) {
  if (Stage >= target_depth) {
    cb();
    return;
  }
  if constexpr (Stage < 64) {
    CallStackStage<Stage + 1>(target_depth, cb);
    PreventTailCall(target_depth);
  } else {
    cb();
  }
}

PERFETTO_NO_INLINE void GenerateCallStack(int depth,
                                          const std::function<void()>& cb) {
  if (depth <= 0) {
    cb();
    return;
  }
  CallStackStage<1>(static_cast<size_t>(depth), cb);
}

// Calls through a system shared library (.so) via libc qsort
static thread_local bool g_qsort_cb_invoked = false;

struct QsortItem {
  int key;
  const std::function<void()>* cb;
};

static int QsortComparator(const void* a, const void* b) {
  const auto* item_a = static_cast<const QsortItem*>(a);
  const auto* item_b = static_cast<const QsortItem*>(b);
  if (!g_qsort_cb_invoked && item_a->cb && *item_a->cb) {
    g_qsort_cb_invoked = true;
    (*item_a->cb)();
  }
  return (item_a->key > item_b->key) - (item_a->key < item_b->key);
}

template <size_t Stage = 1>
PERFETTO_NO_INLINE void CallStackStage_SystemLib(
    size_t target_depth,
    const std::function<void()>& cb) {
  if (Stage >= target_depth) {
    g_qsort_cb_invoked = false;
    QsortItem items[4] = {
        {40, &cb}, {10, nullptr}, {30, nullptr}, {20, nullptr}};
    qsort(items, 4, sizeof(QsortItem), QsortComparator);
    return;
  }
  if constexpr (Stage < 64) {
    CallStackStage_SystemLib<Stage + 1>(target_depth, cb);
    PreventTailCall(target_depth);
  } else {
    cb();
  }
}

PERFETTO_NO_INLINE void GenerateCallStack_SystemLib(
    int depth,
    const std::function<void()>& cb) {
  if (depth <= 0) {
    cb();
    return;
  }
  CallStackStage_SystemLib<1>(static_cast<size_t>(depth), cb);
}

struct SampleSnapshot {
  CpuRegisters regs;
  std::vector<uint8_t> stack_bytes;
  uint64_t sp = 0;
};

// Copies raw stack bytes across frames without triggering ASan stack-redzone
// checks or compiler loop-to-memcpy replacement.
PERFETTO_NO_INLINE void CopyStackMemory(uint8_t* dst,
                                        const volatile uint8_t* src,
                                        size_t size)
    __attribute__((no_sanitize("address", "hwaddress", "memory"))) {
  for (size_t i = 0; i < size; ++i) {
    dst[i] = src[i];
  }
}

SampleSnapshot CaptureCurrentStack() {
  SampleSnapshot sample;
  alignas(8) uintptr_t reg_data[CpuRegisters::kMaxRegs] = {};
  GetAsmRegs(reg_data);
  sample.regs = CpuRegisters(CurrentCpuArch());
  for (size_t i = 0; i < CpuRegisters::kMaxRegs; ++i) {
    sample.regs[i] = reg_data[i];
  }
  sample.sp = sample.regs.sp();

  uintptr_t sp = static_cast<uintptr_t>(sample.sp);
  uintptr_t stack_top = 0;

  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* stack_base = nullptr;
    size_t stack_size = 0;
    if (pthread_attr_getstack(&attr, &stack_base, &stack_size) == 0) {
      stack_top = reinterpret_cast<uintptr_t>(stack_base) + stack_size;
    }
    pthread_attr_destroy(&attr);
  }

  size_t bytes_available = (stack_top > sp) ? (stack_top - sp) : 4096;
  size_t bytes_to_copy = std::min(bytes_available, kMaxStackDumpBytes);

  sample.stack_bytes.resize(bytes_to_copy);
  CopyStackMemory(sample.stack_bytes.data(),
                  reinterpret_cast<const volatile uint8_t*>(sp), bytes_to_copy);

  return sample;
}

class UnwindBenchmarkFixture {
 public:
  explicit UnwindBenchmarkFixture(int callstack_depth,
                                  bool through_system_lib = false) {
    std::function<void()> capture = [this] { sample_ = CaptureCurrentStack(); };
    if (through_system_lib) {
      GenerateCallStack_SystemLib(callstack_depth, capture);
    } else {
      GenerateCallStack(callstack_depth, capture);
    }

    base::ScopedFile maps_fd(open("/proc/self/maps", O_RDONLY));
    base::ScopedFile mem_fd(open("/proc/self/mem", O_RDONLY));
    PERFETTO_CHECK(maps_fd.get() >= 0);
    PERFETTO_CHECK(mem_fd.get() >= 0);

    metadata_ = std::make_unique<UnwindingMetadata>(std::move(maps_fd),
                                                    std::move(mem_fd));
    overlay_memory_ = std::make_shared<StackOverlayMemory>(
        metadata_->fd_mem, sample_.regs.sp(), sample_.stack_bytes.data(),
        sample_.stack_bytes.size());
  }

  std::vector<unwindstack::FrameData> Unwind(bool resolve_names = true) {
    std::unique_ptr<unwindstack::Regs> regs =
        ToLibUnwindstackRegs(sample_.regs);
    PERFETTO_DCHECK(regs != nullptr);
    unwindstack::Unwinder unwinder(kMaxFrames, &metadata_->fd_maps, regs.get(),
                                   overlay_memory_);
    unwinder.SetResolveNames(resolve_names);
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
    unwinder.SetJitDebug(metadata_->GetJitDebug(regs->Arch()));
    unwinder.SetDexFiles(metadata_->GetDexFiles(regs->Arch()));
#endif
    unwinder.Unwind(/*initial_map_names_to_skip=*/nullptr,
                    /*map_suffixes_to_ignore=*/nullptr);
    return unwinder.ConsumeFrames();
  }

  const SampleSnapshot& sample() const { return sample_; }
  UnwindingMetadata* metadata() { return metadata_.get(); }
  std::shared_ptr<unwindstack::Memory> overlay_memory() {
    return overlay_memory_;
  }

 private:
  SampleSnapshot sample_;
  std::unique_ptr<UnwindingMetadata> metadata_;
  std::shared_ptr<unwindstack::Memory> overlay_memory_;
};

void RecordMetrics(benchmark::State& state,
                   const MemoryStatus& start_mem,
                   size_t total_frames,
                   size_t error_count) {
  MemoryStatus end_mem = ReadMemoryStatus();
  double avg_frames = static_cast<double>(total_frames) /
                      static_cast<double>(state.iterations());
  state.counters["frames_unwound"] = benchmark::Counter(avg_frames);
  state.counters["unwind_errors"] =
      benchmark::Counter(static_cast<double>(error_count));
  state.counters["peak_rss_kb"] =
      benchmark::Counter(static_cast<double>(end_mem.vmhwm_kb));
  state.counters["rss_growth_kb"] = benchmark::Counter(
      end_mem.vmrss_kb > start_mem.vmrss_kb
          ? static_cast<double>(end_mem.vmrss_kb - start_mem.vmrss_kb)
          : 0.0);
}

struct UnwindArgs {
  int depth;
  bool through_system_lib = false;
  bool resolve_names = false;
};

// Benchmark DWARF unwinding
void BM_Unwind_Dwarf(benchmark::State& state, const UnwindArgs& args) {
  UnwindBenchmarkFixture fixture(args.depth, args.through_system_lib);
  MemoryStatus start_mem = ReadMemoryStatus();

  size_t total_frames = 0;
  size_t error_count = 0;
  for (auto _ : state) {
    std::vector<unwindstack::FrameData> frames =
        fixture.Unwind(args.resolve_names);
    if (frames.empty()) {
      error_count++;
    }
    std::vector<std::string> build_ids;
    build_ids.reserve(frames.size());
    for (const auto& frame : frames) {
      build_ids.emplace_back(fixture.metadata()->GetBuildId(frame));
    }

    total_frames += frames.size();
    benchmark::DoNotOptimize(total_frames);
    benchmark::DoNotOptimize(build_ids);
  }

  RecordMetrics(state, start_mem, total_frames, error_count);
}

// Benchmark Frame Pointer unwinding
void BM_Unwind_FramePointer(benchmark::State& state, int depth) {
  UnwindBenchmarkFixture fixture(depth);
  MemoryStatus start_mem = ReadMemoryStatus();

  size_t total_frames = 0;
  size_t error_count = 0;
  for (auto _ : state) {
    CpuRegisters regs_copy = fixture.sample().regs;
    FramePointerUnwinder unwinder(kMaxFrames, &fixture.metadata()->fd_maps,
                                  &regs_copy, fixture.overlay_memory(),
                                  fixture.sample().stack_bytes.size());
    unwinder.Unwind();

    std::vector<unwindstack::FrameData> frames = unwinder.ConsumeFrames();
    if (frames.empty()) {
      error_count++;
    }
    std::vector<std::string> build_ids;
    build_ids.reserve(frames.size());
    for (const auto& frame : frames) {
      build_ids.emplace_back(fixture.metadata()->GetBuildId(frame));
    }

    total_frames += frames.size();
    benchmark::DoNotOptimize(total_frames);
    benchmark::DoNotOptimize(build_ids);
  }

  RecordMetrics(state, start_mem, total_frames, error_count);
}

// Benchmark cold-cache DWARF unwinding (first sample / cache flush)
void BM_Unwind_Dwarf_ColdStart(benchmark::State& state, int depth) {
  UnwindBenchmarkFixture fixture(depth);
  MemoryStatus start_mem = ReadMemoryStatus();

  size_t total_frames = 0;
  size_t error_count = 0;
  for (auto _ : state) {
    state.PauseTiming();
    fixture.metadata()->ReparseMaps();
    unwindstack::Elf::SetCachingEnabled(false);
    unwindstack::Elf::SetCachingEnabled(true);
    state.ResumeTiming();

    std::vector<unwindstack::FrameData> frames = fixture.Unwind();
    if (frames.empty()) {
      error_count++;
    }
    total_frames += frames.size();
    benchmark::DoNotOptimize(total_frames);
  }

  RecordMetrics(state, start_mem, total_frames, error_count);
}

// Benchmark memory stability over thousands of unwinds
void BM_Unwind_MemoryCache_Churn(benchmark::State& state) {
  UnwindBenchmarkFixture fixture(20, /*through_system_lib=*/true);
  MemoryStatus start_mem = ReadMemoryStatus();

  size_t count = 0;
  for (auto _ : state) {
    std::vector<unwindstack::FrameData> frames = fixture.Unwind();
    benchmark::DoNotOptimize(frames);
    count++;
  }

  MemoryStatus end_mem = ReadMemoryStatus();
  state.counters["samples_unwound"] =
      benchmark::Counter(static_cast<double>(count));
  state.counters["peak_rss_kb"] =
      benchmark::Counter(static_cast<double>(end_mem.vmhwm_kb));
  state.counters["rss_growth_kb"] = benchmark::Counter(
      end_mem.vmrss_kb > start_mem.vmrss_kb
          ? static_cast<double>(end_mem.vmrss_kb - start_mem.vmrss_kb)
          : 0.0);
}

// DWARF benchmarks without symbol resolution
BENCHMARK_CAPTURE(BM_Unwind_Dwarf, Depth10_Local, UnwindArgs{10, false, false});
BENCHMARK_CAPTURE(BM_Unwind_Dwarf, Depth30_Local, UnwindArgs{30, false, false});
BENCHMARK_CAPTURE(BM_Unwind_Dwarf,
                  Depth20_SharedLibDso,
                  UnwindArgs{20, true, false});

// DWARF benchmarks with symbol resolution
BENCHMARK_CAPTURE(BM_Unwind_Dwarf,
                  Depth10_Local_WithNames,
                  UnwindArgs{10, false, true});
BENCHMARK_CAPTURE(BM_Unwind_Dwarf,
                  Depth30_Local_WithNames,
                  UnwindArgs{30, false, true});
BENCHMARK_CAPTURE(BM_Unwind_Dwarf,
                  Depth20_SharedLibDso_WithNames,
                  UnwindArgs{20, true, true});

// ColdStart & Churn
BENCHMARK_CAPTURE(BM_Unwind_Dwarf_ColdStart, Depth20, 20)->Iterations(100);
BENCHMARK(BM_Unwind_MemoryCache_Churn)->Iterations(5000);

// FramePointer benchmarks
BENCHMARK_CAPTURE(BM_Unwind_FramePointer, Depth10_Local, 10);
BENCHMARK_CAPTURE(BM_Unwind_FramePointer, Depth30_Local, 30);

}  // namespace
}  // namespace profiling
}  // namespace perfetto
