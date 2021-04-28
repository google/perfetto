/*
 * Copyright 2021 The Android Open Source Project
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

#include "perfetto/base/logging.h"

#include <stdint.h>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

char g_last_line[256];

TEST(LoggingTest, Basic) {
  SetLogMessageCallback(nullptr);
  LogMessage(kLogDebug, "file.cc", 100, "test message %d", 1);

  SetLogMessageCallback(+[](LogMessageCallbackArgs log) {
    sprintf(g_last_line, "%d:%s:%d:%s", log.level, log.filename, log.line,
            log.message);
  });

  g_last_line[0] = 0;
  LogMessage(kLogDebug, "file.cc", 101, "test message %d", 2);
  ASSERT_STREQ(g_last_line, "0:file.cc:101:test message 2");

  g_last_line[0] = 0;
  SetLogMessageCallback(nullptr);
  LogMessage(kLogDebug, "file.cc", 102, "test message %d", 3);
  ASSERT_STREQ(g_last_line, "");
}

}  // namespace
}  // namespace base
}  // namespace perfetto
