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

#include <string>
#include <utility>
#include <vector>

#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/string_pool.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {
using base::gtest_matchers::IsOk;
using testing::Not;

class RuntimeTableTest : public ::testing::Test {
 protected:
  StringPool pool_;
  std::vector<std::string> names_{{"foo"}};
  RuntimeTable::Builder builder_{&pool_, names_};
};

TEST_F(RuntimeTableTest, DoubleThenIntValid) {
  ASSERT_OK(builder_.AddFloat(0, 1024.3));
  ASSERT_OK(builder_.AddInteger(0, 1ll << 53));
  ASSERT_OK_AND_ASSIGN(auto table, std::move(builder_).Build(2));

  const auto& col = table->columns()[0];
  ASSERT_EQ(col.Get(0).AsDouble(), 1024.3);
  ASSERT_EQ(col.Get(1).AsDouble(), static_cast<double>(1ll << 53));
}

TEST_F(RuntimeTableTest, DoubleThenIntInvalid) {
  ASSERT_OK(builder_.AddFloat(0, 1024.0));
  ASSERT_THAT(builder_.AddInteger(0, (1ll << 53) + 1), Not(IsOk()));
  ASSERT_THAT(builder_.AddInteger(0, -(1ll << 53) - 1), Not(IsOk()));
}

TEST_F(RuntimeTableTest, IntThenDouble) {
  ASSERT_TRUE(builder_.AddInteger(0, 1024).ok());
  ASSERT_TRUE(builder_.AddFloat(0, 1.3).ok());
  ASSERT_OK_AND_ASSIGN(auto table, std::move(builder_).Build(2));

  const auto& col = table->columns()[0];
  ASSERT_EQ(col.Get(0).AsDouble(), 1024.0);
  ASSERT_EQ(col.Get(1).AsDouble(), 1.3);
}

}  // namespace
}  // namespace perfetto::trace_processor
