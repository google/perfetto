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

#include "perfetto/base/string_writer.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace base {
namespace {

TEST(StringWriterTest, BasicCases) {
  char buffer[128];
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendChar('0');
    ASSERT_STREQ(writer.GetCString(), "0");
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendInt(132545);
    ASSERT_STREQ(writer.GetCString(), "132545");
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendPaddedInt<'0', 3>(0);
    ASSERT_STREQ(writer.GetCString(), "000");
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendPaddedInt<'0', 1>(1);
    ASSERT_STREQ(writer.GetCString(), "1");
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendPaddedInt<'0', 3>(1);
    ASSERT_STREQ(writer.GetCString(), "001");
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendPaddedInt<'0', 0>(1);
    ASSERT_STREQ(writer.GetCString(), "1");
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendPaddedInt<' ', 5>(123);
    ASSERT_STREQ(writer.GetCString(), "  123");
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendDouble(123.25);
    ASSERT_STREQ(writer.GetCString(), "123.250000");
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendInt(std::numeric_limits<int64_t>::min());
    ASSERT_STREQ(writer.GetCString(), "-9223372036854775808");
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendInt(std::numeric_limits<int64_t>::max());
    ASSERT_STREQ(writer.GetCString(), "9223372036854775807");
  }

  constexpr char kTestStr[] = "test";
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendLiteral(kTestStr);
    ASSERT_STREQ(writer.GetCString(), kTestStr);
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendString(kTestStr, sizeof(kTestStr) - 1);
    ASSERT_STREQ(writer.GetCString(), kTestStr);
  }
  {
    base::StringWriter writer(buffer, sizeof(buffer));
    writer.AppendString(kTestStr);
    ASSERT_STREQ(writer.GetCString(), kTestStr);
  }
}

TEST(StringWriterTest, WriteAllTypes) {
  char buffer[128];
  base::StringWriter writer(buffer, sizeof(buffer));
  writer.AppendChar('0');
  writer.AppendInt(132545);
  writer.AppendPaddedInt<'0', 0>(1);
  writer.AppendPaddedInt<'0', 3>(0);
  writer.AppendPaddedInt<'0', 1>(1);
  writer.AppendPaddedInt<'0', 2>(1);
  writer.AppendPaddedInt<'0', 3>(1);
  writer.AppendPaddedInt<' ', 5>(123);
  writer.AppendDouble(123.25);

  constexpr char kTestStr[] = "test";
  writer.AppendLiteral(kTestStr);
  writer.AppendString(kTestStr, sizeof(kTestStr) - 1);
  writer.AppendString(kTestStr);

  ASSERT_STREQ(writer.GetCString(),
               "01325451000101001  123123.250000testtesttest");
}

}  // namespace
}  // namespace base
}  // namespace perfetto
