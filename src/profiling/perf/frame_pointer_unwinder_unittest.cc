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

#include "perfetto/base/logging.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

class MockUnwindContext : public UnwindContext {
 public:
  struct MockMapping : public UnwoundMapping {
    MockMapping(std::string name, uint64_t start, uint64_t end)
        : name_(std::move(name)), start_(start), end_(end) {}
    std::string map_name() const override { return name_; }
    uint64_t map_start() const override { return start_; }
    uint64_t map_end() const override { return end_; }
    std::string name_;
    uint64_t start_;
    uint64_t end_;
  };

  void AddMap(uint64_t start, uint64_t end, std::string name) {
    maps_.push_back(std::make_shared<MockMapping>(std::move(name), start, end));
  }

  void ReparseMaps() override {}
  void ResetMaps() override { maps_.clear(); }

  FrameData BuildFrameFromPc(uint64_t pc, bool /*resolve_names*/) override {
    FrameData frame{};
    frame.pc = pc;
    frame.rel_pc = pc;
    for (const auto& m : maps_) {
      if (pc >= m->map_start() && pc < m->map_end()) {
        frame.rel_pc = pc - m->map_start();
        frame.map_info = m;
        return frame;
      }
    }
    return frame;
  }

  UnwindResult Unwind(const UnwindInputSample&, const UnwindOptions&) override {
    return {};
  }

 private:
  std::vector<std::shared_ptr<UnwoundMapping>> maps_;
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
  CpuRegisters regs_x86(CpuArch::X86);
  regs_x86.set_sp(kStackBase);
  FramePointerUnwinder unwinder_x86(kMaxFrames, context_.get(), &regs_x86,
                                    stack_.data(), stack_.size());

  unwinder_x86.Unwind();
  EXPECT_EQ(unwinder_x86.LastErrorCode(), UnwindErrorCode::kUnsupported);

  CpuRegisters regs_arm(CpuArch::ARM);
  regs_arm.set_sp(kStackBase);
  FramePointerUnwinder unwinder_arm(kMaxFrames, context_.get(), &regs_arm,
                                    stack_.data(), stack_.size());
  unwinder_arm.Unwind();
  EXPECT_EQ(unwinder_arm.LastErrorCode(), UnwindErrorCode::kUnsupported);
}

TEST_F(FramePointerUnwinderTest, UnwindInvalidMaps) {
  // Set up a valid stack frame
  CpuRegisters regs(CpuArch::X86_64);
  regs.set_pc(0x1000);
  regs.set_sp(kStackBase);
  regs[X86_64_REG_RBP] = 0x2000;
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
    CpuRegisters regs_x86_64(CpuArch::X86_64);
    regs_x86_64.set_pc(0x1900);
    regs_x86_64.set_sp(0x1800);
    regs_x86_64[X86_64_REG_RBP] = 0x2000;

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
    CpuRegisters regs_arm64(CpuArch::ARM64);
    regs_arm64.set_pc(0x1900);
    regs_arm64.set_sp(0x1800);
    regs_arm64[ARM64_REG_X29] = 0x2000;

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

}  // namespace
}  // namespace profiling
}  // namespace perfetto
