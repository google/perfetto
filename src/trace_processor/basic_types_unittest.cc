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

#include "perfetto/trace_processor/basic_types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(SqlValueTest, DifferentTypes) {
  ASSERT_LT(SqlValue(), SqlValue::Long(10));
  ASSERT_LT(SqlValue::Long(10), SqlValue::Double(10.0));
  ASSERT_LT(SqlValue::Double(10.0), SqlValue::String("10"));
}

TEST(SqlValueTest, CompareLong) {
  SqlValue int32_min = SqlValue::Long(std::numeric_limits<int32_t>::min());
  SqlValue minus_1 = SqlValue::Long(-1);
  SqlValue zero = SqlValue::Long(0);
  SqlValue uint32_max = SqlValue::Long(std::numeric_limits<uint32_t>::max());

  ASSERT_LT(int32_min, minus_1);
  ASSERT_LT(int32_min, uint32_max);
  ASSERT_LT(minus_1, uint32_max);

  ASSERT_GT(uint32_max, zero);

  ASSERT_EQ(zero, zero);
}

TEST(SqlValueTest, CompareDouble) {
  SqlValue int32_min = SqlValue::Double(std::numeric_limits<int32_t>::min());
  SqlValue minus_1 = SqlValue::Double(-1.0);
  SqlValue zero = SqlValue::Double(0);
  SqlValue uint32_max = SqlValue::Double(std::numeric_limits<uint32_t>::max());

  ASSERT_LT(int32_min, minus_1);
  ASSERT_LT(int32_min, uint32_max);
  ASSERT_LT(minus_1, uint32_max);

  ASSERT_GT(uint32_max, zero);

  ASSERT_EQ(zero, zero);
}

TEST(SqlValueTest, CompareString) {
  SqlValue a = SqlValue::String("a");
  SqlValue aa = SqlValue::String("aa");
  SqlValue b = SqlValue::String("b");

  ASSERT_LT(a, aa);
  ASSERT_LT(aa, b);
  ASSERT_LT(a, b);

  ASSERT_GT(aa, a);

  ASSERT_EQ(a, a);
  ASSERT_EQ(aa, SqlValue::String("aa"));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
