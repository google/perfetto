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

#include "src/android_sdk/jni/perfetto_app_filter.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace jni {
namespace {

TEST(PerfettoAppFilterTest, AllowlistedProcess) {
  // Allowlisted platform process
  EXPECT_TRUE(IsAllowlistedProcess("system_server"));

  // Allowlisted apps
  EXPECT_TRUE(IsAllowlistedProcess("com.google.android.youtube"));
  EXPECT_TRUE(IsAllowlistedProcess("com.google.android.googlequicksearchbox"));
  EXPECT_TRUE(IsAllowlistedProcess("com.android.systemui"));

  // Multi-process sub-processes of allowlisted apps
  EXPECT_TRUE(
      IsAllowlistedProcess("com.google.android.youtube:sandboxed_process0"));
  EXPECT_TRUE(IsAllowlistedProcess("com.google.android.youtube:crash_handler"));
  EXPECT_TRUE(
      IsAllowlistedProcess("com.google.android.googlequicksearchbox:search"));
  EXPECT_TRUE(IsAllowlistedProcess("com.android.systemui:screenshot"));

  // Non-allowlisted apps
  EXPECT_FALSE(IsAllowlistedProcess("com.example.unrelated"));
  EXPECT_FALSE(IsAllowlistedProcess("com.example.unrelated:subproc"));
  EXPECT_FALSE(IsAllowlistedProcess("com.android.settings"));

  // Suffix matching or substring tricks should NOT match
  EXPECT_FALSE(IsAllowlistedProcess("com.google.android.youtube.fake"));
  EXPECT_FALSE(IsAllowlistedProcess("my.com.google.android.youtube"));

  // Empty process name
  EXPECT_FALSE(IsAllowlistedProcess(""));
}

TEST(PerfettoAppFilterTest, InProcessBackendAlwaysAllowed) {
  EXPECT_TRUE(IsAppRegistrationAllowed(/*is_backend_in_process=*/true));
}

#if !defined(__ANDROID__)
TEST(PerfettoAppFilterTest, HostAlwaysAllowed) {
  EXPECT_TRUE(IsAppRegistrationAllowed(/*is_backend_in_process=*/false));
}
#endif

TEST(PerfettoAppFilterTest, GetProcessCmdlineMemoized) {
  const std::string& cmdline1 = GetProcessCmdline();
  const std::string& cmdline2 = GetProcessCmdline();
  // Verifies memoization returns the identical memory address without
  // re-reading
  EXPECT_EQ(&cmdline1, &cmdline2);
}

}  // namespace
}  // namespace jni
}  // namespace perfetto
