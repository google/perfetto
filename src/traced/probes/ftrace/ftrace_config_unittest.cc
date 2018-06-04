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

#include "src/traced/probes/ftrace/ftrace_config.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::Contains;

namespace perfetto {
namespace {

TEST(ConfigTest, FtraceEventsAsSet) {
  FtraceConfig config;
  *config.add_ftrace_events() = "aaa";
  *config.add_ftrace_events() = "bbb";
  *config.add_ftrace_events() = "aaa";

  EXPECT_EQ(FtraceEventsAsSet(config), std::set<std::string>({
                                           "aaa", "bbb",
                                       }));
}

TEST(ConfigTest, CreateFtraceConfig) {
  FtraceConfig config = CreateFtraceConfig({
      "aaa", "bbb",
  });

  EXPECT_THAT(config.ftrace_events(), Contains("aaa"));
  EXPECT_THAT(config.ftrace_events(), Contains("bbb"));
}

}  // namespace
}  // namespace perfetto
