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
#include <unwindstack/Unwinder.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

class RegsFake : public unwindstack::Regs {
 public:
  RegsFake(uint16_t total_regs)
      : unwindstack::Regs(
            total_regs,
            unwindstack::Regs::Location(unwindstack::Regs::LOCATION_UNKNOWN,
                                        0)) {
    fake_data_ = std::make_unique<uint64_t[]>(total_regs);
  }
  ~RegsFake() override = default;

  unwindstack::ArchEnum Arch() override { return fake_arch_; }
  void* RawData() override { return fake_data_.get(); }
  uint64_t pc() override { return fake_pc_; }
  uint64_t sp() override { return fake_sp_; }
  void set_pc(uint64_t pc) override { fake_pc_ = pc; }
  void set_sp(uint64_t sp) override { fake_sp_ = sp; }

  void set_fp(uint64_t fp) {
    switch (fake_arch_) {
      case unwindstack::ARCH_ARM64:
        fake_data_[unwindstack::Arm64Reg::ARM64_REG_R29] = fp;
        break;
      case unwindstack::ARCH_X86_64:
        fake_data_[unwindstack::X86_64Reg::X86_64_REG_RBP] = fp;
        break;
      case unwindstack::ARCH_RISCV64:
        fake_data_[unwindstack::Riscv64Reg::RISCV64_REG_S0] = fp;
        break;
      case unwindstack::ARCH_UNKNOWN:
      case unwindstack::ARCH_ARM:
      case unwindstack::ARCH_X86:
          // not supported
          ;
    }
  }

  bool SetPcFromReturnAddress(unwindstack::Memory*) override { return false; }

  void IterateRegisters(std::function<void(const char*, uint64_t)>) override {}

  bool StepIfSignalHandler(uint64_t,
                           unwindstack::Elf*,
                           unwindstack::Memory*) override {
    return false;
  }

  void FakeSetArch(unwindstack::ArchEnum arch) { fake_arch_ = arch; }

  Regs* Clone() override { return nullptr; }

 private:
  unwindstack::ArchEnum fake_arch_ = unwindstack::ARCH_UNKNOWN;
  uint64_t fake_pc_ = 0;
  uint64_t fake_sp_ = 0;
  std::unique_ptr<uint64_t[]> fake_data_;
};

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
    regs_fake_ = std::make_unique<RegsFake>(64);
    regs_fake_->FakeSetArch(unwindstack::ARCH_X86_64);
    process_memory_.reset(memory_fake_);

    unwinder_ = std::make_unique<FramePointerUnwinder>(
        kMaxFrames, maps_.get(), regs_fake_.get(), process_memory_, kStackSize);
  }

  MemoryFake* memory_fake_;
  std::unique_ptr<unwindstack::Maps> maps_;
  std::unique_ptr<RegsFake> regs_fake_;
  std::shared_ptr<unwindstack::Memory> process_memory_;

  std::unique_ptr<FramePointerUnwinder> unwinder_;
};

TEST_F(FramePointerUnwinderTest, UnwindUnsupportedArch) {
  regs_fake_->FakeSetArch(unwindstack::ARCH_UNKNOWN);
  unwinder_.reset(new FramePointerUnwinder(
      kMaxFrames, maps_.get(), regs_fake_.get(), process_memory_, kStackSize));
  unwinder_->Unwind();
  EXPECT_EQ(unwinder_->LastErrorCode(),
            unwindstack::ErrorCode::ERROR_UNSUPPORTED);

  regs_fake_->FakeSetArch(unwindstack::ARCH_X86);
  unwinder_.reset(new FramePointerUnwinder(
      kMaxFrames, maps_.get(), regs_fake_.get(), process_memory_, kStackSize));
  unwinder_->Unwind();
  EXPECT_EQ(unwinder_->LastErrorCode(),
            unwindstack::ErrorCode::ERROR_UNSUPPORTED);

  regs_fake_->FakeSetArch(unwindstack::ARCH_ARM);
  unwinder_.reset(new FramePointerUnwinder(
      kMaxFrames, maps_.get(), regs_fake_.get(), process_memory_, kStackSize));
  unwinder_->Unwind();
  EXPECT_EQ(unwinder_->LastErrorCode(),
            unwindstack::ErrorCode::ERROR_UNSUPPORTED);
}

TEST_F(FramePointerUnwinderTest, UnwindInvalidMaps) {
  // Set up a valid stack frame
  regs_fake_->set_pc(0x1000);
  regs_fake_->set_sp(0x2000);
  memory_fake_->SetData64(0x2000, 0x3000);
  memory_fake_->SetData64(0x2008, 0x2000);
  unwinder_->Unwind();
  EXPECT_EQ(unwinder_->LastErrorCode(),
            unwindstack::ErrorCode::ERROR_INVALID_MAP);
  EXPECT_EQ(unwinder_->ConsumeFrames().size(), 0UL);
}

TEST_F(FramePointerUnwinderTest, UnwindValidStack) {
  regs_fake_->set_pc(0x1900);
  regs_fake_->set_sp(0x1800);
  regs_fake_->set_fp(0x2000);

  memory_fake_->SetData64(0x2000, 0x2200);  // mock next_fp
  memory_fake_->SetData64(0x2000 + sizeof(uint64_t),
                          0x2100);  // mock return_address(next_pc)

  memory_fake_->SetData64(0x2200, 0);

  maps_->Add(0x1000, 0x12000, 0, PROT_READ | PROT_WRITE, "libmock.so");

  unwinder_.reset(new FramePointerUnwinder(
      kMaxFrames, maps_.get(), regs_fake_.get(), process_memory_, kStackSize));
  unwinder_->Unwind();
  EXPECT_EQ(unwinder_->LastErrorCode(), unwindstack::ErrorCode::ERROR_NONE);
  EXPECT_EQ(unwinder_->ConsumeFrames().size(), 2UL);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
