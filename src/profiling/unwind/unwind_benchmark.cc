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

#include <unwindstack/Elf.h>
#include <unwindstack/Unwinder.h>

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

    metadata_ = std::make_unique<UnwindingMetadata>(std::move(maps_fd),
                                                    std::move(mem_fd));
    overlay_memory_ = std::make_shared<StackOverlayMemory>(
        metadata_->fd_mem, sample_.regs.sp(), sample_.stack_bytes.data(),
        sample_.stack_bytes.size());

    // Non-timed warmup so lazy MapInfo/ELF/symbol/build-ID caches are
    // populated before timed iterations begin.
    for (const auto& frame : Unwind(/*resolve_names=*/true)) {
      metadata_->GetBuildId(frame);
    }
  }

  std::vector<unwindstack::FrameData> Unwind(bool resolve_names) {
    std::unique_ptr<unwindstack::Regs> regs =
        ToLibUnwindstackRegs(sample_.regs);
    PERFETTO_DCHECK(regs != nullptr);
    unwindstack::Unwinder unwinder(kMaxFrames, &metadata_->fd_maps, regs.get(),
                                   overlay_memory_);
    unwinder.SetResolveNames(resolve_names);
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

  RecordMetrics(state, depth, total_frames, error_count);
}

// Benchmark cold-cache DWARF unwinding (first sample / cache flush)
void BM_Unwind_Dwarf_ColdStart(benchmark::State& state, int depth) {
  if (CurrentCpuArch() == CpuArch::UNKNOWN) {
    state.SkipWithError("Unsupported CPU architecture");
    return;
  }
  UnwindBenchmarkFixture fixture(depth);

  size_t total_frames = 0;
  size_t error_count = 0;
  const bool elf_cache_enabled = unwindstack::Elf::CachingEnabled();
  for (auto _ : state) {
    state.PauseTiming();
    fixture.metadata()->ReparseMaps();
    unwindstack::Elf::SetCachingEnabled(false);
    unwindstack::Elf::SetCachingEnabled(true);
    state.ResumeTiming();

    std::vector<unwindstack::FrameData> frames =
        fixture.Unwind(/*resolve_names=*/true);
    if (frames.empty()) {
      error_count++;
    }
    total_frames += frames.size();
    benchmark::DoNotOptimize(total_frames);
  }

  unwindstack::Elf::SetCachingEnabled(elf_cache_enabled);
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
