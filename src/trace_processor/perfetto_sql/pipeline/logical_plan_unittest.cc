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

#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::pipeline {
namespace {

TEST(NameTableTest, EachSpellingOnce) {
  NameTable names;
  NameId ts = names.Intern("ts");
  EXPECT_EQ(names.Intern("ts"), ts);
  EXPECT_EQ(names.text(ts), "ts");
  EXPECT_EQ(names.size(), 1u);
}

// Names match ignoring case, as SQL's do, but each keeps its spelling.
TEST(NameTableTest, CaseDiffersButMatches) {
  NameTable names;
  NameId lower = names.Intern("dur");
  NameId upper = names.Intern("DUR");
  NameId mixed = names.Intern("Dur");
  NameId other = names.Intern("ts");
  EXPECT_NE(lower, upper);
  EXPECT_EQ(names.text(upper), "DUR");
  EXPECT_EQ(names.text(mixed), "Dur");
  EXPECT_TRUE(names.Same(lower, upper));
  EXPECT_TRUE(names.Same(upper, mixed));
  EXPECT_FALSE(names.Same(lower, other));
}

}  // namespace
}  // namespace perfetto::trace_processor::pipeline
