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

}  // namespace
}  // namespace profiling
}  // namespace perfetto
