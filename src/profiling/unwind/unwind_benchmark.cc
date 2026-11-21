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
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/perf/frame_pointer_unwinder.h"
#include "src/profiling/unwind/get_asm_regs.h"
#include "src/profiling/unwind/unwind_engine.h"

#if PERFETTO_BUILDFLAG(PERFETTO_UNWINDER_LIBUNWINDSTACK)
#include <unwindstack/Elf.h>
#include <unwindstack/Unwinder.h>
#endif

namespace perfetto {
namespace profiling {
namespace {

constexpr size_t kMaxFrames = 64;
constexpr size_t kMaxStackDumpBytes = 65536;  // 64kb

bool IsBenchmarkFunctionalOnly() {
  return getenv("BENCHMARK_FUNCTIONAL_TEST_ONLY") != nullptr;
}

static void BmArgs(benchmark::internal::Benchmark* b) {
  if (IsBenchmarkFunctionalOnly()) {
    b->Iterations(1);
  }
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
  pthread_attr_t attr;
  PERFETTO_CHECK(pthread_getattr_np(pthread_self(), &attr) == 0);
  void* stack_base = nullptr;
  size_t stack_size = 0;
  PERFETTO_CHECK(pthread_attr_getstack(&attr, &stack_base, &stack_size) == 0);
  pthread_attr_destroy(&attr);

  uintptr_t stack_low = reinterpret_cast<uintptr_t>(stack_base);
  uintptr_t stack_top = stack_low + stack_size;
  PERFETTO_CHECK(sp >= stack_low && sp < stack_top);
  size_t bytes_to_copy = std::min(stack_top - sp, kMaxStackDumpBytes);

  sample.stack_bytes.resize(bytes_to_copy);
  CopyStackMemory(sample.stack_bytes.data(),
                  reinterpret_cast<const volatile uint8_t*>(sp), bytes_to_copy);

  return sample;
}

class UnwindBenchmarkFixture {
 public:
  explicit UnwindBenchmarkFixture(int callstack_depth) {
    std::function<void()> capture = [this] { sample_ = CaptureCurrentStack(); };
    GenerateCallStack(callstack_depth, capture);

    base::ScopedFile maps_fd(open("/proc/self/maps", O_RDONLY));
    base::ScopedFile mem_fd(open("/proc/self/mem", O_RDONLY));
    PERFETTO_CHECK(maps_fd.get() >= 0);
    PERFETTO_CHECK(mem_fd.get() >= 0);

    unwind_state_ =
        ProcessUnwindState::Create(std::move(maps_fd), std::move(mem_fd));

    // Non-timed warmup so lazy MapInfo/ELF/symbol/build-ID caches are
    // populated before timed iterations begin.
    UnwindInputSample sample;
    sample.regs = &sample_.regs;
    sample.stack_data = sample_.stack_bytes.data();
    sample.stack_size = sample_.stack_bytes.size();

    UnwindOptions options;
    options.max_frames = kMaxFrames;
    options.resolve_names = true;

    UnwindResult result =
        UnwindEngine::Get()->Unwind(unwind_state_.get(), sample, options);
    for (const auto& frame : result.frames) {
      if (frame.map_info != nullptr) {
        frame.map_info->build_id();
      }
    }
  }

  const SampleSnapshot& sample() const { return sample_; }
  ProcessUnwindState* unwind_state() { return unwind_state_.get(); }

 private:
  SampleSnapshot sample_;
  std::unique_ptr<ProcessUnwindState> unwind_state_;
};

void RecordMetrics(benchmark::State& state,
                   int expected_min_depth,
                   size_t total_frames,
                   size_t error_count) {
  if (IsBenchmarkFunctionalOnly()) {
    PERFETTO_CHECK(error_count == 0);
    PERFETTO_CHECK(total_frames >= static_cast<size_t>(expected_min_depth));
  }
  double avg_frames = static_cast<double>(total_frames) /
                      static_cast<double>(state.iterations());
  state.counters["frames_unwound"] = benchmark::Counter(avg_frames);
  state.counters["unwind_errors"] =
      benchmark::Counter(static_cast<double>(error_count));
}

struct UnwindArgs {
  int depth;
  bool resolve_names;
};

// Benchmark DWARF unwinding
void BM_Unwind_Dwarf(benchmark::State& state, const UnwindArgs& args) {
  if (CurrentCpuArch() == CpuArch::UNKNOWN) {
    state.SkipWithError("Unsupported CPU architecture");
    return;
  }
  UnwindBenchmarkFixture fixture(args.depth);

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
    UnwindResult result =
        UnwindEngine::Get()->Unwind(fixture.unwind_state(), sample, options);
    if (result.frames.empty()) {
      error_count++;
    }
    std::vector<std::string> build_ids;
    build_ids.reserve(result.frames.size());
    for (const auto& frame : result.frames) {
      build_ids.emplace_back(
          frame.map_info != nullptr ? frame.map_info->build_id() : "");
    }

    total_frames += result.frames.size();
    benchmark::DoNotOptimize(total_frames);
    benchmark::DoNotOptimize(build_ids);
  }

  RecordMetrics(state, args.depth, total_frames, error_count);
}

// Benchmark Frame Pointer unwinding
void BM_Unwind_FramePointer(benchmark::State& state, int depth) {
  CpuRegisters probe_regs(CurrentCpuArch());
  if (!FramePointerUnwinder(0, nullptr, &probe_regs, nullptr, 0)
           .IsArchSupported()) {
    state.SkipWithError(
        "Frame pointer unwinding is not supported on this architecture");
    return;
  }
  UnwindBenchmarkFixture fixture(depth);

  size_t total_frames = 0;
  size_t error_count = 0;
  for (auto _ : state) {
    FramePointerUnwinder unwinder(kMaxFrames, fixture.unwind_state(),
                                  &fixture.sample().regs,
                                  fixture.sample().stack_bytes.data(),
                                  fixture.sample().stack_bytes.size());
    unwinder.Unwind();

    std::vector<FrameData> frames = unwinder.ConsumeFrames();
    if (frames.empty()) {
      error_count++;
    }
    std::vector<std::string> build_ids;
    build_ids.reserve(frames.size());
    for (const auto& frame : frames) {
      build_ids.emplace_back(
          frame.map_info != nullptr ? frame.map_info->build_id() : "");
    }

    total_frames += frames.size();
    benchmark::DoNotOptimize(total_frames);
    benchmark::DoNotOptimize(build_ids);
  }

  RecordMetrics(state, depth, total_frames, error_count);
}

// Benchmark cold-cache DWARF unwinding (first sample / cache flush)
void BM_Unwind_Dwarf_ColdStart(benchmark::State& state, int depth) {
  if (CurrentCpuArch() == CpuArch::UNKNOWN) {
    state.SkipWithError("Unsupported CPU architecture");
    return;
  }
  UnwindBenchmarkFixture fixture(depth);

  UnwindInputSample sample;
  sample.regs = &fixture.sample().regs;
  sample.stack_data = fixture.sample().stack_bytes.data();
  sample.stack_size = fixture.sample().stack_bytes.size();

  UnwindOptions options;
  options.max_frames = kMaxFrames;
  options.resolve_names = true;

  size_t total_frames = 0;
  size_t error_count = 0;
#if PERFETTO_BUILDFLAG(PERFETTO_UNWINDER_LIBUNWINDSTACK)
  const bool elf_cache_enabled = unwindstack::Elf::CachingEnabled();
#endif
  for (auto _ : state) {
    state.PauseTiming();
    fixture.unwind_state()->ReparseMaps();
#if PERFETTO_BUILDFLAG(PERFETTO_UNWINDER_LIBUNWINDSTACK)
    unwindstack::Elf::SetCachingEnabled(false);
    unwindstack::Elf::SetCachingEnabled(true);
#endif
    state.ResumeTiming();

    UnwindResult result =
        UnwindEngine::Get()->Unwind(fixture.unwind_state(), sample, options);
    if (result.frames.empty()) {
      error_count++;
    }
    total_frames += result.frames.size();
    benchmark::DoNotOptimize(total_frames);
  }

#if PERFETTO_BUILDFLAG(PERFETTO_UNWINDER_LIBUNWINDSTACK)
  unwindstack::Elf::SetCachingEnabled(elf_cache_enabled);
#endif
  RecordMetrics(state, depth, total_frames, error_count);
}

// DWARF benchmarks without symbol resolution
BENCHMARK_CAPTURE(BM_Unwind_Dwarf, Depth10, UnwindArgs{10, false})
    ->Apply(BmArgs);
BENCHMARK_CAPTURE(BM_Unwind_Dwarf, Depth30, UnwindArgs{30, false})
    ->Apply(BmArgs);

// DWARF benchmarks with symbol resolution
BENCHMARK_CAPTURE(BM_Unwind_Dwarf, Depth10_WithNames, UnwindArgs{10, true})
    ->Apply(BmArgs);
BENCHMARK_CAPTURE(BM_Unwind_Dwarf, Depth30_WithNames, UnwindArgs{30, true})
    ->Apply(BmArgs);

// ColdStart
BENCHMARK_CAPTURE(BM_Unwind_Dwarf_ColdStart, Depth20, 20)
    ->Iterations(100)
    ->Apply(BmArgs);

// FramePointer benchmarks
BENCHMARK_CAPTURE(BM_Unwind_FramePointer, Depth10, 10)->Apply(BmArgs);
BENCHMARK_CAPTURE(BM_Unwind_FramePointer, Depth30, 30)->Apply(BmArgs);

}  // namespace
}  // namespace profiling
}  // namespace perfetto
