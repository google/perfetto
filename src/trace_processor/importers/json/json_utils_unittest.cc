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

#include "src/trace_processor/importers/json/json_utils.h"

#include <json/config.h>
#include <json/value.h>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::json {
namespace {

TEST(JsonTraceUtilsTest, CoerceToUint32) {
  ASSERT_EQ(CoerceToUint32(Json::Value(42)).value_or(0), 42u);
  ASSERT_EQ(CoerceToUint32(Json::Value("42")).value_or(0), 42u);
  ASSERT_EQ(CoerceToInt64(Json::Value(42.1)).value_or(-1), 42);
}

TEST(JsonTraceUtilsTest, CoerceToInt64) {
  ASSERT_EQ(CoerceToInt64(Json::Value(42)).value_or(-1), 42);
  ASSERT_EQ(CoerceToInt64(Json::Value("42")).value_or(-1), 42);
  ASSERT_EQ(CoerceToInt64(Json::Value(42.1)).value_or(-1), 42);
  ASSERT_FALSE(CoerceToInt64(Json::Value("foo")).has_value());
  ASSERT_FALSE(CoerceToInt64(Json::Value("1234!")).has_value());

  Json::UInt64 n = 18446744073709551615UL;
  ASSERT_EQ(CoerceToInt64(Json::Value{n}).value_or(0), -1);
}

TEST(JsonTraceUtilsTest, CoerceToTs) {
  ASSERT_EQ(CoerceToTs(Json::Value(42)).value_or(-1), 42000);
  ASSERT_EQ(CoerceToTs(Json::Value("42")).value_or(-1), 42000);
  ASSERT_EQ(CoerceToTs(Json::Value(42.1)).value_or(-1), 42100);
  ASSERT_EQ(CoerceToTs(Json::Value("42.1")).value_or(-1), 42100);
  ASSERT_EQ(CoerceToTs(Json::Value(".42")).value_or(-1), 420);
  ASSERT_EQ(CoerceToTs(Json::Value("42.")).value_or(-1), 42000);
  ASSERT_EQ(CoerceToTs(Json::Value("42.0")).value_or(-1), 42000);
  ASSERT_EQ(CoerceToTs(Json::Value("0.2")).value_or(-1), 200);
  ASSERT_EQ(CoerceToTs(Json::Value("0.2e-1")).value_or(-1), 20);
  ASSERT_EQ(CoerceToTs(Json::Value("0.2e-2")).value_or(-1), 2);
  ASSERT_EQ(CoerceToTs(Json::Value("0.2e-3")).value_or(-1), 0);
  ASSERT_EQ(CoerceToTs(Json::Value("1.692108548132154500e+15")).value_or(-1),
            1'692'108'548'132'154'500);
  ASSERT_EQ(CoerceToTs(Json::Value("1692108548132154.500")).value_or(-1),
            1'692'108'548'132'154'500);
  ASSERT_EQ(CoerceToTs(Json::Value("1.692108548132154501e+15")).value_or(-1),
            1'692'108'548'132'154'501);
  ASSERT_EQ(CoerceToTs(Json::Value("1692108548132154.501")).value_or(-1),
            1'692'108'548'132'154'501);
  ASSERT_EQ(CoerceToTs(Json::Value("-1.692108548132154500E+15")).value_or(-1),
            -1'692'108'548'132'154'500);
  ASSERT_EQ(CoerceToTs(Json::Value("-1692108548132154.500")).value_or(-1),
            -1'692'108'548'132'154'500);
  ASSERT_EQ(CoerceToTs(Json::Value("-1.692108548132154501E+15")).value_or(-1),
            -1'692'108'548'132'154'501);
  ASSERT_EQ(CoerceToTs(Json::Value("-1692108548132154.501")).value_or(-1),
            -1'692'108'548'132'154'501);
  ASSERT_EQ(CoerceToTs(Json::Value("-0")).value_or(-1), 0);
  ASSERT_EQ(CoerceToTs(Json::Value("0")).value_or(-1), 0);
  ASSERT_EQ(CoerceToTs(Json::Value(".")).value_or(-1), 0);
  ASSERT_FALSE(CoerceToTs(Json::Value("1234!")).has_value());
  ASSERT_FALSE(CoerceToTs(Json::Value("123e4!")).has_value());
}

}  // namespace
}  // namespace perfetto::trace_processor::json
