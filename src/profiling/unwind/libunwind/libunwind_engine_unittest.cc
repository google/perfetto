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

#include "src/profiling/unwind/libunwind/libunwind_engine.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/cpu_registers.h"
#include "src/profiling/unwind/get_asm_regs.h"
#include "src/profiling/unwind/libunwind/process_map.h"
#include "src/profiling/unwind/unwind_engine.h"
#include "src/profiling/unwind/unwind_types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

__attribute__((noinline)) uint64_t GetCurrentPcForTest() {
  return reinterpret_cast<uint64_t>(__builtin_return_address(0));
}

struct CapturedStackSample {
  CpuRegisters regs;
  std::vector<uint8_t> stack;
};

__attribute__((noinline, no_sanitize("address", "hwaddress", "memory"))) void
CopyStackBytesForTest(uint8_t* dst, const uint8_t* src, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    dst[i] = src[i];
  }
}

__attribute__((noinline)) void CaptureUnwindSampleLeaf(
    CapturedStackSample* out,
    const uint8_t* outer_frame_addr) {
  alignas(8) uintptr_t reg_data[CpuRegisters::kMaxRegs] = {};
  GetAsmRegs(reg_data);
  out->regs = CpuRegisters(CurrentCpuArch());
  for (size_t i = 0; i < CpuRegisters::kMaxRegs; ++i) {
    out->regs[i] = reg_data[i];
  }

  const auto* sp_ptr =
      reinterpret_cast<const uint8_t*>(__builtin_frame_address(0));
  out->regs.set_sp(reinterpret_cast<uint64_t>(sp_ptr));

  const size_t copy_bytes =
      outer_frame_addr > sp_ptr
          ? static_cast<size_t>(outer_frame_addr - sp_ptr) + 256
          : 4096;
  out->stack.resize(copy_bytes);
  CopyStackBytesForTest(out->stack.data(), sp_ptr, copy_bytes);
}

__attribute__((noinline)) void CaptureUnwindSampleCaller(
    CapturedStackSample* out) {
  const auto* outer_frame_addr =
      reinterpret_cast<const uint8_t*>(__builtin_frame_address(0));
  CaptureUnwindSampleLeaf(out, outer_frame_addr);
  asm volatile("" : : "r"(out) : "memory");
}

std::unique_ptr<ProcessUnwindState> CreateSelfState() {
  base::ScopedFile proc_maps(base::OpenFile("/proc/self/maps", O_RDONLY));
  base::ScopedFile proc_mem(base::OpenFile("/proc/self/mem", O_RDONLY));
  if (!proc_maps || !proc_mem)
    return nullptr;
  return ProcessUnwindState::Create(std::move(proc_maps), std::move(proc_mem));
}

TEST(ProcMapsTest, ParseProcMaps) {
  std::string maps_content =
      "00400000-00409000 r-xp 00001000 fc:00 426998  /usr/bin/app\n"
      "7fff0000-7fff2000 rw-p 00000000 00:00 0       [stack]\n"
      "7fff2000-7fff3000 rw-s 00002000 00:01 1234    /dev/shm/test\n"
      "7fff3000-7fff4000 ---p 00000000 00:00 0\n";

  std::vector<ProcMapsEntry> parsed;
  ASSERT_TRUE(ParseProcMaps(maps_content, [&](const ProcMapsEntry& entry) {
    parsed.push_back(entry);
  }));

  ASSERT_EQ(parsed.size(), 4u);
  EXPECT_EQ(parsed[0].start, 0x00400000u);
  EXPECT_EQ(parsed[0].end, 0x00409000u);
  EXPECT_EQ(parsed[0].flags, PROT_READ | PROT_EXEC);
  EXPECT_EQ(parsed[0].pgoff, 0x1000u);
  EXPECT_EQ(parsed[0].inode, 426998u);
  EXPECT_EQ(parsed[0].name, "/usr/bin/app");
  EXPECT_FALSE(parsed[0].shared);

  EXPECT_EQ(parsed[1].start, 0x7fff0000u);
  EXPECT_EQ(parsed[1].end, 0x7fff2000u);
  EXPECT_EQ(parsed[1].flags, PROT_READ | PROT_WRITE);
  EXPECT_EQ(parsed[1].name, "[stack]");
  EXPECT_FALSE(parsed[1].shared);

  EXPECT_EQ(parsed[2].flags, PROT_READ | PROT_WRITE);
  EXPECT_EQ(parsed[2].pgoff, 0x2000u);
  EXPECT_EQ(parsed[2].name, "/dev/shm/test");
  EXPECT_TRUE(parsed[2].shared);

  // Anonymous mapping without a name column.
  EXPECT_EQ(parsed[3].flags, 0u);
  EXPECT_TRUE(parsed[3].name.empty());
}

TEST(ProcMapsTest, ParseProcMapsLineRejectsMalformed) {
  ProcMapsEntry entry;
  EXPECT_FALSE(ParseProcMapsLine("", &entry));
  EXPECT_FALSE(ParseProcMapsLine("00400000 r-xp 00001000 fc:00 1 x", &entry));
  EXPECT_FALSE(ParseProcMapsLine("00400000-00409000 rwxq 0 fc:00 1", &entry));
  EXPECT_FALSE(ParseProcMapsLine("00400000-00409000 r-xp 0 fc00 1", &entry));
  EXPECT_FALSE(ParseProcMapsLine("00400000-00409000 r-xp 0 fc:00", &entry));

  // A bad line aborts the whole parse.
  std::string content =
      "00400000-00409000 r-xp 00001000 fc:00 426998  /usr/bin/app\n"
      "garbage\n";
  size_t calls = 0;
  EXPECT_FALSE(ParseProcMaps(content, [&](const ProcMapsEntry&) { calls++; }));
  EXPECT_EQ(calls, 1u);
}

TEST(LibunwindEngineTest, BuildFrameFromPc) {
  auto state = CreateSelfState();
  ASSERT_NE(state, nullptr);
  UnwindEngine* engine = UnwindEngine::Get();
  uint64_t pc = GetCurrentPcForTest();

  // With resolve_names = true.
  FrameData frame = engine->BuildFrameFromPc(state.get(), pc, true);
  EXPECT_EQ(frame.pc, pc);
  ASSERT_NE(frame.map_info, nullptr);
  EXPECT_FALSE(frame.map_info->map_name().empty());
  EXPECT_EQ(frame.symbol_status, SymbolStatus::kResolved);
  EXPECT_FALSE(frame.function_name.empty());

  // With resolve_names = false.
  FrameData frame_no_sym = engine->BuildFrameFromPc(state.get(), pc, false);
  EXPECT_EQ(frame_no_sym.pc, pc);
  ASSERT_NE(frame_no_sym.map_info, nullptr);
  EXPECT_EQ(frame_no_sym.symbol_status, SymbolStatus::kNotAttempted);
  EXPECT_TRUE(frame_no_sym.function_name.empty());

  // Unmapped PC returns a fallback frame with map_info == nullptr.
  FrameData unmapped = engine->BuildFrameFromPc(state.get(), 0u, true);
  EXPECT_EQ(unmapped.pc, 0u);
  EXPECT_EQ(unmapped.rel_pc, 0u);
  EXPECT_EQ(unmapped.map_info, nullptr);
}

TEST(LibunwindEngineTest, UnwindMultiFrameAndOptions) {
  auto state = CreateSelfState();
  ASSERT_NE(state, nullptr);
  UnwindEngine* engine = UnwindEngine::Get();

  CapturedStackSample captured;
  CaptureUnwindSampleCaller(&captured);

  UnwindInputSample sample;
  sample.regs = &captured.regs;
  sample.stack_data = captured.stack.data();
  sample.stack_size = captured.stack.size();

  UnwindOptions options;
  options.max_frames = 64;
  options.resolve_names = true;

  UnwindResult res = engine->Unwind(state.get(), sample, options);
  EXPECT_EQ(res.error_code, UnwindErrorCode::kNone);
  ASSERT_GE(res.frames.size(), 2u);
  EXPECT_THAT(res.frames[0].function_name,
              ::testing::HasSubstr("CaptureUnwindSampleLeaf"));
  EXPECT_THAT(res.frames[1].function_name,
              ::testing::HasSubstr("CaptureUnwindSampleCaller"));

  // Verify max_frames limit sets kMaxFramesExceeded.
  options.max_frames = 2;
  UnwindResult capped_res = engine->Unwind(state.get(), sample, options);
  EXPECT_EQ(capped_res.frames.size(), 2u);
  EXPECT_EQ(capped_res.error_code, UnwindErrorCode::kMaxFramesExceeded);

  // Verify skip_initial_maps skips initial frames matching the map basename.
  ASSERT_NE(res.frames[0].map_info, nullptr);
  options.max_frames = 64;
  std::vector<std::string> skip_maps = {
      base::Basename(res.frames[0].map_info->map_name())};
  options.skip_initial_maps = &skip_maps;
  UnwindResult skipped_res = engine->Unwind(state.get(), sample, options);
  EXPECT_LT(skipped_res.frames.size(), res.frames.size());
  options.skip_initial_maps = nullptr;

  // Verify ResetMaps() drops the maps (unwinding fails with kInvalidMap) and
  // that frames handed out earlier remain valid, then ReparseMaps() restores
  // unwinding.
  state->ResetMaps();
  EXPECT_FALSE(res.frames[0].map_info->map_name().empty());
  UnwindResult reset_res = engine->Unwind(state.get(), sample, options);
  EXPECT_EQ(reset_res.error_code, UnwindErrorCode::kInvalidMap);

  state->ReparseMaps();
  UnwindResult reparsed_res = engine->Unwind(state.get(), sample, options);
  EXPECT_EQ(reparsed_res.error_code, UnwindErrorCode::kNone);
  ASSERT_GE(reparsed_res.frames.size(), 2u);
  EXPECT_THAT(reparsed_res.frames[0].function_name,
              ::testing::HasSubstr("CaptureUnwindSampleLeaf"));
}

TEST(LibunwindEngineTest, UnwindInvalidPcReturnsInvalidMap) {
  auto state = CreateSelfState();
  ASSERT_NE(state, nullptr);

  CpuRegisters bad_regs(CurrentCpuArch());
  bad_regs.set_pc(0x100u);  // Unmapped address.
  bad_regs.set_sp(0x100u);

  uint8_t dummy_stack[64] = {};
  UnwindInputSample sample;
  sample.regs = &bad_regs;
  sample.stack_data = dummy_stack;
  sample.stack_size = sizeof(dummy_stack);

  UnwindOptions options;
  options.max_frames = 16;

  UnwindResult res = UnwindEngine::Get()->Unwind(state.get(), sample, options);
  EXPECT_EQ(res.error_code, UnwindErrorCode::kInvalidMap);
  ASSERT_EQ(res.frames.size(), 1u);
  EXPECT_EQ(res.frames[0].pc, 0x100u);
  EXPECT_EQ(res.frames[0].map_info, nullptr);
}

TEST(LibunwindEngineTest, UnwindWrongArchReturnsBadArch) {
  auto state = CreateSelfState();
  ASSERT_NE(state, nullptr);

  CpuRegisters regs(CpuArch::ARM64);
  UnwindInputSample sample;
  sample.regs = &regs;

  UnwindResult res = UnwindEngine::Get()->Unwind(state.get(), sample);
  EXPECT_EQ(res.error_code, UnwindErrorCode::kBadArch);
  EXPECT_TRUE(res.frames.empty());
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
