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

#include <cstring>
#include <limits>
#include <string>

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
    writer.AppendPaddedInt<'0', 3>(0);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "000");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedInt<'0', 1>(1);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "1");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedInt<'0', 3>(1);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "001");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedInt<'0', 0>(1);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "1");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedInt<' ', 5>(123);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "  123");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedUnsignedInt<' ', 5>(123);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "  123");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendDouble(123.25);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "123.25");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendDouble(1e100);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "1e+100");
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
  writer.AppendPaddedInt<'0', 0>(1);
  writer.AppendPaddedInt<'0', 3>(0);
  writer.AppendPaddedInt<'0', 1>(1);
  writer.AppendPaddedInt<'0', 2>(1);
  writer.AppendPaddedInt<'0', 3>(1);
  writer.AppendPaddedInt<' ', 5>(123);
  writer.AppendPaddedUnsignedInt<' ', 5>(456);
  writer.AppendDouble(123.25);
  writer.AppendBool(true);

  constexpr char kTestStr[] = "test";
  writer.AppendLiteral(kTestStr);
  writer.AppendString(kTestStr, sizeof(kTestStr) - 1);
  writer.AppendString(kTestStr);

  ASSERT_EQ(writer.GetStringView().ToStdString(),
            "01325455231000101001  123  456123.25truetesttesttest");
}

TEST(DynamicStringWriterTest, PaddedHexInt) {
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(0xAB, '0', 0);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "ab");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(0xAB, '0', 1);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "ab");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(0xAB, '0', 2);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "ab");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(0xAB, '0', 4);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "00ab");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(0xAB, ' ', 5);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "   ab");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(static_cast<uint8_t>(0xFF), '0', 2);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "ff");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(static_cast<uint32_t>(0x12345678), '0', 8);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "12345678");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(static_cast<uint64_t>(0xFF), '0', 2);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "ff");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(static_cast<uint64_t>(0x123456789abcdef0), ' ',
                              16);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "123456789abcdef0");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(static_cast<uint64_t>(0x123456789abcdef0), ' ',
                              18);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "  123456789abcdef0");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(0, '0', 3);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "000");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(0, ' ', 0);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "0");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendPaddedHexInt(0, ' ', 3);
    ASSERT_EQ(writer.GetStringView().ToStdString(), "  0");
  }
}

TEST(DynamicStringWriterTest, HexInt) {
  {
    base::DynamicStringWriter writer;
    writer.AppendHexInt(uint64_t{0xABCD});
    ASSERT_EQ(writer.GetStringView().ToStdString(), "abcd");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendHexInt(uint64_t{0});
    writer.AppendHexInt(uint64_t{1});
    writer.AppendHexInt(uint64_t{15});
    writer.AppendHexInt(uint64_t{16});
    writer.AppendHexInt(uint64_t{255});
    ASSERT_EQ(writer.GetStringView().ToStdString(), "01f10ff");
  }
  {
    base::DynamicStringWriter writer;
    writer.AppendHexInt(std::numeric_limits<uint64_t>::max());
    ASSERT_EQ(writer.GetStringView().ToStdString(), "ffffffffffffffff");
  }
}

TEST(DynamicStringWriterTest, HexBuffer) {
  {
    base::DynamicStringWriter writer;
    const uint8_t data[] = {0x12, 0x34, 0x56, 0x78};
    writer.AppendHexString(data, sizeof(data), '-');
    ASSERT_EQ(writer.GetStringView().ToStdString(), "12-34-56-78");
  }
  {
    base::DynamicStringWriter writer;
    const uint8_t data[] = {0xAA, 0xBB, 0xCC};
    writer.AppendHexString(data, sizeof(data), ':');
    ASSERT_EQ(writer.GetStringView().ToStdString(), "aa:bb:cc");
  }
  {
    base::DynamicStringWriter writer;
    const uint8_t data[] = {0xAA, 0xBB, 0xCC};
    writer.AppendHexString(data, 0, '-');
    ASSERT_EQ(writer.GetStringView().ToStdString(), "");
  }
  {
    base::DynamicStringWriter writer;
    const uint8_t data[] = {0xFF};
    writer.AppendHexString(data, sizeof(data), '-');
    ASSERT_EQ(writer.GetStringView().ToStdString(), "ff");
  }
  {
    base::DynamicStringWriter writer;
    base::StringView sv("ABC");
    writer.AppendHexString(sv, '-');
    ASSERT_EQ(writer.GetStringView().ToStdString(), "41-42-43");
  }
  {
    base::DynamicStringWriter writer;
    uint8_t large_data[100];
    for (int i = 0; i < 100; i++) {
      large_data[i] = static_cast<uint8_t>(i % 256);
    }
    writer.AppendHexString(large_data, sizeof(large_data), '-');

    // Should only print the first 64 bytes.
    std::string expected =
        "00-01-02-03-04-05-06-07-08-09-0a-0b-0c-0d-0e-0f-"
        "10-11-12-13-14-15-16-17-18-19-1a-1b-1c-1d-1e-1f-"
        "20-21-22-23-24-25-26-27-28-29-2a-2b-2c-2d-2e-2f-"
        "30-31-32-33-34-35-36-37-38-39-3a-3b-3c-3d-3e-3f";
    ASSERT_EQ(writer.GetStringView().ToStdString(), expected);
  }
}

TEST(DynamicStringWriterTest, CombinedHexOperations) {
  base::DynamicStringWriter writer;

  const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
  writer.AppendHexString(data, sizeof(data), '-');
  writer.AppendPaddedHexInt(0x12345678, ' ', 10);
  writer.AppendHexInt(uint64_t{0xFF});

  ASSERT_EQ(writer.GetStringView().ToStdString(), "de-ad-be-ef  12345678ff");
}

TEST(DynamicStringWriterTest, CreateStringCopy) {
  base::DynamicStringWriter writer;
  writer.AppendLiteral("hello");

  auto copy = writer.CreateStringCopy();
  ASSERT_NE(copy.get(), nullptr);
  ASSERT_STREQ(copy.get(), "hello");

  // Mutating the writer afterwards must not affect the copy.
  writer.Clear();
  writer.AppendLiteral("world");
  ASSERT_STREQ(copy.get(), "hello");
}

TEST(DynamicStringWriterTest, ClearReusesCapacity) {
  base::DynamicStringWriter writer;
  writer.AppendString(std::string(4096, 'x').c_str(), 4096);
  ASSERT_EQ(writer.pos(), 4096u);

  writer.Clear();
  ASSERT_EQ(writer.pos(), 0u);

  writer.AppendLiteral("abc");
  ASSERT_EQ(writer.GetStringView().ToStdString(), "abc");
}

TEST(DynamicStringWriterTest, GrowsBeyondInitial) {
  // Confirm the writer accepts inputs far larger than any historical stack
  // buffer in the codebase, without overflow.
  std::string huge(64 * 1024, 'a');
  base::DynamicStringWriter writer;
  writer.AppendString(huge.c_str(), huge.size());
  ASSERT_EQ(writer.pos(), huge.size());
  ASSERT_EQ(writer.GetStringView().size(), huge.size());
  ASSERT_EQ(memcmp(writer.GetStringView().data(), huge.data(), huge.size()), 0);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
