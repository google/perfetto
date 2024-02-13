/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/kernel_utils/syscall_table.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

TEST(SyscallTableTest, Arm64) {
  SyscallTable t(Architecture::kArm64);
  EXPECT_STREQ(t.GetById(0), "sys_io_setup");
  EXPECT_EQ(t.GetByName("sys_io_setup"), 0u);

  EXPECT_STREQ(t.GetById(1), "sys_io_destroy");
  EXPECT_EQ(t.GetByName("sys_io_destroy"), 1u);

  EXPECT_STREQ(t.GetById(220), "sys_clone");
  EXPECT_EQ(t.GetByName("sys_clone"), 220u);

  EXPECT_STREQ(t.GetById(293), "sys_rseq");
  EXPECT_EQ(t.GetByName("sys_rseq"), 293u);

  EXPECT_STREQ(t.GetById(457), nullptr);
  EXPECT_STREQ(t.GetById(kMaxSyscalls), nullptr);

  EXPECT_EQ(t.GetByName("sys_non_existent"), std::nullopt);
}

TEST(SyscallTableTest, Arm32) {
  SyscallTable t(Architecture::kArm32);
  EXPECT_STREQ(t.GetById(0), "sys_restart_syscall");
  EXPECT_EQ(t.GetByName("sys_restart_syscall"), 0u);

  EXPECT_STREQ(t.GetById(1), "sys_exit");
  EXPECT_EQ(t.GetByName("sys_exit"), 1u);

  EXPECT_STREQ(t.GetById(190), "sys_vfork");
  EXPECT_EQ(t.GetByName("sys_vfork"), 190u);

  EXPECT_STREQ(t.GetById(399), "sys_io_pgetevents");
  EXPECT_EQ(t.GetByName("sys_io_pgetevents"), 399u);

  EXPECT_STREQ(t.GetById(457), nullptr);
  EXPECT_STREQ(t.GetById(kMaxSyscalls), nullptr);

  EXPECT_EQ(t.GetByName("sys_non_existent"), std::nullopt);
}

TEST(SyscallTableTest, X86_64) {
  SyscallTable t(Architecture::kX86_64);
  EXPECT_STREQ(t.GetById(0), "sys_read");
  EXPECT_EQ(t.GetByName("sys_read"), 0u);

  EXPECT_STREQ(t.GetById(1), "sys_write");
  EXPECT_EQ(t.GetByName("sys_write"), 1u);

  EXPECT_STREQ(t.GetById(58), "sys_vfork");
  EXPECT_EQ(t.GetByName("sys_vfork"), 58u);

  // sys_pwritev2 shows up in two slots, 328 and 547 (which is really
  // compat_sys_pwritev64v2). Ensure we can lookup both.
  EXPECT_STREQ(t.GetById(547), "sys_pwritev2");
  EXPECT_STREQ(t.GetById(328), "sys_pwritev2");
  EXPECT_EQ(t.GetByName("sys_pwritev2"), 328u);

  EXPECT_STREQ(t.GetById(335), "");
  EXPECT_STREQ(t.GetById(511), "");

  EXPECT_STREQ(t.GetById(548), nullptr);
  EXPECT_STREQ(t.GetById(kMaxSyscalls), nullptr);
  EXPECT_EQ(t.GetByName("sys_non_existent"), std::nullopt);
}

TEST(SyscallTableTest, X86) {
  SyscallTable t(Architecture::kX86);
  EXPECT_STREQ(t.GetById(0), "sys_restart_syscall");
  EXPECT_EQ(t.GetByName("sys_restart_syscall"), 0u);

  EXPECT_STREQ(t.GetById(1), "sys_exit");
  EXPECT_EQ(t.GetByName("sys_exit"), 1u);

  EXPECT_STREQ(t.GetById(190), "sys_vfork");
  EXPECT_EQ(t.GetByName("sys_vfork"), 190u);

  EXPECT_STREQ(t.GetById(386), "sys_rseq");
  EXPECT_EQ(t.GetByName("sys_rseq"), 386);

  EXPECT_STREQ(t.GetById(457), nullptr);
  EXPECT_STREQ(t.GetById(kMaxSyscalls), nullptr);
  EXPECT_EQ(t.GetByName("sys_non_existent"), std::nullopt);
}

}  // namespace
}  // namespace perfetto
