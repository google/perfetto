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

#include "src/trace_processor/types/task_state.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace ftrace_utils {
namespace {

using ::testing::ElementsAre;

TEST(TaskStateUnittest, Invalid) {
  TaskState state;
  ASSERT_FALSE(state.is_valid());
}

TEST(TaskStateUnittest, Smoke) {
  auto state = TaskState(static_cast<uint16_t>(0u));
  ASSERT_TRUE(state.is_valid());

  ASSERT_STREQ(state.ToString().data(), "R");
  ASSERT_STREQ(TaskState(1).ToString().data(), "S");
  ASSERT_STREQ(TaskState(2).ToString().data(), "D");
  ASSERT_STREQ(TaskState(4).ToString().data(), "T");
  ASSERT_STREQ(TaskState(8).ToString().data(), "t");
  ASSERT_STREQ(TaskState(16).ToString().data(), "X");
  ASSERT_STREQ(TaskState(32).ToString().data(), "Z");
  ASSERT_STREQ(TaskState(64).ToString().data(), "I");
  ASSERT_STREQ(TaskState(128).ToString().data(), "K");
  ASSERT_STREQ(TaskState(256).ToString().data(), "W");
  ASSERT_STREQ(TaskState(512).ToString().data(), "P");
  ASSERT_STREQ(TaskState(1024).ToString().data(), "N");
}

TEST(TaskStateUnittest, MultipleState) {
  ASSERT_STREQ(TaskState(130).ToString().data(), "DK");
  ASSERT_STREQ(TaskState(258).ToString().data(), "DW");

  ASSERT_EQ(TaskState("D|K").raw_state(), 130);
  ASSERT_EQ(TaskState("D|W").raw_state(), 258);
}

TEST(TaskStateUnittest, KernelVersion) {
  auto state = TaskState(static_cast<uint16_t>(0u), VersionNumber{4, 14});
  ASSERT_TRUE(state.is_valid());

  ASSERT_STREQ(state.ToString().data(), "R");
  ASSERT_STREQ(TaskState(1, VersionNumber{4, 14}).ToString().data(), "S");
  ASSERT_STREQ(TaskState(2, VersionNumber{4, 14}).ToString().data(), "D");
  ASSERT_STREQ(TaskState(4, VersionNumber{4, 14}).ToString().data(), "T");
  ASSERT_STREQ(TaskState(8, VersionNumber{4, 14}).ToString().data(), "t");
  ASSERT_STREQ(TaskState(16, VersionNumber{4, 14}).ToString().data(), "X");
  ASSERT_STREQ(TaskState(32, VersionNumber{4, 14}).ToString().data(), "Z");
  ASSERT_STREQ(TaskState(64, VersionNumber{4, 14}).ToString().data(), "P");
  ASSERT_STREQ(TaskState(128, VersionNumber{4, 14}).ToString().data(), "I");

  // Any without a specific state but less than max are runnable in this kernel.
  ASSERT_STREQ(TaskState(256, VersionNumber{4, 14}).ToString().data(), "R");
  ASSERT_STREQ(TaskState(512, VersionNumber{4, 14}).ToString().data(), "R");
  ASSERT_STREQ(TaskState(1024, VersionNumber{4, 14}).ToString().data(), "R");
  ASSERT_STREQ(TaskState(2048, VersionNumber{4, 14}).ToString().data(), "R");
}

TEST(TaskStateUnittest, MaxValueKernelVersion) {
  // Max value means pre-empted but is different for each kernel version.
  ASSERT_STREQ(TaskState(2048).ToString().data(), "R+");
  ASSERT_STREQ(TaskState(2048, VersionNumber{4, 8}).ToString().data(), "R+");
  ASSERT_STREQ(TaskState(4096, VersionNumber{4, 14}).ToString().data(), "R+");
  ASSERT_STREQ(TaskState(4096, VersionNumber{4, 19}).ToString().data(), "R+");
}

}  // namespace
}  // namespace ftrace_utils
}  // namespace trace_processor
}  // namespace perfetto
