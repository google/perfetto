/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "perfetto/ext/base/sched.h"

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_utils.h"

#include "test/gtest_and_gmock.h"
#include "test/status_matchers.h"

namespace perfetto::base {
namespace {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
TEST(SchedConfigTest, ValidateNiceValue) {
  const Status bad_nice_1 = SchedConfig::ValidateNiceValue(-42);
  ASSERT_FALSE(bad_nice_1.ok());
  ASSERT_STREQ(bad_nice_1.c_message(),
               "Invalid nice value: -42. Valid range is [-20, 19]");

  const Status bad_nice_2 = SchedConfig::ValidateNiceValue(42);
  ASSERT_FALSE(bad_nice_2.ok());
  ASSERT_STREQ(bad_nice_2.c_message(),
               "Invalid nice value: 42. Valid range is [-20, 19]");

  const Status good_nice = SchedConfig::ValidateNiceValue(13);
  ASSERT_OK(good_nice);
}

TEST(SchedConfigTest, ValidatePriority) {
  const Status bad_priority_1 = SchedConfig::ValidatePriority(0);
  ASSERT_FALSE(bad_priority_1.ok());
  ASSERT_STREQ(bad_priority_1.c_message(),
               "Invalid priority: 0. Valid range is [1, 99]");

  const Status bad_priority_2 = SchedConfig::ValidatePriority(100);
  ASSERT_FALSE(bad_priority_2.ok());
  ASSERT_STREQ(bad_priority_2.c_message(),
               "Invalid priority: 100. Valid range is [1, 99]");

  const Status good_priority = SchedConfig::ValidatePriority(42);
  ASSERT_OK(good_priority);
}

TEST(SchedConfigTest, IdleConfig) {
  const SchedConfig idle = SchedConfig::CreateIdle();
  const SchedConfig minimal_userspace = SchedConfig::CreateOther(19);

  ASSERT_LT(idle, minimal_userspace);
  ASSERT_EQ(idle.ToString(), "IDLE(kernel_policy=5, kernel_prio=120)");
}

TEST(SchedConfigTest, UserspaceConfig) {
  const SchedConfig lowest = SchedConfig::CreateOther(19);
  const SchedConfig highest = SchedConfig::CreateOther(-20);
  const SchedConfig highest2 = SchedConfig::CreateOther(-20);
  const SchedConfig inbetween = SchedConfig::CreateOther(0);

  ASSERT_LT(lowest, inbetween);

  ASSERT_LT(lowest, highest);

  ASSERT_LT(inbetween, highest);

  ASSERT_EQ(highest, highest2);

  const SchedConfig inbetween_batch = SchedConfig::CreateBatch(0);
  ASSERT_LT(inbetween_batch, inbetween);
  ASSERT_EQ(inbetween.KernelPriority(), inbetween_batch.KernelPriority());
}

TEST(SchedConfigTest, RealTimeConfigs) {
  const SchedConfig lowestRr = SchedConfig::CreateRr(1);
  const SchedConfig lowestFifo = SchedConfig::CreateFifo(1);
  const SchedConfig highestFifo = SchedConfig::CreateFifo(99);

  ASSERT_EQ(lowestRr.KernelPriority(), lowestFifo.KernelPriority());
  ASSERT_LT(lowestFifo, lowestRr);

  ASSERT_LT(lowestFifo, highestFifo);
  ASSERT_LT(lowestRr, highestFifo);
}

TEST(SchedConfigTest, AllConfigOrdering) {
  const std::set ordered_set{
      SchedConfig::CreateIdle(),     SchedConfig::CreateOther(19),
      SchedConfig::CreateOther(0),   SchedConfig::CreateOther(-20),
      SchedConfig::CreateBatch(19),  SchedConfig::CreateBatch(0),
      SchedConfig::CreateBatch(-20), SchedConfig::CreateRr(1),
      SchedConfig::CreateRr(99),     SchedConfig::CreateFifo(1),
      SchedConfig::CreateFifo(99)};

  std::vector<std::string> strings;
  strings.reserve(ordered_set.size());
  for (const auto& it : ordered_set) {
    strings.emplace_back(it.ToString());
  }

  const std::string actual_string = Join(strings, "\n");

  const auto expected_string = R"(IDLE(kernel_policy=5, kernel_prio=120)
BATCH(nice=19, kernel_policy=3, kernel_prio=139)
OTHER(nice=19, kernel_policy=0, kernel_prio=139)
BATCH(nice=0, kernel_policy=3, kernel_prio=120)
OTHER(nice=0, kernel_policy=0, kernel_prio=120)
BATCH(nice=-20, kernel_policy=3, kernel_prio=100)
OTHER(nice=-20, kernel_policy=0, kernel_prio=100)
FIFO(priority=1, kernel_policy=1, kernel_prio=98)
RR(priority=1, kernel_policy=2, kernel_prio=98)
FIFO(priority=99, kernel_policy=1, kernel_prio=0)
RR(priority=99, kernel_policy=2, kernel_prio=0))";

  ASSERT_STREQ(actual_string.c_str(), expected_string);
}

TEST(SchedManagerTest, TestHasCapabilityToSetSchedPolicy) {
  SchedManager* instance = SchedManager::GetInstance();
  ASSERT_NE(instance, nullptr);
  const bool is_root = geteuid() == 0;
  // Assert we don't crash and return the correct value.
  ASSERT_EQ(is_root, instance->HasCapabilityToSetSchedPolicy());
}

TEST(SchedManagerTest, TestGetAndSetSchedConfig) {
  // Root is required to set the higher priority for the process, but not
  // required to set the lower priority.
  // We don't want all other tests to continue running in this process with
  // reduced priority, so we fork and check try to lower the priority in a
  // child process.
  SchedManager* instance = SchedManager::GetInstance();
  ASSERT_NE(instance, nullptr);
  const auto current = instance->GetCurrentSchedConfig();
  ASSERT_OK(current);
  const SchedConfig initial = current.value();
  if (initial != SchedConfig::CreateDefaultUserspacePolicy()) {
    GTEST_SKIP() << "Skipping test because the current sched policy for the "
                    "test process '"
                 << initial.ToString() << "' is not what we expect";
  }

  // Inspired by UnixSocketTest#SharedMemory test.
  Pipe pipe = Pipe::Create();
  pid_t pid = fork();
  ASSERT_GE(pid, 0);

  if (pid == 0) {
    // Child process.
    const SchedConfig new_value = SchedConfig::CreateOther(1);
    ASSERT_LT(new_value, initial);
    ASSERT_OK(instance->SetSchedConfig(new_value));
    const auto new_current = instance->GetCurrentSchedConfig();
    ASSERT_OK(new_current);
    ASSERT_EQ(new_current.value(), new_value);
    // We can't change the priority to the initial value because it is higher
    // than the current one. We can end the test here.
    exit(HasFailure());
  } else {
    // Parent process.
    int st = 0;
    PERFETTO_EINTR(waitpid(pid, &st, 0));
    ASSERT_FALSE(WIFSIGNALED(st)) << "Child died with signal " << WTERMSIG(st);
    EXPECT_TRUE(WIFEXITED(st));
    ASSERT_EQ(0, WEXITSTATUS(st)) << "Test failed";
  }
}

#else
TEST(SchedManagerTest, TestReportErrorWhenNotSupported) {
  SchedManager* instance = SchedManager::GetInstance();
  ASSERT_NE(instance, nullptr);
  ASSERT_FALSE(instance->IsSupportedOnTheCurrentPlatform());
  ASSERT_FALSE(instance->HasCapabilityToSetSchedPolicy());

  const StatusOr config = instance->GetCurrentSchedConfig();
  ASSERT_FALSE(config.ok());
  ASSERT_STREQ(
      config.status().c_message(),
      "GetCurrentSchedConfig() not implemented on the current platform");

  const Status status = instance->SetSchedConfig(SchedConfig::CreateOther(0));
  ASSERT_FALSE(status.ok());
  ASSERT_STREQ(status.c_message(),
               "SetSchedConfig() not implemented on the current platform");
}
#endif

}  // namespace
}  // namespace perfetto::base
