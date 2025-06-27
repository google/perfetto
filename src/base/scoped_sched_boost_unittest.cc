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
#include "perfetto/base/thread_utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)

#include <sys/resource.h>
#include "test/gtest_and_gmock.h"

namespace perfetto::base {
namespace {

int GetCurThreadPrio() {
  return getpriority(PRIO_PROCESS, GetThreadId());
}

TEST(ScopedSchedBoostTest, Boost) {
  {
    EXPECT_EQ(GetCurThreadPrio(), 0);
    setpriority(PRIO_PROCESS, GetThreadId(), 2);
    // auto boost = ScopedSchedBoost::Boost({SchedPolicyAndPrio::kSchedOther,
    // -2}); EXPECT_TRUE(boost.ok());
    EXPECT_EQ(GetCurThreadPrio(), 2);
  }
  EXPECT_EQ(GetCurThreadPrio(), 0);
}

}  // namespace
}  // namespace perfetto::base

#endif