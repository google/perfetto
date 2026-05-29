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

#include "src/trace_processor/parallel_query_config.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

TEST(ParallelQueryConfigTest, UsesConfigWhenEnvUnset) {
  EXPECT_FALSE(ResolveParallelQueriesEnabled(false, nullptr));
  EXPECT_TRUE(ResolveParallelQueriesEnabled(true, nullptr));
}

TEST(ParallelQueryConfigTest, EmptyEnvFallsBackToConfig) {
  EXPECT_FALSE(ResolveParallelQueriesEnabled(false, ""));
  EXPECT_TRUE(ResolveParallelQueriesEnabled(true, ""));
}

TEST(ParallelQueryConfigTest, EnvZeroForcesOff) {
  // The kill-switch overrides config in both directions.
  EXPECT_FALSE(ResolveParallelQueriesEnabled(true, "0"));
  EXPECT_FALSE(ResolveParallelQueriesEnabled(false, "0"));
}

TEST(ParallelQueryConfigTest, EnvNonZeroForcesOn) {
  EXPECT_TRUE(ResolveParallelQueriesEnabled(false, "1"));
  EXPECT_TRUE(ResolveParallelQueriesEnabled(false, "true"));
  EXPECT_TRUE(ResolveParallelQueriesEnabled(true, "1"));
}

}  // namespace
}  // namespace perfetto::trace_processor
