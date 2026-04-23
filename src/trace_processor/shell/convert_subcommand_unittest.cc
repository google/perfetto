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

#include "src/trace_processor/shell/convert_subcommand.h"

#include <algorithm>
#include <cstring>

#include "src/trace_processor/shell/subcommand.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::shell {
namespace {

// Fast in-process check that the expected flags are registered on the
// ConvertSubcommand. End-to-end behaviour (including --proguard-map
// propagation to traceconv) is covered by the subprocess-based shell
// integration tests (TraceProcessorShellIntegrationTest.ConvertBundle*).
TEST(ConvertSubcommandTest, FlagsRegistered) {
  ConvertSubcommand cmd;
  auto flags = cmd.GetFlags();
  auto has = [&](const char* name) {
    return std::any_of(flags.begin(), flags.end(), [name](const FlagSpec& f) {
      return std::strcmp(f.long_name, name) == 0;
    });
  };
  EXPECT_TRUE(has("proguard-map"));
  EXPECT_TRUE(has("symbol-paths"));
  EXPECT_TRUE(has("no-auto-symbol-paths"));
  EXPECT_TRUE(has("no-auto-proguard-maps"));
  EXPECT_TRUE(has("verbose"));
}

}  // namespace
}  // namespace perfetto::trace_processor::shell
