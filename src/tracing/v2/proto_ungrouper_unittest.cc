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

#include "src/tracing/v2/proto_ungrouper.h"

#include <initializer_list>
#include <limits>
#include <vector>

#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace tracing_v2 {
namespace {

using ::testing::ElementsAreArray;

std::vector<uint8_t> Bytes(std::initializer_list<int> bytes) {
  std::vector<uint8_t> result;
  for (int byte : bytes)
    result.push_back(static_cast<uint8_t>(byte));
  return result;
}

std::vector<uint8_t> RedundantLength(uint32_t size) {
  std::vector<uint8_t> result(protozero::proto_utils::kMessageLengthFieldSize);
  protozero::proto_utils::WriteRedundantVarInt(size, result.data());
  return result;
}

void Append(std::vector<uint8_t>* destination,
            const std::vector<uint8_t>& source) {
  destination->insert(destination->end(), source.begin(), source.end());
}

bool Ungroup(const std::vector<uint8_t>& input, std::vector<uint8_t>* output) {
  return UngroupProtoBytes(input.data(), input.data() + input.size(),
                           std::numeric_limits<size_t>::max(), output);
}

TEST(ProtoUngrouperTest, PassesScalarsAndConvertsNestedGroups) {
  const std::vector<uint8_t> grouped = Bytes({
      protozero::proto_utils::MakeTagVarInt(1),
      7,
      protozero::proto_utils::MakeTagStartGroup(2),
      protozero::proto_utils::MakeTagLengthDelimited(3),
      2,
      'h',
      'i',
      protozero::proto_utils::MakeTagEndGroup(2),
  });
  std::vector<uint8_t> expected =
      Bytes({protozero::proto_utils::MakeTagVarInt(1), 7,
             protozero::proto_utils::MakeTagLengthDelimited(2)});
  Append(&expected, RedundantLength(4));
  Append(&expected, Bytes({protozero::proto_utils::MakeTagLengthDelimited(3), 2,
                           'h', 'i'}));

  std::vector<uint8_t> canonical;
  ASSERT_TRUE(Ungroup(grouped, &canonical));
  EXPECT_THAT(canonical, ElementsAreArray(expected));
}

TEST(ProtoUngrouperTest, AcceptsEmptyRootMessage) {
  std::vector<uint8_t> canonical = {1, 2, 3};
  const uint8_t empty = 0;
  EXPECT_TRUE(
      UngroupProtoBytes(&empty, &empty, /*max_output_size=*/0, &canonical));
  EXPECT_TRUE(canonical.empty());
}

TEST(ProtoUngrouperTest, RejectsMalformedInput) {
  const std::vector<std::vector<uint8_t>> malformed = {
      Bytes({0}),
      Bytes({protozero::proto_utils::MakeTagVarInt(1), 0x80}),
      Bytes({protozero::proto_utils::MakeTagLengthDelimited(1), 3, 'x'}),
      Bytes({protozero::proto_utils::MakeTagStartGroup(1)}),
      Bytes({protozero::proto_utils::MakeTagStartGroup(1),
             protozero::proto_utils::MakeTagEndGroup(2)}),
      Bytes({protozero::proto_utils::MakeTagEndGroup(1)}),
      Bytes({(1 << 3) | 6}),
  };
  for (const auto& input : malformed) {
    std::vector<uint8_t> canonical;
    EXPECT_FALSE(Ungroup(input, &canonical));
    EXPECT_TRUE(canonical.empty());
  }
}

TEST(ProtoUngrouperTest, RoundTripsGeneratedTracePacket) {
  auto fill_packet = [](protos::pbzero::TracePacket* packet) {
    packet->set_timestamp(123456789);
    packet->set_trusted_packet_sequence_id(42);
    auto* payload = packet->set_for_testing()->set_payload();
    payload->add_str("alpha");
    payload->add_str("beta");
    payload->set_single_int(-7);
    payload->add_nested()->set_single_string("nested");
  };

  protozero::HeapBuffered<protos::pbzero::TracePacket> grouped;
  grouped->set_nested_messages_as_groups();
  fill_packet(grouped.get());
  const std::vector<uint8_t> grouped_bytes = grouped.SerializeAsArray();

  protozero::HeapBuffered<protos::pbzero::TracePacket> canonical;
  fill_packet(canonical.get());
  const std::vector<uint8_t> canonical_bytes = canonical.SerializeAsArray();
  ASSERT_NE(grouped_bytes, canonical_bytes);

  std::vector<uint8_t> converted;
  ASSERT_TRUE(Ungroup(grouped_bytes, &converted));
  protos::gen::TracePacket parsed_grouped;
  ASSERT_TRUE(
      parsed_grouped.ParseFromArray(converted.data(), converted.size()));
  protos::gen::TracePacket parsed_canonical;
  ASSERT_TRUE(parsed_canonical.ParseFromArray(canonical_bytes.data(),
                                              canonical_bytes.size()));
  EXPECT_EQ(parsed_grouped.SerializeAsString(),
            parsed_canonical.SerializeAsString());
}

TEST(ProtoUngrouperTest, EnforcesExpandedOutputLimit) {
  const std::vector<uint8_t> grouped =
      Bytes({protozero::proto_utils::MakeTagStartGroup(1),
             protozero::proto_utils::MakeTagEndGroup(1)});
  std::vector<uint8_t> converted;
  EXPECT_FALSE(UngroupProtoBytes(grouped.data(),
                                 grouped.data() + grouped.size(),
                                 /*max_output_size=*/4, &converted));
  EXPECT_TRUE(converted.empty());
}

}  // namespace
}  // namespace tracing_v2
}  // namespace perfetto
