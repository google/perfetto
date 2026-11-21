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

#include "src/shared_lib/track_event/encoded_fields.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include "perfetto/protozero/proto_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::shlib {
namespace {

void WritePlaceholder(uint8_t* bytes) {
  std::fill(bytes, bytes + PERFETTO_TE_HL_IID_SLOT_SIZE - 1, 0x80);
  bytes[PERFETTO_TE_HL_IID_SLOT_SIZE - 1] = 0;
}

TEST(EncodedFieldsTest, FullWidthIidRoundTrip) {
  for (uint64_t iid :
       {uint64_t{0}, uint64_t{1}, uint64_t{1} << 28, uint64_t{1} << 63,
        std::numeric_limits<uint64_t>::max()}) {
    std::array<uint8_t, PERFETTO_TE_HL_IID_SLOT_SIZE> bytes{};
    size_t size = WriteEncodedIid(iid, bytes.data());
    // Positive values must not use a padded ten-byte varint: protobuf-java's
    // fast path treats the ninth continuation bit as uint64 bit 63.
    EXPECT_EQ(size == PERFETTO_TE_HL_IID_SLOT_SIZE, iid >= (uint64_t{1} << 63));
    if (iid < 128)
      EXPECT_EQ(size, 1u);
    uint64_t decoded = 0;
    EXPECT_EQ(protozero::proto_utils::ParseVarInt(
                  bytes.data(), bytes.data() + size, &decoded),
              bytes.data() + size);
    EXPECT_EQ(decoded, iid);
  }
}

TEST(EncodedFieldsTest, NestedAndroidJobNameSlot) {
  // Synthetic outer field 1000 wraps AndroidJobSchedulerJob.job_name_iid (21).
  // Its definition comes from InternedData.android_job_name (44).
  std::array<uint8_t, 15> bytes{0xC2, 0x3E, 12, 0xA8, 0x01};
  WritePlaceholder(bytes.data() + 5);
  PerfettoTeHlIidSlot slot{5, 44, "job-name", 8};
  PerfettoTeHlEventFields fields{bytes.data(), bytes.size(), &slot, 1};
  EXPECT_TRUE(PerfettoTeHlValidateEventFields(&fields));
  const auto unchanged = bytes;
  EncodedFieldLengths lengths;
  EXPECT_TRUE(ValidateEncodedFields(&fields, 128, &lengths));
  ASSERT_EQ(lengths.size(), 1u);
  EXPECT_EQ(lengths[0].offset, 2u);
  EXPECT_EQ(lengths[0].width, 1u);
  EXPECT_EQ(lengths[0].payload_size, 12u);
  EXPECT_EQ(lengths[0].first_slot, 0u);
  EXPECT_EQ(lengths[0].end_slot, 1u);
  EXPECT_EQ(bytes, unchanged);
}

TEST(EncodedFieldsTest, RejectsInvalidSlotLayout) {
  std::array<uint8_t, 22> bytes{};
  bytes[0] = 8;
  bytes[11] = 16;
  WritePlaceholder(bytes.data() + 1);
  WritePlaceholder(bytes.data() + 12);
  std::array<PerfettoTeHlIidSlot, 2> slots{{{1, 44, "a", 1}, {12, 44, "b", 1}}};
  PerfettoTeHlEventFields fields{bytes.data(), bytes.size(), slots.data(), 2};
  ASSERT_TRUE(PerfettoTeHlValidateEventFields(&fields));
  slots[1].offset = 1;
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  slots[1].offset = 2;
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  slots[1].offset = std::numeric_limits<uint32_t>::max();
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  slots[1].offset = 12;
  slots[0].interned_type_id = 0;
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  slots[0].interned_type_id = 0x20000000;
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  slots[0].interned_type_id = 44;
  slots[0].str = nullptr;
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  slots[0].str_size = 0;
  EXPECT_TRUE(PerfettoTeHlValidateEventFields(&fields));
  bytes[10] = 1;
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
}

TEST(EncodedFieldsTest, RejectsMalformedWireAndPointerPairs) {
  EXPECT_TRUE(PerfettoTeHlValidateEventFields(nullptr));
  PerfettoTeHlEventFields fields{};
  EXPECT_TRUE(PerfettoTeHlValidateEventFields(&fields));
  fields.size = 1;
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  const uint8_t truncated[] = {0x12, 4, 'a'};
  fields = {truncated, sizeof(truncated), nullptr, 0};
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  const uint8_t bad_tag[] = {0};
  fields = {bad_tag, sizeof(bad_tag), nullptr, 0};
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  fields = {};
  fields.num_iid_slots = std::numeric_limits<size_t>::max();
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
}

TEST(EncodedFieldsTest, BoundsCombinedPayloadBeforeReadingStrings) {
  std::array<uint8_t, 11> bytes{8};
  WritePlaceholder(bytes.data() + 1);
  PerfettoTeHlIidSlot slot{1, 44, "", 0};
  PerfettoTeHlEventFields fields{bytes.data(), bytes.size(), &slot, 1};
  const size_t available = PERFETTO_TE_HL_MAX_ENCODED_EVENT_SIZE -
                           bytes.size() - PERFETTO_TE_HL_IID_SLOT_RECORD_SIZE;
  EXPECT_TRUE(ValidateEncodedFields(&fields, available));
  EXPECT_FALSE(ValidateEncodedFields(&fields, available + 1));
  slot.str_size = std::numeric_limits<size_t>::max();
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  EXPECT_FALSE(ValidateEncodedFields(
      nullptr, PERFETTO_TE_HL_MAX_ENCODED_EVENT_SIZE + 1));
}

TEST(EncodedFieldsTest, CollectsNestedAndSiblingLengthRanges) {
  std::array<uint8_t, 39> bytes{};
  bytes[0] = 18;
  bytes[1] = 24;
  bytes[2] = 8;
  WritePlaceholder(bytes.data() + 3);
  bytes[13] = 18;
  bytes[14] = 11;
  bytes[15] = 8;
  WritePlaceholder(bytes.data() + 16);
  bytes[26] = 26;
  bytes[27] = 11;
  bytes[28] = 8;
  WritePlaceholder(bytes.data() + 29);
  const PerfettoTeHlIidSlot slots[] = {
      {3, 44, "a", 1}, {16, 44, "b", 1}, {29, 44, "c", 1}};
  PerfettoTeHlEventFields fields{bytes.data(), bytes.size(), slots, 3};
  EncodedFieldLengths lengths;
  ASSERT_TRUE(ValidateEncodedFields(&fields, 0, &lengths));
  ASSERT_EQ(lengths.size(), 3u);
  EXPECT_EQ(lengths[0].offset, 1u);
  EXPECT_EQ(lengths[0].first_slot, 0u);
  EXPECT_EQ(lengths[0].end_slot, 2u);
  EXPECT_EQ(lengths[1].offset, 14u);
  EXPECT_EQ(lengths[1].first_slot, 1u);
  EXPECT_EQ(lengths[1].end_slot, 2u);
  EXPECT_EQ(lengths[2].offset, 27u);
  EXPECT_EQ(lengths[2].first_slot, 2u);
  EXPECT_EQ(lengths[2].end_slot, 3u);
}

TEST(EncodedFieldsTest, RejectsSlotsInOpaqueBytesAndLongLengthPrefixes) {
  std::array<uint8_t, 12> opaque{18, 10};
  WritePlaceholder(opaque.data() + 2);
  PerfettoTeHlIidSlot slot{2, 44, "", 0};
  PerfettoTeHlEventFields fields{opaque.data(), opaque.size(), &slot, 1};
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
  fields.num_iid_slots = 0;
  EXPECT_TRUE(PerfettoTeHlValidateEventFields(&fields));

  std::vector<uint8_t> bytes{18, 0x8b, 0x80, 0x80, 0, 8};
  bytes.resize(bytes.size() + PERFETTO_TE_HL_IID_SLOT_SIZE);
  WritePlaceholder(bytes.data() + 6);
  slot.offset = 6;
  fields = {bytes.data(), bytes.size(), &slot, 1};
  EXPECT_TRUE(PerfettoTeHlValidateEventFields(&fields));
  bytes.insert(bytes.begin() + 4, 0x80);
  slot.offset = 7;
  fields = {bytes.data(), bytes.size(), &slot, 1};
  EXPECT_FALSE(PerfettoTeHlValidateEventFields(&fields));
}

TEST(EncodedFieldsTest, BoundsSlotContainingNesting) {
  std::vector<uint8_t> bytes(11);
  bytes[0] = 8;
  WritePlaceholder(bytes.data() + 1);
  PerfettoTeHlIidSlot slot{1, 44, "", 0};
  for (size_t depth = 1; depth <= 65; ++depth) {
    uint32_t payload_size = static_cast<uint32_t>(bytes.size());
    bytes.insert(bytes.begin(), 5, 0);
    bytes[0] = 18;
    protozero::proto_utils::WriteRedundantVarInt(payload_size,
                                                 bytes.data() + 1);
    slot.offset += 5;
    PerfettoTeHlEventFields fields{bytes.data(), bytes.size(), &slot, 1};
    EXPECT_EQ(PerfettoTeHlValidateEventFields(&fields), depth <= 64);
  }
}

}  // namespace
}  // namespace perfetto::shlib
