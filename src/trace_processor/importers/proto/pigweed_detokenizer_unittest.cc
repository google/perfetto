/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/pigweed_detokenizer.h"

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/protozero/field.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::pigweed {
namespace {

void AppendUint32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xFF));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

// Encodes a signed integer the way Pigweed does: zigzag, then varint.
void AppendZigZagVarInt(std::vector<uint8_t>* out, int64_t value) {
  uint64_t zigzag = (static_cast<uint64_t>(value) << 1) ^
                    static_cast<uint64_t>(value >> 63);
  do {
    uint8_t byte = static_cast<uint8_t>(zigzag & 0x7F);
    zigzag >>= 7;
    if (zigzag != 0) {
      byte |= 0x80;
    }
    out->push_back(byte);
  } while (zigzag != 0);
}

// Builds a single-entry Pigweed token database mapping `token` to `format`.
std::vector<uint8_t> BuildDatabase(uint32_t token, const std::string& format) {
  std::vector<uint8_t> db;
  // Header.
  const char magic[] = {'T', 'O', 'K', 'E', 'N', 'S'};
  db.insert(db.end(), magic, magic + sizeof(magic));
  db.push_back(0);  // version low
  db.push_back(0);  // version high
  AppendUint32(&db, 1);  // entry_count
  AppendUint32(&db, 0);  // reserved
  // Entry: {token, date_removed}. 0xFFFFFFFF means "live".
  AppendUint32(&db, token);
  AppendUint32(&db, 0xFFFFFFFF);
  // String table.
  db.insert(db.end(), format.begin(), format.end());
  db.push_back('\0');
  return db;
}

PigweedDetokenizer MakeDetokenizer(const std::vector<uint8_t>& db) {
  protozero::ConstBytes bytes{db.data(), db.size()};
  auto detok = CreateDetokenizer(bytes);
  PERFETTO_CHECK(detok.ok());
  return std::move(detok.value());
}

TEST(PigweedDetokenizerTest, PlainSignedInt) {
  auto db = BuildDatabase(0x1234, "value=%d");
  auto detok = MakeDetokenizer(db);

  std::vector<uint8_t> payload;
  AppendUint32(&payload, 0x1234);
  AppendZigZagVarInt(&payload, 42);

  auto result = detok.Detokenize({payload.data(), payload.size()});
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->Format(), "value=42");
}

// A '*' field width consumes an extra integer argument from the payload. The
// detokenizer must resolve it rather than passing a single value to vsnprintf
// (which would otherwise read uninitialized memory).
TEST(PigweedDetokenizerTest, StarWidth) {
  auto db = BuildDatabase(0x1234, "[%*d]");
  auto detok = MakeDetokenizer(db);

  std::vector<uint8_t> payload;
  AppendUint32(&payload, 0x1234);
  AppendZigZagVarInt(&payload, 5);   // width
  AppendZigZagVarInt(&payload, 42);  // value

  auto result = detok.Detokenize({payload.data(), payload.size()});
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->Format(), "[   42]");
}

// A '*' precision consumes an extra integer argument.
TEST(PigweedDetokenizerTest, StarPrecision) {
  auto db = BuildDatabase(0x1234, "[%.*f]");
  auto detok = MakeDetokenizer(db);

  std::vector<uint8_t> payload;
  AppendUint32(&payload, 0x1234);
  AppendZigZagVarInt(&payload, 2);  // precision
  float value = 3.14159f;
  const uint8_t* value_bytes = reinterpret_cast<const uint8_t*>(&value);
  payload.insert(payload.end(), value_bytes, value_bytes + sizeof(value));

  auto result = detok.Detokenize({payload.data(), payload.size()});
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->Format(), "[3.14]");
}

// Both width and precision as wildcards: two extra integer arguments.
TEST(PigweedDetokenizerTest, StarWidthAndPrecision) {
  auto db = BuildDatabase(0x1234, "[%*.*f]");
  auto detok = MakeDetokenizer(db);

  std::vector<uint8_t> payload;
  AppendUint32(&payload, 0x1234);
  AppendZigZagVarInt(&payload, 8);  // width
  AppendZigZagVarInt(&payload, 2);  // precision
  float value = 3.14159f;
  const uint8_t* value_bytes = reinterpret_cast<const uint8_t*>(&value);
  payload.insert(payload.end(), value_bytes, value_bytes + sizeof(value));

  auto result = detok.Detokenize({payload.data(), payload.size()});
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->Format(), "[    3.14]");
}

// A negative precision argument must be treated as if precision were omitted
// (per printf semantics); it must not corrupt the format string.
TEST(PigweedDetokenizerTest, NegativeStarPrecision) {
  auto db = BuildDatabase(0x1234, "[%.*d]");
  auto detok = MakeDetokenizer(db);

  std::vector<uint8_t> payload;
  AppendUint32(&payload, 0x1234);
  AppendZigZagVarInt(&payload, -1);  // precision (negative => omitted)
  AppendZigZagVarInt(&payload, 42);  // value

  auto result = detok.Detokenize({payload.data(), payload.size()});
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->Format(), "[42]");
}

}  // namespace
}  // namespace perfetto::trace_processor::pigweed
