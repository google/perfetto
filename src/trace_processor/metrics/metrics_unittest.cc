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

#include "src/trace_processor/metrics/metrics.h"

#include <vector>

#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace metrics {

namespace {

std::string RunTemplateReplace(
    const std::string& str,
    std::unordered_map<std::string, std::string> subs) {
  std::string out;
  EXPECT_EQ(TemplateReplace(str, subs, &out), 0);
  return out;
}

TEST(MetricsTest, TemplateReplace) {
  auto res = RunTemplateReplace("no templates here", {});
  ASSERT_EQ(res, "no templates here");

  res = RunTemplateReplace("{{justtemplate}}", {{"justtemplate", "result"}});
  ASSERT_EQ(res, "result");

  res = RunTemplateReplace("{{temp1}} {{temp2}}!",
                           {{"temp1", "hello"}, {"temp2", "world"}});
  ASSERT_EQ(res, "hello world!");

  std::string unused;
  ASSERT_NE(TemplateReplace("{{missing}}", {{}}, &unused), 0);
}

}  // namespace

}  // namespace metrics
}  // namespace trace_processor
}  // namespace perfetto
