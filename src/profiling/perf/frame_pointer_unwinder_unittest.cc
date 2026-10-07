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

#include <sys/mman.h>
#include <unwindstack/MachineArm64.h>
#include <unwindstack/MachineX86_64.h>
#include <unwindstack/RegsArm.h>
#include <unwindstack/RegsArm64.h>
#include <unwindstack/RegsX86.h>
#include <unwindstack/RegsX86_64.h>
#include <unwindstack/Unwinder.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

class MemoryFake : public unwindstack::Memory {
 public:
  MemoryFake() = default;
  ~MemoryFake() override = default;

  size_t Read(uint64_t addr, void* memory, size_t size) override {
    uint8_t* dst = reinterpret_cast<uint8_t*>(memory);
    for (size_t i = 0; i < size; i++, addr++) {
      auto value = data_.find(addr);
      if (value == data_.end()) {
        return i;
      }
      dst[i] = value->second;
    }
    return size;
  }

  void SetMemory(uint64_t addr, const void* memory, size_t length) {
    const uint8_t* src = reinterpret_cast<const uint8_t*>(memory);
    for (size_t i = 0; i < length; i++, addr++) {
      auto value = data_.find(addr);
      if (value != data_.end()) {
        value->second = src[i];
      } else {
        data_.insert({addr, src[i]});
      }
    }
  }

  void SetData8(uint64_t addr, uint8_t value) {
    SetMemory(addr, &value, sizeof(value));
  }

  void SetData16(uint64_t addr, uint16_t value) {
    SetMemory(addr, &value, sizeof(value));
  }

  void SetData32(uint64_t addr, uint32_t value) {
    SetMemory(addr, &value, sizeof(value));
  }

  void SetData64(uint64_t addr, uint64_t value) {
    SetMemory(addr, &value, sizeof(value));
  }

  void SetMemory(uint64_t addr, std::vector<uint8_t> values) {
    SetMemory(addr, values.data(), values.size());
  }

  void SetMemory(uint64_t addr, std::string string) {
    SetMemory(addr, string.c_str(), string.size() + 1);
  }

  void Clear() override { data_.clear(); }

 private:
  std::unordered_map<uint64_t, uint8_t> data_;
};

constexpr static uint64_t kMaxFrames = 64;
constexpr static uint64_t kStackSize = 0xFFFFFFF;

class FramePointerUnwinderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    memory_fake_ = new MemoryFake;
    maps_.reset(new unwindstack::Maps);
    process_memory_.reset(memory_fake_);
  }

  MemoryFake* memory_fake_;
  std::unique_ptr<unwindstack::Maps> maps_;
  std::shared_ptr<unwindstack::Memory> process_memory_;
};

TEST_F(FramePointerUnwinderTest, UnwindUnsupportedArch) {
  unwindstack::RegsX86 regs_x86;
  FramePointerUnwinder unwinder_x86(kMaxFrames, maps_.get(), &regs_x86,
                                    process_memory_, kStackSize);
  unwinder_x86.Unwind();
  EXPECT_EQ(unwinder_x86.LastErrorCode(),
            unwindstack::ErrorCode::ERROR_UNSUPPORTED);

  unwindstack::RegsArm regs_arm;
  FramePointerUnwinder unwinder_arm(kMaxFrames, maps_.get(), &regs_arm,
                                    process_memory_, kStackSize);
  unwinder_arm.Unwind();
  EXPECT_EQ(unwinder_arm.LastErrorCode(),
            unwindstack::ErrorCode::ERROR_UNSUPPORTED);
}

TEST_F(FramePointerUnwinderTest, UnwindInvalidMaps) {
  // Set up a valid stack frame
  unwindstack::RegsX86_64 regs;
  regs.set_pc(0x1000);
  regs.set_sp(0x2000);
  memory_fake_->SetData64(0x2000, 0x3000);
  memory_fake_->SetData64(0x2008, 0x2000);

  FramePointerUnwinder unwinder(kMaxFrames, maps_.get(), &regs, process_memory_,
                                kStackSize);
  unwinder.Unwind();
  EXPECT_EQ(unwinder.LastErrorCode(),
            unwindstack::ErrorCode::ERROR_INVALID_MAP);
  EXPECT_EQ(unwinder.ConsumeFrames().size(), 0UL);
}

TEST_F(FramePointerUnwinderTest, UnwindValidStack) {
  memory_fake_->SetData64(0x2000, 0x2200);  // mock next_fp
  memory_fake_->SetData64(0x2000 + sizeof(uint64_t),
                          0x2100);  // mock return_address(next_pc)
  memory_fake_->SetData64(0x2200, 0);

  maps_->Add(0x1000, 0x12000, 0, PROT_READ | PROT_WRITE, "libmock.so");

  {
    unwindstack::RegsX86_64 regs_x86_64;
    regs_x86_64.set_pc(0x1900);
    regs_x86_64.set_sp(0x1800);
    regs_x86_64[unwindstack::X86_64_REG_RBP] = 0x2000;

    FramePointerUnwinder unwinder(kMaxFrames, maps_.get(), &regs_x86_64,
                                  process_memory_, kStackSize);
    unwinder.Unwind();
    EXPECT_EQ(unwinder.LastErrorCode(), unwindstack::ErrorCode::ERROR_NONE);
    EXPECT_EQ(unwinder.ConsumeFrames().size(), 2UL);
  }

  {
    unwindstack::RegsArm64 regs_arm64;
    regs_arm64.set_pc(0x1900);
    regs_arm64.set_sp(0x1800);
    regs_arm64[unwindstack::ARM64_REG_R29] = 0x2000;

    FramePointerUnwinder unwinder(kMaxFrames, maps_.get(), &regs_arm64,
                                  process_memory_, kStackSize);
    unwinder.Unwind();
    EXPECT_EQ(unwinder.LastErrorCode(), unwindstack::ErrorCode::ERROR_NONE);
    EXPECT_EQ(unwinder.ConsumeFrames().size(), 2UL);
  }
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
