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

#include "src/profiling/unwind/libunwindstack/libunwindstack_context.h"

#include <fcntl.h>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

TEST(LibunwindstackContextTest, StackOverlayMemoryOverlay) {
  base::ScopedFile proc_mem(base::OpenFile("/proc/self/mem", O_RDONLY));
  ASSERT_TRUE(proc_mem);
  uint8_t fake_stack[1] = {120};
  std::shared_ptr<FDMemory> mem(
      std::make_shared<FDMemory>(std::move(proc_mem)));
  StackOverlayMemory memory(mem, 0u, fake_stack, 1);
  uint8_t buf[1] = {};
  ASSERT_EQ(memory.Read(0u, buf, 1), 1u);
  ASSERT_EQ(buf[0], 120);
}

TEST(LibunwindstackContextTest, StackOverlayMemoryNonOverlay) {
  uint8_t value = 52;

  base::ScopedFile proc_mem(base::OpenFile("/proc/self/mem", O_RDONLY));
  ASSERT_TRUE(proc_mem);
  uint8_t fake_stack[1] = {120};
  std::shared_ptr<FDMemory> mem(
      std::make_shared<FDMemory>(std::move(proc_mem)));
  StackOverlayMemory memory(mem, 0u, fake_stack, 1);
  uint8_t buf[1] = {1};
  ASSERT_EQ(memory.Read(reinterpret_cast<uint64_t>(&value), buf, 1), 1u);
  ASSERT_EQ(buf[0], value);
}

TEST(LibunwindstackContextTest, FDMapsParse) {
#if defined(ADDRESS_SANITIZER)
  PERFETTO_LOG("Skipping /proc/self/maps as ASAN distorts what is where");
  GTEST_SKIP();
#else
  base::ScopedFile proc_maps(base::OpenFile("/proc/self/maps", O_RDONLY));
  ASSERT_TRUE(proc_maps);
  FDMaps maps(std::move(proc_maps));
  ASSERT_TRUE(maps.Parse());
  unwindstack::MapInfo* map_info =
      maps.Find(reinterpret_cast<uint64_t>(&proc_maps)).get();
  ASSERT_NE(map_info, nullptr);
  ASSERT_EQ(map_info->name(), "[stack]");
#endif
}

#if PERFETTO_BUILDFLAG(PERFETTO_ARCH_CPU_X86_64)
// Real target function in the test binary for testing ELF symbol resolution:
// - Forward declaration satisfies -Wmissing-prototypes.
// - extern "C" keeps the ELF symbol name unmangled.
// - noinline + visibility("default") prevent stripping/inlining and export the
//   symbol into .dynsym.
// - nop instructions ensure the body is >2 bytes so |target_pc - pc_adjustment|
//   still lands inside this function.
extern "C" int DummyTargetFunctionForUnwindTest(int x);
extern "C" __attribute__((noinline, visibility("default"))) int
DummyTargetFunctionForUnwindTest(int x) {
  asm volatile("nop\n\tnop\n\tnop\n\tnop" : "+r"(x));
  return x + 1;
}

TEST(LibunwindstackContextTest, BuildFrameFromPc) {
  base::ScopedFile maps_fd(base::OpenFile("/proc/self/maps", O_RDONLY));
  base::ScopedFile mem_fd(base::OpenFile("/proc/self/mem", O_RDONLY));
  ASSERT_TRUE(maps_fd);
  ASSERT_TRUE(mem_fd);

  auto ctx = UnwindContext::Create(std::move(maps_fd), std::move(mem_fd));
  ASSERT_NE(ctx, nullptr);

  uint64_t target_pc =
      reinterpret_cast<uint64_t>(&DummyTargetFunctionForUnwindTest) + 2;
  FrameData frame = ctx->BuildFrameFromPc(target_pc, /*resolve_names=*/true);
  EXPECT_EQ(frame.function_name, "DummyTargetFunctionForUnwindTest");
  EXPECT_EQ(frame.symbol_status, SymbolStatus::kResolved);
  ASSERT_NE(frame.map_info, nullptr);
  EXPECT_FALSE(frame.map_info->map_name().empty());
}
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ARCH_CPU_X86_64)

}  // namespace
}  // namespace profiling
}  // namespace perfetto
