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

#include "src/profiling/unwind/libunwind/libunwind_context.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/asm_get_regs.h"
#include "src/profiling/unwind/cpu_registers.h"
#include "src/profiling/unwind/libunwind/process_map.h"
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
  char reg_data[64 * sizeof(uint64_t)] = {};
  AsmGetRegs(reg_data);
  out->regs = CpuRegisters::FromUserRegs(CurrentCpuArch(), reg_data);

  const auto* sp_ptr =
      reinterpret_cast<const uint8_t*>(__builtin_frame_address(0));
  out->regs.sp = reinterpret_cast<uint64_t>(sp_ptr);

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

TEST(ProcessMapTest, ReadMapFileContent) {
  std::string maps_content =
      "00400000-00409000 r-xp 00001000 fc:00 426998  /usr/bin/app\n"
      "7fff0000-7fff2000 rw-p 00000000 00:00 0       [stack]\n"
      "7fff2000-7fff3000 rw-s 00002000 00:01 1234    /dev/shm/test\n";

  std::vector<procinfo::MapInfo> parsed;
  ASSERT_TRUE(procinfo::ReadMapFileContent(
      maps_content.data(),
      [&](const procinfo::MapInfo& info) { parsed.push_back(info); }));

  ASSERT_EQ(parsed.size(), 3u);
  EXPECT_EQ(parsed[0].start, 0x00400000u);
  EXPECT_EQ(parsed[0].end, 0x00409000u);
  EXPECT_EQ(parsed[0].flags, PROT_READ | PROT_EXEC);
  EXPECT_EQ(parsed[0].pgoff, 0x1000u);
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
}

TEST(LibunwindContextTest, BuildFrameFromPc) {
  base::ScopedFile proc_maps(base::OpenFile("/proc/self/maps", O_RDONLY));
  base::ScopedFile proc_mem(base::OpenFile("/proc/self/mem", O_RDONLY));
  ASSERT_TRUE(proc_maps);
  ASSERT_TRUE(proc_mem);

  LibunwindContext ctx(std::move(proc_maps), std::move(proc_mem));
  uint64_t pc = GetCurrentPcForTest();

  // With resolve_names = true.
  FrameData frame = ctx.BuildFrameFromPc(pc, true);
  EXPECT_EQ(frame.pc, pc);
  ASSERT_TRUE(frame.map_info.has_value());
  EXPECT_FALSE(frame.map_info->map_name.empty());
  EXPECT_EQ(frame.symbol_status, SymbolStatus::kResolved);
  EXPECT_FALSE(frame.function_name.empty());

  // With resolve_names = false.
  FrameData frame_no_sym = ctx.BuildFrameFromPc(pc, false);
  EXPECT_EQ(frame_no_sym.pc, pc);
  EXPECT_TRUE(frame_no_sym.map_info.has_value());
  EXPECT_EQ(frame_no_sym.symbol_status, SymbolStatus::kNotAttempted);
  EXPECT_TRUE(frame_no_sym.function_name.empty());

  // Unmapped PC returns a fallback frame with map_info == std::nullopt.
  FrameData unmapped = ctx.BuildFrameFromPc(0u, true);
  EXPECT_EQ(unmapped.pc, 0u);
  EXPECT_EQ(unmapped.rel_pc, 0u);
  EXPECT_FALSE(unmapped.map_info.has_value());
}

TEST(LibunwindContextTest, UnwindMultiFrameAndOptions) {
  base::ScopedFile proc_maps(base::OpenFile("/proc/self/maps", O_RDONLY));
  base::ScopedFile proc_mem(base::OpenFile("/proc/self/mem", O_RDONLY));
  ASSERT_TRUE(proc_maps);
  ASSERT_TRUE(proc_mem);

  LibunwindContext ctx(std::move(proc_maps), std::move(proc_mem));

  CapturedStackSample captured;
  CaptureUnwindSampleCaller(&captured);

  UnwindInputSample sample;
  sample.regs = &captured.regs;
  sample.stack_data = captured.stack.data();
  sample.stack_size = captured.stack.size();

  UnwindOptions options;
  options.max_frames = 64;
  options.resolve_names = true;

  UnwindResult res = ctx.Unwind(sample, options);
  EXPECT_EQ(res.error_code, UnwindErrorCode::kNone);
  ASSERT_GE(res.frames.size(), 2u);
  EXPECT_THAT(res.frames[0].function_name,
              ::testing::HasSubstr("CaptureUnwindSampleLeaf"));
  EXPECT_THAT(res.frames[1].function_name,
              ::testing::HasSubstr("CaptureUnwindSampleCaller"));

  // Verify max_frames limit sets kMaxFramesExceeded.
  options.max_frames = 2;
  UnwindResult capped_res = ctx.Unwind(sample, options);
  EXPECT_EQ(capped_res.frames.size(), 2u);
  EXPECT_EQ(capped_res.error_code, UnwindErrorCode::kMaxFramesExceeded);

  // Verify skip_initial_maps skips initial frames matching the map basename.
  ASSERT_TRUE(res.frames[0].map_info.has_value());
  options.max_frames = 64;
  options.skip_initial_maps = {
      base::Basename(res.frames[0].map_info->map_name)};
  UnwindResult skipped_res = ctx.Unwind(sample, options);
  EXPECT_LT(skipped_res.frames.size(), res.frames.size());

  // Verify ResetMaps() clears maps and Unwind(max_attempts = 2) reparses them.
  options.skip_initial_maps.clear();
  options.max_attempts = 2;
  ctx.ResetMaps();
  EXPECT_EQ(ctx.reparses(), 0u);
  UnwindResult reparsed_res = ctx.Unwind(sample, options);
  EXPECT_EQ(reparsed_res.error_code, UnwindErrorCode::kNone);
  EXPECT_EQ(ctx.reparses(), 1u);
}

TEST(LibunwindContextTest, UnwindInvalidPcReturnsInvalidMap) {
  base::ScopedFile proc_maps(base::OpenFile("/proc/self/maps", O_RDONLY));
  base::ScopedFile proc_mem(base::OpenFile("/proc/self/mem", O_RDONLY));
  ASSERT_TRUE(proc_maps);
  ASSERT_TRUE(proc_mem);

  LibunwindContext ctx(std::move(proc_maps), std::move(proc_mem));

  CpuRegisters bad_regs;
  bad_regs.arch = CurrentCpuArch();
  bad_regs.pc = 0x100u;  // Unmapped address.
  bad_regs.sp = 0x100u;

  uint8_t dummy_stack[64] = {};
  UnwindInputSample sample;
  sample.regs = &bad_regs;
  sample.stack_data = dummy_stack;
  sample.stack_size = sizeof(dummy_stack);

  UnwindOptions options;
  options.max_frames = 16;
  options.max_attempts = 1;

  UnwindResult res = ctx.Unwind(sample, options);
  EXPECT_EQ(res.error_code, UnwindErrorCode::kInvalidMap);
  ASSERT_EQ(res.frames.size(), 1u);
  EXPECT_EQ(res.frames[0].pc, 0x100u);
  EXPECT_FALSE(res.frames[0].map_info.has_value());
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
