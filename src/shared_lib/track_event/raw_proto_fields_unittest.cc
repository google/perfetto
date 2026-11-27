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

#include "src/shared_lib/track_event/raw_proto_fields.h"

#include <array>
#include <cstdint>
#include <vector>

#include "perfetto/public/abi/track_event_hl_abi.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::shlib {
namespace {

template <size_t N>
bool Validate(const std::array<uint8_t, N>& bytes) {
  return ValidateRawProtoFields(bytes.data(), bytes.size(), /*extra_bytes=*/0);
}

TEST(RawProtoFieldsTest, AcceptsAllValidFieldIds) {
  // Field 2^29 - 1 (the largest id), varint 1. Tag 0xFFFFFFF8 as a varint.
  EXPECT_TRUE(
      Validate(std::array<uint8_t, 6>{0xF8, 0xFF, 0xFF, 0xFF, 0x0F, 1}));
  // Field 2^24 (above protozero's decoder limit), varint 1.
  EXPECT_TRUE(Validate(std::array<uint8_t, 5>{0x80, 0x80, 0x80, 0x40, 1}));
  // Fixed32 and fixed64 values.
  EXPECT_TRUE(Validate(std::array<uint8_t, 5>{0x0D, 1, 2, 3, 4}));
  EXPECT_TRUE(Validate(std::array<uint8_t, 9>{0x09, 1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(RawProtoFieldsTest, RejectsInvalidTags) {
  // Field id 0.
  EXPECT_FALSE(Validate(std::array<uint8_t, 2>{0x00, 1}));
  // Field id 2^29 (a tag wider than 32 bits).
  EXPECT_FALSE(
      Validate(std::array<uint8_t, 6>{0x80, 0x80, 0x80, 0x80, 0x10, 1}));
  // Groups (wire types 3 and 4) and invalid wire types (6 and 7).
  for (int wire_type : {3, 4, 6, 7}) {
    EXPECT_FALSE(Validate(
        std::array<uint8_t, 2>{static_cast<uint8_t>(8 | wire_type), 1}));
  }
  // A bad field after a valid one.
  EXPECT_FALSE(Validate(std::array<uint8_t, 4>{0x08, 1, 0x00, 1}));
}

TEST(RawProtoFieldsTest, RejectsTruncatedValues) {
  EXPECT_FALSE(Validate(std::array<uint8_t, 1>{0x08}));
  EXPECT_FALSE(Validate(std::array<uint8_t, 2>{0x08, 0x80}));
  EXPECT_FALSE(Validate(std::array<uint8_t, 4>{0x0D, 1, 2, 3}));
  EXPECT_FALSE(Validate(std::array<uint8_t, 8>{0x09, 1, 2, 3, 4, 5, 6, 7}));
  EXPECT_FALSE(Validate(std::array<uint8_t, 3>{0x12, 4, 'a'}));
}

TEST(RawProtoFieldsTest, KeepsPayloadsOpaque) {
  // Synthetic outer field 1000 wraps a nested message.
  EXPECT_TRUE(Validate(std::array<uint8_t, 6>{0xC2, 0x3E, 3, 0xA8, 0x01, 7}));
  // Garbage inside a length-delimited payload is not inspected.
  EXPECT_TRUE(Validate(std::array<uint8_t, 4>{0x12, 2, 0xFF, 0xFF}));
}

TEST(RawProtoFieldsTest, ChecksPointerAndSize) {
  EXPECT_TRUE(ValidateRawProtoFields(nullptr, 0, 0));
  EXPECT_FALSE(ValidateRawProtoFields(nullptr, 1, 0));
}

TEST(RawProtoFieldsTest, BoundsFieldsAndNameTogether) {
  std::vector<uint8_t> bytes(2 * 1024);
  for (size_t i = 0; i < bytes.size(); i += 2) {
    bytes[i] = 0x08;  // Field 1, varint 0.
  }
  const size_t available =
      PERFETTO_TE_HL_MAX_RAW_PROTO_EVENT_SIZE - bytes.size();
  EXPECT_TRUE(ValidateRawProtoFields(bytes.data(), bytes.size(), available));
  EXPECT_FALSE(
      ValidateRawProtoFields(bytes.data(), bytes.size(), available + 1));
  EXPECT_FALSE(ValidateRawProtoFields(
      nullptr, 0, PERFETTO_TE_HL_MAX_RAW_PROTO_EVENT_SIZE + 1));
}

}  // namespace
}  // namespace perfetto::shlib
