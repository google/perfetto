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

#include "src/trace_processor/importers/fuchsia/fuchsia_trace_utils.h"

#include <cstdint>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace fuchsia_trace_utils {
namespace {

TEST(FuchsiaTraceUtilsTest, TicksToNs) {
  ASSERT_EQ(TicksToNs(40, 10), 4000000000);

  // Case where naive computation would overflow
  ASSERT_EQ(TicksToNs(40000000000, 1000000000), 40000000000);

  // Test detecting overflow
  ASSERT_EQ(TicksToNs(40000000000, 1), -1);
}

TEST(FuchsiaTraceUtilsTest, ReadInlineString) {
  base::StringView out;

  // Partial word and full word with no NUL (fills buffer exactly).
  const uint8_t kBuf[8] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  {
    RecordCursor c(kBuf, sizeof(kBuf));
    ASSERT_TRUE(c.ReadInlineString(0x8005, &out));
    EXPECT_EQ(out, "abcde");
    EXPECT_EQ(c.WordIndex(), 1u);
  }
  {
    RecordCursor c(kBuf, sizeof(kBuf));
    ASSERT_TRUE(c.ReadInlineString(0x8008, &out));
    EXPECT_EQ(out, "abcdefgh");
    EXPECT_EQ(c.WordIndex(), 1u);
  }

  // Empty strings (0, inline 0, and nullptr buffer) do not advance the cursor.
  {
    RecordCursor c(kBuf, sizeof(kBuf));
    ASSERT_TRUE(c.ReadInlineString(0, &out));
    EXPECT_TRUE(out.empty());
    EXPECT_NE(out.data(), nullptr);
    EXPECT_EQ(c.WordIndex(), 0u);
    ASSERT_TRUE(c.ReadInlineString(0x8000, &out));
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(c.WordIndex(), 0u);
  }
  {
    RecordCursor empty(nullptr, 0);
    ASSERT_TRUE(empty.ReadInlineString(0, &out));
    EXPECT_TRUE(out.empty());
  }

  // Skipping (nullptr output) and out-of-bounds reads.
  {
    RecordCursor c(kBuf, sizeof(kBuf));
    ASSERT_TRUE(c.ReadInlineString(0x8008, nullptr));
    EXPECT_EQ(c.WordIndex(), 1u);
    EXPECT_FALSE(c.ReadInlineString(0x8001, nullptr));
  }
  {
    RecordCursor c(kBuf, sizeof(kBuf));
    EXPECT_FALSE(c.ReadInlineString(0x8009, &out));
  }
}

TEST(FuchsiaTraceUtilsTest, ReadInlineStringTruncatesAtFirstNul) {
  base::StringView out;

  // Trailing NUL padding spanning two 64-bit words still advances 2 words.
  {
    const uint8_t kBuf[16] = {'m', 'y', '_', 'p', 'r', 'o', 'c', 0,
                              0,   0,   0,   0,   0,   0,   0,   0};
    RecordCursor c(kBuf, sizeof(kBuf));
    ASSERT_TRUE(c.ReadInlineString(0x800C, &out));
    EXPECT_EQ(out, "my_proc");
    EXPECT_EQ(c.WordIndex(), 2u);
  }

  // Embedded NUL drops trailing bytes.
  {
    const uint8_t kBuf[8] = {'f', 'o', 'o', 0, 'b', 'a', 'r', '!'};
    RecordCursor c(kBuf, sizeof(kBuf));
    ASSERT_TRUE(c.ReadInlineString(0x8008, &out));
    EXPECT_EQ(out, "foo");
    EXPECT_EQ(c.WordIndex(), 1u);
  }

  // All-NUL payload becomes an empty non-null StringView.
  {
    const uint8_t kBuf[8] = {0, 0, 0, 0, 'x', 'y', 'z', 'w'};
    RecordCursor c(kBuf, sizeof(kBuf));
    ASSERT_TRUE(c.ReadInlineString(0x8004, &out));
    EXPECT_TRUE(out.empty());
    EXPECT_NE(out.data(), nullptr);
    EXPECT_EQ(c.WordIndex(), 1u);
  }
}

}  // namespace
}  // namespace fuchsia_trace_utils
}  // namespace trace_processor
}  // namespace perfetto
