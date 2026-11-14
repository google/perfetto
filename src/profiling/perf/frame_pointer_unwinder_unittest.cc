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

#include <cstring>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/unwind_engine.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

// Real functions in the test binary, used as PCs so that the real
// UnwindEngine can resolve them against /proc/self/maps.
__attribute__((noinline)) int LeafFunctionForTest(int x) {
  asm volatile("" : "+r"(x));
  return x + 1;
}

__attribute__((noinline)) int CallerFunctionForTest(int x) {
  asm volatile("" : "+r"(x));
  return x + 2;
}

// Synthetic stack layout. |kStackBase| is the sampled sp; the stack buffer
// covers [kStackBase, kStackBase + kStackSize).
constexpr static size_t kMaxFrames = 64;
constexpr static uint64_t kStackBase = 0x1800;
constexpr static size_t kStackSize = 0x2000;

// An address that is guaranteed not to be mapped (below mmap_min_addr).
constexpr static uint64_t kUnmappedPc = 0x1000;

class FramePointerUnwinderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    base::ScopedFile maps_fd(base::OpenFile("/proc/self/maps", O_RDONLY));
    base::ScopedFile mem_fd(base::OpenFile("/proc/self/mem", O_RDONLY));
    ASSERT_TRUE(maps_fd);
    ASSERT_TRUE(mem_fd);
    state_ = ProcessUnwindState::Create(std::move(maps_fd), std::move(mem_fd));
    stack_.assign(kStackSize, 0);
  }

  void SetStackData64(uint64_t addr, uint64_t value) {
    PERFETTO_CHECK(addr >= kStackBase &&
                   addr + sizeof(uint64_t) <= kStackBase + stack_.size());
    size_t offset = static_cast<size_t>(addr - kStackBase);
    std::memcpy(stack_.data() + offset, &value, sizeof(uint64_t));
  }

  // Lays out a two-frame stack: fp=0x2000 -> {next_fp=0x2200, ret=caller_pc},
  // fp=0x2200 -> {next_fp=0 (terminates)}.
  void SetUpTwoFrameStack(uint64_t caller_pc) {
    SetStackData64(0x2000, 0x2200);                        // next_fp
    SetStackData64(0x2000 + sizeof(uint64_t), caller_pc);  // return address
    SetStackData64(0x2200, 0);                             // end of chain
  }

  static uint64_t PcOf(int (*fn)(int)) {
    // +4 so the PC looks like a return address inside the function body, as
    // the engine subtracts a per-arch adjustment to find the call site.
    return reinterpret_cast<uint64_t>(fn) + 4;
  }

  static void ExpectFrameInFunction(const FrameData& frame, int (*fn)(int)) {
    uint64_t fn_addr = reinterpret_cast<uint64_t>(fn);
    EXPECT_NE(frame.map_info, nullptr);
    EXPECT_GE(frame.pc, fn_addr);
    EXPECT_LT(frame.pc, fn_addr + 4);
  }

  std::unique_ptr<ProcessUnwindState> state_;
  std::vector<uint8_t> stack_;
};

TEST_F(FramePointerUnwinderTest, UnwindUnsupportedArch) {
  CpuRegisters regs_x86(CpuArch::X86);
  regs_x86.set_sp(kStackBase);
  FramePointerUnwinder unwinder_x86(kMaxFrames, state_.get(), &regs_x86,
                                    stack_.data(), stack_.size());
  unwinder_x86.Unwind();
  EXPECT_EQ(unwinder_x86.LastErrorCode(), UnwindErrorCode::kUnsupported);

  CpuRegisters regs_arm(CpuArch::ARM);
  regs_arm.set_sp(kStackBase);
  FramePointerUnwinder unwinder_arm(kMaxFrames, state_.get(), &regs_arm,
                                    stack_.data(), stack_.size());
  unwinder_arm.Unwind();
  EXPECT_EQ(unwinder_arm.LastErrorCode(), UnwindErrorCode::kUnsupported);
}

TEST_F(FramePointerUnwinderTest, UnwindInvalidMaps) {
  // A valid frame chain, but the leaf PC is not in any mapping.
  CpuRegisters regs(CpuArch::X86_64);
  regs.set_pc(kUnmappedPc);
  regs.set_sp(kStackBase);
  regs[X86_64_REG_RBP] = 0x2000;
  SetUpTwoFrameStack(PcOf(&CallerFunctionForTest));

  FramePointerUnwinder unwinder(kMaxFrames, state_.get(), &regs, stack_.data(),
                                stack_.size());
  unwinder.Unwind();
  EXPECT_EQ(unwinder.LastErrorCode(), UnwindErrorCode::kInvalidMap);
  EXPECT_EQ(unwinder.ConsumeFrames().size(), 0UL);
}

TEST_F(FramePointerUnwinderTest, UnwindValidStack) {
  SetUpTwoFrameStack(PcOf(&CallerFunctionForTest));

  {
    CpuRegisters regs_x86_64(CpuArch::X86_64);
    regs_x86_64.set_pc(PcOf(&LeafFunctionForTest));
    regs_x86_64.set_sp(kStackBase);
    regs_x86_64[X86_64_REG_RBP] = 0x2000;

    FramePointerUnwinder unwinder(kMaxFrames, state_.get(), &regs_x86_64,
                                  stack_.data(), stack_.size());
    unwinder.Unwind();
    EXPECT_EQ(unwinder.LastErrorCode(), UnwindErrorCode::kNone);
    auto frames = unwinder.ConsumeFrames();
    ASSERT_EQ(frames.size(), 2UL);
    ExpectFrameInFunction(frames[0], &LeafFunctionForTest);
    ExpectFrameInFunction(frames[1], &CallerFunctionForTest);
  }

  {
    CpuRegisters regs_arm64(CpuArch::ARM64);
    regs_arm64.set_pc(PcOf(&LeafFunctionForTest));
    regs_arm64.set_sp(kStackBase);
    regs_arm64[ARM64_REG_X29] = 0x2000;

    FramePointerUnwinder unwinder(kMaxFrames, state_.get(), &regs_arm64,
                                  stack_.data(), stack_.size());
    unwinder.Unwind();
    EXPECT_EQ(unwinder.LastErrorCode(), UnwindErrorCode::kNone);
    auto frames = unwinder.ConsumeFrames();
    ASSERT_EQ(frames.size(), 2UL);
    ExpectFrameInFunction(frames[0], &LeafFunctionForTest);
    ExpectFrameInFunction(frames[1], &CallerFunctionForTest);
  }
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
