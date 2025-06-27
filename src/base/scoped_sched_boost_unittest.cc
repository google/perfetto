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

#include "perfetto/ext/base/scoped_sched_boost.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

#include <sched.h>
#include <sys/resource.h>

#include "perfetto/base/thread_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::base {
namespace {

id_t GetTid() {
  return static_cast<id_t>(GetThreadId());
}

int GetCurThreadPrio() {
  return getpriority(PRIO_PROCESS, GetTid());
}

int GetCurRtPrio() {
  EXPECT_EQ(sched_getscheduler(0), SCHED_FIFO);
  struct sched_param param{};
  sched_getparam(0, &param);
  return param.sched_priority;
}

class ScopedSchedBoostTest : public testing::Test {
 public:
  void SetUp() override {
    initial_prio = GetCurThreadPrio();
    ScopedSchedBoost::ResetForTesting();
  }
  void TearDown() override {
    struct sched_param param{};
    sched_setscheduler(0, SCHED_OTHER, &param);
    setpriority(PRIO_PROCESS, GetTid(), initial_prio);
  }

  int initial_prio = 0;
};

TEST_F(ScopedSchedBoostTest, SchedOther) {
  {
    auto boost = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedOther, 5});
    EXPECT_TRUE(boost.ok());
    EXPECT_EQ(GetCurThreadPrio(), -5);
    {
      auto ignored =
          ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedOther, 3});
      EXPECT_TRUE(ignored.ok());
      EXPECT_EQ(GetCurThreadPrio(), -5);  // Should be still -5

      {
        auto ignored2 =
            ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedOther, 3});
        EXPECT_TRUE(ignored2.ok());
        EXPECT_EQ(GetCurThreadPrio(), -5);  // Should be still -5
      }

      auto reboost =
          ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedOther, 7});
      EXPECT_TRUE(reboost.ok());
      EXPECT_EQ(GetCurThreadPrio(), -7);

      auto reboost2 =
          ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedOther, 7});
      EXPECT_TRUE(reboost2.ok());
      EXPECT_EQ(GetCurThreadPrio(), -7);
    }
    EXPECT_EQ(GetCurThreadPrio(), -5);
  }
  EXPECT_EQ(GetCurThreadPrio(), initial_prio);
}

TEST_F(ScopedSchedBoostTest, SchedFifo) {
  {
    auto boost2 = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedFifo, 2});
    EXPECT_TRUE(boost2.ok());
    EXPECT_EQ(GetCurRtPrio(), 2);

    auto boost1 = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedFifo, 1});
    EXPECT_TRUE(boost1.ok());
    EXPECT_EQ(GetCurRtPrio(), 2);

    auto boost5 = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedFifo, 5});
    EXPECT_TRUE(boost5.ok());
    EXPECT_EQ(GetCurRtPrio(), 5);
  }
  EXPECT_EQ(sched_getscheduler(0), SCHED_OTHER);
  EXPECT_EQ(GetCurThreadPrio(), initial_prio);
}

TEST_F(ScopedSchedBoostTest, ReturnToInitialSched) {
  struct sched_param param{};
  ASSERT_EQ(sched_setscheduler(0, SCHED_BATCH, &param), 0);
  ASSERT_EQ(sched_getscheduler(0), SCHED_BATCH);

  ScopedSchedBoost::ResetForTesting();

  {
    auto boost = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedFifo, 2});
    EXPECT_TRUE(boost.ok());
    EXPECT_EQ(GetCurRtPrio(), 2);
  }

  EXPECT_EQ(sched_getscheduler(0), SCHED_BATCH);
}

TEST_F(ScopedSchedBoostTest, SchedFifoAndOther) {
  auto boost = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedOther, 0});
  {
    auto boost2 = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedFifo, 2});
    EXPECT_TRUE(boost2.ok());
    EXPECT_EQ(GetCurRtPrio(), 2);

    {
      auto boost1 =
          ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedOther, 5});
      EXPECT_TRUE(boost1.ok());
      EXPECT_EQ(GetCurRtPrio(), 2);
    }
    auto boost3 = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedFifo, 3});
    EXPECT_TRUE(boost3.ok());
    EXPECT_EQ(GetCurRtPrio(), 3);
  }
  EXPECT_EQ(sched_getscheduler(0), SCHED_OTHER);
}

TEST_F(ScopedSchedBoostTest, MoveOperator) {
  auto other = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedOther, 0});
  {
    auto boost1 = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedFifo, 1});
    auto boost2 = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedFifo, 2});
    EXPECT_EQ(GetCurRtPrio(), 2);

    boost2 = std::move(boost1);
    EXPECT_EQ(GetCurRtPrio(), 1);
  }
  EXPECT_EQ(sched_getscheduler(0), SCHED_OTHER);
}

}  // namespace
}  // namespace perfetto::base

#endif
