/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/profiling/memory/unwinding.h"
#include "perfetto/base/scoped_file.h"
#include "src/profiling/memory/transport_data.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <unwindstack/RegsGetLocal.h>

namespace perfetto {
namespace {

TEST(UnwindingTest, StackMemoryOverlay) {
  base::ScopedFile proc_mem(open("/proc/self/mem", O_RDONLY));
  ASSERT_TRUE(proc_mem);
  uint8_t fake_stack[1] = {120};
  StackMemory memory(*proc_mem, 0u, fake_stack, 1);
  uint8_t buf[1] = {};
  ASSERT_EQ(memory.Read(0u, buf, 1), 1);
  ASSERT_EQ(buf[0], 120);
}

TEST(UnwindingTest, StackMemoryNonOverlay) {
  uint8_t value = 52;

  base::ScopedFile proc_mem(open("/proc/self/mem", O_RDONLY));
  ASSERT_TRUE(proc_mem);
  uint8_t fake_stack[1] = {120};
  StackMemory memory(*proc_mem, 0u, fake_stack, 1);
  uint8_t buf[1] = {1};
  ASSERT_EQ(memory.Read(reinterpret_cast<uint64_t>(&value), buf, 1), 1);
  ASSERT_EQ(buf[0], value);
}

TEST(UnwindingTest, FileDescriptorMapsParse) {
  base::ScopedFile proc_maps(open("/proc/self/maps", O_RDONLY));
  ASSERT_TRUE(proc_maps);
  FileDescriptorMaps maps(std::move(proc_maps));
  ASSERT_TRUE(maps.Parse());
  unwindstack::MapInfo* map_info =
      maps.Find(reinterpret_cast<uint64_t>(&proc_maps));
  ASSERT_NE(map_info, nullptr);
  ASSERT_EQ(map_info->name, "[stack]");
}

}  // namespace
}  // namespace perfetto
