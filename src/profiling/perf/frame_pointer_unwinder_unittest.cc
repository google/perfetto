/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/profiling/perf/frame_pointer_unwinder.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/cpu_registers.h"
#include "src/profiling/unwind/perf_regs.h"
#include "src/profiling/unwind/unwind_context.h"
#include "src/profiling/unwind/unwind_types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

class MockUnwindContext : public UnwindContext {
 public:
  struct MockMap {
    uint64_t start;
    uint64_t end;
    std::string name;
  };

  void AddMap(uint64_t start, uint64_t end, std::string name) {
    maps_.push_back({start, end, std::move(name)});
  }

  void ReparseMaps() override {}
  void ResetMaps() override { maps_.clear(); }

  std::optional<FrameData> BuildFrameFromPc(uint64_t pc,
                                            bool /*resolve_names*/) override {
    for (const auto& m : maps_) {
      if (pc >= m.start && pc < m.end) {
        FrameData frame;
        frame.pc = pc;
        frame.rel_pc = pc - m.start;
        MapInfo mi;
        mi.map_name = m.name;
        mi.map_start = m.start;
        mi.map_end = m.end;
        frame.map_info = std::move(mi);
        return frame;
      }
    }
    return std::nullopt;
  }

 private:
  std::vector<MockMap> maps_;
};

constexpr static size_t kMaxFrames = 64;
constexpr static uint64_t kStackBase = 0x1800;
constexpr static size_t kStackSize = 0x2000;

class FramePointerUnwinderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    context_ = std::make_unique<MockUnwindContext>();
    stack_.assign(kStackSize, 0);
  }

  void SetStackData64(uint64_t addr, uint64_t value) {
    PERFETTO_CHECK(addr >= kStackBase &&
                   addr + sizeof(uint64_t) <= kStackBase + stack_.size());
    size_t offset = static_cast<size_t>(addr - kStackBase);
    std::memcpy(stack_.data() + offset, &value, sizeof(uint64_t));
  }

  std::unique_ptr<MockUnwindContext> context_;
  std::vector<uint8_t> stack_;
};

TEST_F(FramePointerUnwinderTest, UnwindUnsupportedArch) {
  CpuRegisters regs_x86;
  regs_x86.arch = CpuArch::kX86;
  regs_x86.sp = kStackBase;
  FramePointerUnwinder unwinder_x86(kMaxFrames, context_.get(), &regs_x86,
                                    stack_.data(), stack_.size());
  unwinder_x86.Unwind();
  EXPECT_EQ(unwinder_x86.LastErrorCode(), UnwindErrorCode::kUnsupported);

  CpuRegisters regs_arm;
  regs_arm.arch = CpuArch::kArm;
  regs_arm.sp = kStackBase;
  FramePointerUnwinder unwinder_arm(kMaxFrames, context_.get(), &regs_arm,
                                    stack_.data(), stack_.size());
  unwinder_arm.Unwind();
  EXPECT_EQ(unwinder_arm.LastErrorCode(), UnwindErrorCode::kUnsupported);
}

TEST_F(FramePointerUnwinderTest, UnwindInvalidMaps) {
  // Set up a valid stack frame
  CpuRegisters regs;
  regs.arch = CpuArch::kX86_64;
  regs.pc = 0x1000;
  regs.sp = kStackBase;
  regs.raw_data[PERF_REG_X86_BP] = 0x2000;
  SetStackData64(0x2000, 0x3000);
  SetStackData64(0x2008, 0x2000);

  FramePointerUnwinder unwinder(kMaxFrames, context_.get(), &regs,
                                stack_.data(), stack_.size());
  unwinder.Unwind();
  EXPECT_EQ(unwinder.LastErrorCode(), UnwindErrorCode::kInvalidMap);
  EXPECT_EQ(unwinder.ConsumeFrames().size(), 0UL);
}

TEST_F(FramePointerUnwinderTest, UnwindValidStack) {
  SetStackData64(0x2000, 0x2200);                     // mock next_fp
  SetStackData64(0x2000 + sizeof(uint64_t), 0x2100);  // mock next_pc
  SetStackData64(0x2200, 0);

  context_->AddMap(0x1000, 0x12000, "libmock.so");

  {
    CpuRegisters regs_x86_64;
    regs_x86_64.arch = CpuArch::kX86_64;
    regs_x86_64.pc = 0x1900;
    regs_x86_64.sp = kStackBase;
    regs_x86_64.raw_data[PERF_REG_X86_BP] = 0x2000;

    FramePointerUnwinder unwinder(kMaxFrames, context_.get(), &regs_x86_64,
                                  stack_.data(), stack_.size());
    unwinder.Unwind();
    EXPECT_EQ(unwinder.LastErrorCode(), UnwindErrorCode::kNone);
    auto frames = unwinder.ConsumeFrames();
    ASSERT_EQ(frames.size(), 2UL);
    EXPECT_EQ(frames[0].pc, 0x1900ULL);
    EXPECT_EQ(frames[1].pc, 0x2100ULL);
  }

  {
    CpuRegisters regs_arm64;
    regs_arm64.arch = CpuArch::kArm64;
    regs_arm64.pc = 0x1900;
    regs_arm64.sp = kStackBase;
    regs_arm64.raw_data[PERF_REG_ARM64_X29] = 0x2000;

    FramePointerUnwinder unwinder(kMaxFrames, context_.get(), &regs_arm64,
                                  stack_.data(), stack_.size());
    unwinder.Unwind();
    EXPECT_EQ(unwinder.LastErrorCode(), UnwindErrorCode::kNone);
    auto frames = unwinder.ConsumeFrames();
    ASSERT_EQ(frames.size(), 2UL);
    EXPECT_EQ(frames[0].pc, 0x1900ULL);
    EXPECT_EQ(frames[1].pc, 0x2100ULL);
  }
}

#if PERFETTO_BUILDFLAG(PERFETTO_ARCH_CPU_X86_64)
extern "C" int DummyTargetFunctionForUnwindTest(int x);
extern "C" __attribute__((noinline, visibility("default"))) int
DummyTargetFunctionForUnwindTest(int x) {
  asm volatile("nop\n\tnop\n\tnop\n\tnop" : "+r"(x));
  return x + 1;
}

TEST_F(FramePointerUnwinderTest, UnwindWithRealLibunwindstackContext) {
  base::ScopedFile maps_fd(open("/proc/self/maps", O_RDONLY | O_CLOEXEC));
  base::ScopedFile mem_fd(open("/proc/self/mem", O_RDONLY | O_CLOEXEC));
  ASSERT_GE(maps_fd.get(), 0);
  ASSERT_GE(mem_fd.get(), 0);

  auto real_ctx = UnwindContext::Create(std::move(maps_fd), std::move(mem_fd));
  ASSERT_NE(real_ctx, nullptr);

  uint64_t target_pc =
      reinterpret_cast<uint64_t>(&DummyTargetFunctionForUnwindTest) + 2;
  SetStackData64(0x2000, 0x2200);
  SetStackData64(0x2000 + sizeof(uint64_t), target_pc);
  SetStackData64(0x2200, 0);

  CpuRegisters regs;
  regs.arch = CpuArch::kX86_64;
  regs.pc = target_pc;
  regs.sp = kStackBase;
  regs.raw_data[PERF_REG_X86_BP] = 0x2000;

  FramePointerUnwinder unwinder(kMaxFrames, real_ctx.get(), &regs,
                                stack_.data(), stack_.size());
  unwinder.SetResolveNames(true);
  unwinder.Unwind();
  EXPECT_EQ(unwinder.LastErrorCode(), UnwindErrorCode::kNone);
  auto frames = unwinder.ConsumeFrames();
  ASSERT_EQ(frames.size(), 2UL);
  EXPECT_EQ(frames[0].function_name, "DummyTargetFunctionForUnwindTest");
  EXPECT_EQ(frames[0].symbol_status, SymbolStatus::kResolved);
  ASSERT_TRUE(frames[0].map_info.has_value());
  EXPECT_FALSE(frames[0].map_info->map_name.empty());
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ARCH_CPU_X86_64)

}  // namespace
}  // namespace profiling
}  // namespace perfetto
