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

#include "perfetto/ext/base/ctrl_c_handler.h"

#include "perfetto/base/build_config.h"
#include "test/gtest_and_gmock.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <signal.h>

namespace perfetto {
namespace base {
namespace {

TEST(CtrlCHandlerTest, HandlesSigintAndSigterm) {
  static volatile sig_atomic_t last_signal = 0;
  InstallCtrlCHandler([](int sig) { last_signal = sig; });

  raise(SIGINT);
  EXPECT_EQ(last_signal, SIGINT);

  raise(SIGTERM);
  EXPECT_EQ(last_signal, SIGTERM);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
#endif
