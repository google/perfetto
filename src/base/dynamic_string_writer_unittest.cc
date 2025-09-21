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

#include "perfetto/ext/base/dynamic_string_writer.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(DynamicStringWriterTest, BasicCases) {
  {
    base::DynamicStringWriter writer;
    writer.AppendChar('0');
    ASSERT_EQ(writer.GetStringView().ToStdString(), "0");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendInt(132545);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "132545");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendUnsignedInt(523);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "523");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendDouble(123.25);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "123.250000");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendInt(std::numeric_limits<int64_t>::min());
    ASSERT_EQ(writer.GetStringView().ToStdString(), "-9223372036854775808");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendInt(std::numeric_limits<int64_t>::max());
    ASSERT_EQ(writer.GetStringView().ToStdString(), "9223372036854775807");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendUnsignedInt(std::numeric_limits<uint64_t>::max());
    ASSERT_EQ(writer.GetStringView().ToStdString(), "18446744073709551615");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendBool(true);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "true");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendBool(false);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "false");
  }

  constexpr char kTestStr[] = "test";
  {
    base::DynamicStringWriter writer;
    writer.AppendLiteral(kTestStr);
    ASSERT_EQ(writer.GetStringView().ToStdString(), kTestStr);
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendString(kTestStr, sizeof(kTestStr) - 1);
    ASSERT_EQ(writer.GetStringView().ToStdString(), kTestStr);
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendString(kTestStr);
    ASSERT_EQ(writer.GetStringView().ToStdString(), kTestStr);
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendChar('x', 5);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "xxxxx");
  }
}

TEST(DynamicStringWriterTest, WriteAllTypes) {
  base::DynamicStringWriter writer;
  writer.AppendChar('0');
  writer.AppendInt(132545);
  writer.AppendUnsignedInt(523);
  writer.AppendDouble(123.25);
  writer.AppendBool(true);

  constexpr char kTestStr[] = "test";
  writer.AppendLiteral(kTestStr);
  writer.AppendString(kTestStr, sizeof(kTestStr) - 1);
  writer.AppendString(kTestStr);

  ASSERT_EQ(writer.GetStringView().ToStdString(),
            "0132545523123.250000truetesttesttest");
}

}  // namespace
}  // namespace base
}  // namespace perfetto
