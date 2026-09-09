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
#include <functional>
#include <memory>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/asm_get_regs.h"
#include "src/profiling/unwind/cpu_registers.h"
#include "src/profiling/unwind/unwind_context.h"
#include "src/profiling/unwind/unwind_types.h"

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

SampleSnapshot CaptureCurrentStack() {
  SampleSnapshot sample;
  uint64_t reg_data[CpuRegisters::kMaxRegs] = {};
  AsmGetRegs(reg_data);
  sample.regs = CpuRegisters::FromUserRegs(CurrentCpuArch(), reg_data);
  sample.sp = sample.regs.sp;

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
  memcpy(sample.stack_bytes.data(), reinterpret_cast<const void*>(sp),
         bytes_to_copy);

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

    unwind_context_ =
        UnwindContext::Create(std::move(maps_fd), std::move(mem_fd));
    unwind_context_->ReparseMaps();
  }

  const SampleSnapshot& sample() const { return sample_; }
  UnwindContext* unwind_context() { return unwind_context_.get(); }

 private:
  SampleSnapshot sample_;
  std::unique_ptr<UnwindContext> unwind_context_;
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

  UnwindInputSample sample;
  sample.regs = &fixture.sample().regs;
  sample.stack_data = fixture.sample().stack_bytes.data();
  sample.stack_size = fixture.sample().stack_bytes.size();

  UnwindOptions options;
  options.max_frames = kMaxFrames;
  options.resolve_names = args.resolve_names;

  size_t total_frames = 0;
  size_t error_count = 0;
  for (auto _ : state) {
    UnwindResult result = fixture.unwind_context()->Unwind(sample, options);
    if (result.frames.empty()) {
      error_count++;
    }
    std::vector<std::string> build_ids;
    build_ids.reserve(result.frames.size());
    for (const auto& frame : result.frames) {
      build_ids.emplace_back(
          frame.map_info.has_value() ? frame.map_info->build_id : "");
    }

    total_frames += result.frames.size();
    benchmark::DoNotOptimize(total_frames);
    benchmark::DoNotOptimize(build_ids);
  }

  RecordMetrics(state, start_mem, total_frames, error_count);
}

// Benchmark cold-cache DWARF unwinding (first sample / cache flush)
void BM_Unwind_Dwarf_ColdStart(benchmark::State& state, int depth) {
  UnwindBenchmarkFixture fixture(depth);
  MemoryStatus start_mem = ReadMemoryStatus();

  UnwindInputSample sample;
  sample.regs = &fixture.sample().regs;
  sample.stack_data = fixture.sample().stack_bytes.data();
  sample.stack_size = fixture.sample().stack_bytes.size();

  UnwindOptions options;
  options.max_frames = kMaxFrames;

  size_t total_frames = 0;
  size_t error_count = 0;
  for (auto _ : state) {
    state.PauseTiming();
    fixture.unwind_context()->ReparseMaps();
    UnwindContext::ResetAndEnableGlobalCache();
    state.ResumeTiming();

    UnwindResult result = fixture.unwind_context()->Unwind(sample, options);
    if (result.frames.empty()) {
      error_count++;
    }
    total_frames += result.frames.size();
    benchmark::DoNotOptimize(total_frames);
  }

  RecordMetrics(state, start_mem, total_frames, error_count);
}

// Benchmark memory stability over thousands of unwinds
void BM_Unwind_MemoryCache_Churn(benchmark::State& state) {
  UnwindBenchmarkFixture fixture(20, /*through_system_lib=*/true);
  MemoryStatus start_mem = ReadMemoryStatus();

  UnwindInputSample sample;
  sample.regs = &fixture.sample().regs;
  sample.stack_data = fixture.sample().stack_bytes.data();
  sample.stack_size = fixture.sample().stack_bytes.size();

  UnwindOptions options;
  options.max_frames = kMaxFrames;

  size_t count = 0;
  for (auto _ : state) {
    UnwindResult result = fixture.unwind_context()->Unwind(sample, options);
    benchmark::DoNotOptimize(result);
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

}  // namespace
}  // namespace profiling
}  // namespace perfetto
