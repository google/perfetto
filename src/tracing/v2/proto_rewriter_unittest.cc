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

#include "src/tracing/v2/proto_rewriter.h"

#include <stdint.h>

#include <string>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

constexpr size_t kMaxOutput = 1024 * 1024;

std::vector<uint8_t> Bytes(std::initializer_list<int> values) {
  std::vector<uint8_t> out;
  for (int value : values)
    out.push_back(static_cast<uint8_t>(value));
  return out;
}

bool Rewrite(const std::vector<uint8_t>& in,
             std::vector<uint8_t>* out,
             size_t max_output = kMaxOutput) {
  return RewriteToLengthDelimitedProto(in.data(), in.data() + in.size(),
                                       max_output, out);
}

// ---------------------------------------------------------------------------
// The two worked examples from the encoding contract.
// ---------------------------------------------------------------------------

TEST(ProtoRewriterTest, EmptyNestedMessage) {
  // 0b = start field 1, 04 = terminator.
  std::vector<uint8_t> out;
  ASSERT_TRUE(Rewrite(Bytes({0x0b, 0x04}), &out));
  // 0a = length-delimited field 1, then a redundant four-byte zero length.
  EXPECT_EQ(out, Bytes({0x0a, 0x80, 0x80, 0x80, 0x00}));
}

TEST(ProtoRewriterTest, NestedInsideNested) {
  // 0b = start field 1, 13 = start field 2, then two terminators.
  std::vector<uint8_t> out;
  ASSERT_TRUE(Rewrite(Bytes({0x0b, 0x13, 0x04, 0x04}), &out));
  // Field 1 holds five bytes: field 2's tag and its four-byte zero length.
  EXPECT_EQ(
      out, Bytes({0x0a, 0x85, 0x80, 0x80, 0x00, 0x12, 0x80, 0x80, 0x80, 0x00}));
}

// ---------------------------------------------------------------------------
// Ordinary fields pass through untouched.
// ---------------------------------------------------------------------------

TEST(ProtoRewriterTest, ScalarFieldsAreCopiedVerbatim) {
  const std::vector<uint8_t> in = Bytes({
      0x08, 0x2a,                             // field 1 varint 42
      0x11, 1,    2,   3,   4,   5, 6, 7, 8,  // field 2 fixed64
      0x1a, 0x03, 'a', 'b', 'c',              // field 3 bytes "abc"
      0x25, 9,    10,  11,  12,               // field 4 fixed32
  });
  std::vector<uint8_t> out;
  ASSERT_TRUE(Rewrite(in, &out));
  EXPECT_EQ(out, in);
}

TEST(ProtoRewriterTest, ScalarFieldsInsideAndAroundNestedMessages) {
  std::vector<uint8_t> out;
  ASSERT_TRUE(Rewrite(Bytes({
                          0x08, 0x01,  // field 1 varint 1
                          0x13,        // start field 2
                          0x18, 0x02,  // field 3 varint 2 (nested)
                          0x04,        // terminator
                          0x20, 0x03,  // field 4 varint 3
                      }),
                      &out));
  EXPECT_EQ(out, Bytes({
                     0x08, 0x01,                    // field 1
                     0x12, 0x82, 0x80, 0x80, 0x00,  // field 2, len 2
                     0x18, 0x02,                    // field 3
                     0x20, 0x03,                    // field 4
                 }));
}

// The terminator is only special at a field boundary. A payload byte that
// happens to be 0x04 is data.
TEST(ProtoRewriterTest, TerminatorByteInsideAPayloadStaysOpaque) {
  std::vector<uint8_t> out;
  ASSERT_TRUE(
      Rewrite(Bytes({
                  0x0b,                          // start field 1
                  0x12, 0x03, 0x04, 0x04, 0x04,  // field 2 bytes 04 04 04
                  0x04,                          // terminator
              }),
              &out));
  EXPECT_EQ(out, Bytes({
                     0x0a, 0x85, 0x80, 0x80, 0x00,  // field 1, len 5
                     0x12, 0x03, 0x04, 0x04, 0x04,  // field 2, verbatim
                 }));
}

TEST(ProtoRewriterTest, MultiByteOpeningTag) {
  // Field id 1000: tag = (1000 << 3) | 3 = 8003, encoded as c3 3e.
  std::vector<uint8_t> out;
  ASSERT_TRUE(Rewrite(Bytes({0xc3, 0x3e, 0x04}), &out));
  // The same field id as wire type 2: (1000 << 3) | 2 = 8002 -> c2 3e.
  EXPECT_EQ(out, Bytes({0xc2, 0x3e, 0x80, 0x80, 0x80, 0x00}));
}

TEST(ProtoRewriterTest, SiblingsAndDeepNestingRoundTrip) {
  std::vector<uint8_t> out;
  ASSERT_TRUE(Rewrite(Bytes({
                          0x0b,
                          0x04,  // empty field 1
                          0x0b,
                          0x04,  // another empty field 1
                          0x13,
                          0x0b,
                          0x08,
                          0x07,
                          0x04,
                          0x04,
                      }),
                      &out));
  EXPECT_EQ(out,
            Bytes({
                0x0a, 0x80, 0x80, 0x80, 0x00, 0x0a, 0x80, 0x80,
                0x80, 0x00, 0x12, 0x87, 0x80, 0x80, 0x00,  // field 2, len 7
                0x0a, 0x82, 0x80, 0x80, 0x00,              // field 1, len 2
                0x08, 0x07,                                // field 1 varint 7
            }));
}

// ---------------------------------------------------------------------------
// Malformed input.
// ---------------------------------------------------------------------------

TEST(ProtoRewriterTest, StandardEndGroupTagIsRejected) {
  std::vector<uint8_t> out;
  // 0b = start field 1, 0c = the standard end-group tag for field 1. The
  // private framing closes with the bare terminator, so this is not a close.
  EXPECT_FALSE(Rewrite(Bytes({0x0b, 0x0c}), &out));
  EXPECT_TRUE(out.empty());
}

TEST(ProtoRewriterTest, TerminatorAtRootIsRejected) {
  std::vector<uint8_t> out;
  EXPECT_FALSE(Rewrite(Bytes({0x04}), &out));
  EXPECT_TRUE(out.empty());
  EXPECT_FALSE(Rewrite(Bytes({0x0b, 0x04, 0x04}), &out));
  EXPECT_TRUE(out.empty());
}

TEST(ProtoRewriterTest, UnclosedMessageIsRejected) {
  std::vector<uint8_t> out;
  EXPECT_FALSE(Rewrite(Bytes({0x0b}), &out));
  EXPECT_TRUE(out.empty());
  EXPECT_FALSE(Rewrite(Bytes({0x0b, 0x13, 0x04}), &out));
  EXPECT_TRUE(out.empty());
}

TEST(ProtoRewriterTest, MalformedTagsAndValuesAreRejected) {
  std::vector<uint8_t> out;
  // Field id zero.
  EXPECT_FALSE(Rewrite(Bytes({0x00}), &out));
  // Wire types 6 and 7 do not exist.
  EXPECT_FALSE(Rewrite(Bytes({0x0e}), &out));
  EXPECT_FALSE(Rewrite(Bytes({0x0f}), &out));
  // A truncated varint value.
  EXPECT_FALSE(Rewrite(Bytes({0x08, 0x80}), &out));
  // A truncated fixed64.
  EXPECT_FALSE(Rewrite(Bytes({0x11, 1, 2, 3}), &out));
  // A truncated fixed32.
  EXPECT_FALSE(Rewrite(Bytes({0x25, 1, 2}), &out));
  // A length-delimited field claiming more bytes than are present.
  EXPECT_FALSE(Rewrite(Bytes({0x1a, 0x10, 'a'}), &out));
  // A truncated tag.
  EXPECT_FALSE(Rewrite(Bytes({0x80}), &out));
  EXPECT_TRUE(out.empty());
}

TEST(ProtoRewriterTest, ExcessiveNestingIsRejected) {
  std::vector<uint8_t> in;
  for (int i = 0; i < 64; ++i)
    in.push_back(0x0b);
  std::vector<uint8_t> deep = in;
  deep.push_back(0x0b);  // one level past the bound
  for (int i = 0; i < 65; ++i)
    deep.push_back(0x04);

  std::vector<uint8_t> out;
  EXPECT_FALSE(Rewrite(deep, &out));
  EXPECT_TRUE(out.empty());

  // Exactly at the bound is still accepted.
  for (int i = 0; i < 64; ++i)
    in.push_back(0x04);
  EXPECT_TRUE(Rewrite(in, &out));
  EXPECT_FALSE(out.empty());
}

TEST(ProtoRewriterTest, OutputOverflowIsRejectedAndClearsOutput) {
  std::vector<uint8_t> out;
  // One empty nested message costs five output bytes.
  EXPECT_TRUE(Rewrite(Bytes({0x0b, 0x04}), &out, /*max_output=*/5));
  EXPECT_EQ(out.size(), 5u);
  EXPECT_FALSE(Rewrite(Bytes({0x0b, 0x04}), &out, /*max_output=*/4));
  EXPECT_TRUE(out.empty());
}

// ---------------------------------------------------------------------------
// A real packet.
// ---------------------------------------------------------------------------

TEST(ProtoRewriterTest, GeneratedTracePacketRoundTrips) {
  protozero::HeapBuffered<protos::pbzero::TracePacket> packet;
  packet.Reset(protozero::NestedMessageEncoding::kStartTagAndTerminator);
  packet->set_timestamp(1234567);
  auto* test_event = packet->set_for_testing();
  test_event->set_str("hello");
  auto* payload = test_event->set_payload();
  payload->set_single_int(42);
  payload->add_str("nested");
  packet->set_trusted_packet_sequence_id(9);
  const std::vector<uint8_t> private_bytes = packet.SerializeAsArray();

  std::vector<uint8_t> canonical;
  ASSERT_TRUE(Rewrite(private_bytes, &canonical));

  protos::gen::TracePacket decoded;
  ASSERT_TRUE(decoded.ParseFromArray(canonical.data(), canonical.size()));
  EXPECT_EQ(decoded.timestamp(), 1234567u);
  EXPECT_EQ(decoded.trusted_packet_sequence_id(), 9u);
  ASSERT_TRUE(decoded.has_for_testing());
  EXPECT_EQ(decoded.for_testing().str(), "hello");
  EXPECT_EQ(decoded.for_testing().payload().single_int(), 42);
  ASSERT_EQ(decoded.for_testing().payload().str().size(), 1u);
  EXPECT_EQ(decoded.for_testing().payload().str()[0], "nested");

  // The private bytes are not a valid protobuf on their own: the whole point is
  // that they need this pass.
  protos::gen::TracePacket not_decoded;
  EXPECT_FALSE(
      not_decoded.ParseFromArray(private_bytes.data(), private_bytes.size()));
}

}  // namespace
}  // namespace perfetto::tracing_v2
