/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/db/runtime_table.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

class RuntimeTableTest : public ::testing::Test {
 protected:
  StringPool pool_;
  std::vector<std::string> names_{{"foo"}};
  RuntimeTable table_{&pool_, names_};
};

TEST_F(RuntimeTableTest, DoubleThenIntValid) {
  ASSERT_TRUE(table_.AddFloat(0, 1024.3).ok());
  ASSERT_TRUE(table_.AddInteger(0, 1ll << 53).ok());
  ASSERT_TRUE(table_.AddColumnsAndOverlays(2).ok());

  const auto& col = table_.columns()[0];
  ASSERT_EQ(col.Get(0).AsDouble(), 1024.3);
  ASSERT_EQ(col.Get(1).AsDouble(), static_cast<double>(1ll << 53));
}

TEST_F(RuntimeTableTest, DoubleThenIntInvalid) {
  ASSERT_TRUE(table_.AddFloat(0, 1024.0).ok());
  ASSERT_FALSE(table_.AddInteger(0, (1ll << 53) + 1).ok());
  ASSERT_FALSE(table_.AddInteger(0, -(1ll << 53) - 1).ok());
}

TEST_F(RuntimeTableTest, IntThenDouble) {
  ASSERT_TRUE(table_.AddInteger(0, 1024).ok());
  ASSERT_TRUE(table_.AddFloat(0, 1.3).ok());
  ASSERT_TRUE(table_.AddColumnsAndOverlays(2).ok());

  const auto& col = table_.columns()[0];
  ASSERT_EQ(col.Get(0).AsDouble(), 1024.0);
  ASSERT_EQ(col.Get(1).AsDouble(), 1.3);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
