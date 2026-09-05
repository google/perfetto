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

RewriteResult Rewrite(const std::vector<uint8_t>& in,
                      std::vector<uint8_t>* out,
                      size_t max_output = kMaxOutput) {
  return RewriteProtoGroupToLengthDelimited(in.data(), in.data() + in.size(),
                                            out, max_output);
}

// |depth| empty nested messages of field 1 inside each other.
std::vector<uint8_t> NestedTo(size_t depth) {
  std::vector<uint8_t> in(depth, 0x0b);
  in.insert(in.end(), depth, 0x04);
  return in;
}

// ---------------------------------------------------------------------------
// The two worked examples from the encoding contract.
// ---------------------------------------------------------------------------

TEST(ProtoRewriterTest, EmptyNestedMessage) {
  // 0b = start field 1, 04 = end field 1.
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(Bytes({0x0b, 0x04}), &out), RewriteResult::kSuccess);
  // 0a = length-delimited field 1, then a redundant four-byte zero length.
  EXPECT_EQ(out, Bytes({0x0a, 0x80, 0x80, 0x80, 0x00}));
}

TEST(ProtoRewriterTest, NestedInsideNested) {
  // 0b = start field 1, 13 = start field 2, then two end bytes.
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(Bytes({0x0b, 0x13, 0x04, 0x04}), &out),
            RewriteResult::kSuccess);
  // Field 1 holds five bytes: field 2's tag and its four-byte zero length.
  EXPECT_EQ(
      out, Bytes({0x0a, 0x85, 0x80, 0x80, 0x00, 0x12, 0x80, 0x80, 0x80, 0x00}));
}

// ---------------------------------------------------------------------------
// Ordinary fields pass through untouched.
// ---------------------------------------------------------------------------

TEST(ProtoRewriterTest, EmptyInput) {
  std::vector<uint8_t> out = Bytes({0xff});
  EXPECT_EQ(Rewrite({}, &out), RewriteResult::kSuccess);
  EXPECT_TRUE(out.empty());
}

TEST(ProtoRewriterTest, ScalarFieldsAreCopiedVerbatim) {
  const std::vector<uint8_t> in = Bytes({
      0x08, 0x2a,                             // field 1 varint 42
      0x11, 1,    2,   3,   4,   5, 6, 7, 8,  // field 2 fixed64
      0x1a, 0x03, 'a', 'b', 'c',              // field 3 bytes "abc"
      0x25, 9,    10,  11,  12,               // field 4 fixed32
  });
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(in, &out), RewriteResult::kSuccess);
  EXPECT_EQ(out, in);
}

TEST(ProtoRewriterTest, ScalarFieldsInsideAndAroundNestedMessages) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(Bytes({
                        0x08, 0x01,  // field 1 varint 1
                        0x13,        // start field 2
                        0x18, 0x02,  // field 3 varint 2 (nested)
                        0x04,        // end field 2
                        0x20, 0x03,  // field 4 varint 3
                    }),
                    &out),
            RewriteResult::kSuccess);
  EXPECT_EQ(out, Bytes({
                     0x08, 0x01,                    // field 1
                     0x12, 0x82, 0x80, 0x80, 0x00,  // field 2, len 2
                     0x18, 0x02,                    // field 3
                     0x20, 0x03,                    // field 4
                 }));
}

// The end byte is only special at a field boundary. A payload byte that
// happens to be 0x04 is data.
TEST(ProtoRewriterTest, ProtoGroupEndByteInsideAPayloadStaysOpaque) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(Bytes({
                        0x0b,                          // start field 1
                        0x12, 0x03, 0x04, 0x04, 0x04,  // field 2 bytes 04 04 04
                        0x04,                          // end field 1
                    }),
                    &out),
            RewriteResult::kSuccess);
  EXPECT_EQ(out, Bytes({
                     0x0a, 0x85, 0x80, 0x80, 0x00,  // field 1, len 5
                     0x12, 0x03, 0x04, 0x04, 0x04,  // field 2, verbatim
                 }));
}

TEST(ProtoRewriterTest, FieldIdBoundaries) {
  std::vector<uint8_t> out;
  // Field 15 is the largest id whose tag fits in one byte: (15 << 3) | 3.
  ASSERT_EQ(Rewrite(Bytes({0x7b, 0x04}), &out), RewriteResult::kSuccess);
  EXPECT_EQ(out, Bytes({0x7a, 0x80, 0x80, 0x80, 0x00}));

  // Field 16 needs two tag bytes: (16 << 3) | 3 = 0x83 -> 83 01.
  ASSERT_EQ(Rewrite(Bytes({0x83, 0x01, 0x04}), &out), RewriteResult::kSuccess);
  EXPECT_EQ(out, Bytes({0x82, 0x01, 0x80, 0x80, 0x80, 0x00}));

  // Field id 1000: tag = (1000 << 3) | 3 = 8003, encoded as c3 3e.
  ASSERT_EQ(Rewrite(Bytes({0xc3, 0x3e, 0x04}), &out), RewriteResult::kSuccess);
  EXPECT_EQ(out, Bytes({0xc2, 0x3e, 0x80, 0x80, 0x80, 0x00}));

  // The largest legal id, 2^29 - 1, as a varint field: tag 0xfffffff8.
  const std::vector<uint8_t> max_id_field =
      Bytes({0xf8, 0xff, 0xff, 0xff, 0x0f, 0x2a});
  ASSERT_EQ(Rewrite(max_id_field, &out), RewriteResult::kSuccess);
  EXPECT_EQ(out, max_id_field);
  // And as a nested message: the rewritten tag is 0xfffffffa.
  ASSERT_EQ(Rewrite(Bytes({0xfb, 0xff, 0xff, 0xff, 0x0f, 0x04}), &out),
            RewriteResult::kSuccess);
  EXPECT_EQ(out, Bytes({0xfa, 0xff, 0xff, 0xff, 0x0f, 0x80, 0x80, 0x80, 0x00}));

  // One past it, id 2^29 with wire type 0: tag 2^32 -> 80 80 80 80 10.
  EXPECT_EQ(Rewrite(Bytes({0x80, 0x80, 0x80, 0x80, 0x10, 0x2a}), &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

TEST(ProtoRewriterTest, VarintValueBoundaries) {
  std::vector<uint8_t> out;
  const std::vector<uint8_t> in = Bytes({
      0x08, 0x7f,                                // one byte: 127
      0x08, 0x80, 0x01,                          // two bytes: 128
      0x08, 0xff, 0xff, 0xff, 0xff, 0x0f,        // five bytes: 2^32 - 1
      0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // ten bytes: 2^64 - 1
      0xff, 0xff, 0xff, 0x01,
  });
  ASSERT_EQ(Rewrite(in, &out), RewriteResult::kSuccess);
  EXPECT_EQ(out, in);

  // Eleven bytes is more than a 64-bit varint can take.
  EXPECT_EQ(Rewrite(Bytes({0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                           0xff, 0xff, 0x01}),
                    &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

TEST(ProtoRewriterTest, SiblingsAndDeepNestingRoundTrip) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(Bytes({
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
                    &out),
            RewriteResult::kSuccess);
  EXPECT_EQ(out,
            Bytes({
                0x0a, 0x80, 0x80, 0x80, 0x00, 0x0a, 0x80, 0x80,
                0x80, 0x00, 0x12, 0x87, 0x80, 0x80, 0x00,  // field 2, len 7
                0x0a, 0x82, 0x80, 0x80, 0x00,              // field 1, len 2
                0x08, 0x07,                                // field 1 varint 7
            }));
}

// No per-level state, so protos nest as deep as protozero lets them.
TEST(ProtoRewriterTest, NestingWellBeyondSixtyFourLevels) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(NestedTo(65), &out), RewriteResult::kSuccess);
  // Every level becomes a one-byte tag plus a four-byte length.
  ASSERT_EQ(out.size(), 65u * 5u);
  // The outermost length names everything inside it: 64 levels of 5 bytes.
  EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.begin() + 5),
            Bytes({0x0a, 0xc0, 0x82, 0x80, 0x00}));
  // The innermost is empty.
  EXPECT_EQ(std::vector<uint8_t>(out.end() - 5, out.end()),
            Bytes({0x0a, 0x80, 0x80, 0x80, 0x00}));

  ASSERT_EQ(Rewrite(NestedTo(1000), &out), RewriteResult::kSuccess);
  EXPECT_EQ(out.size(), 1000u * 5u);
}

// ---------------------------------------------------------------------------
// Malformed input.
// ---------------------------------------------------------------------------

TEST(ProtoRewriterTest, StandardEndGroupTagIsRejected) {
  std::vector<uint8_t> out;
  // 0b = start field 1, 0c = the standard end-group tag for field 1. The
  // proto-group closes with 0x04, so this is not a close.
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x0c}), &out), RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

TEST(ProtoRewriterTest, ProtoGroupEndAtRootIsRejected) {
  std::vector<uint8_t> out;
  EXPECT_EQ(Rewrite(Bytes({0x04}), &out), RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x04, 0x04}), &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

TEST(ProtoRewriterTest, UnclosedMessageIsRejected) {
  std::vector<uint8_t> out;
  EXPECT_EQ(Rewrite(Bytes({0x0b}), &out), RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x13, 0x04}), &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
  // Even a hundred levels deep, one missing close is still malformed.
  std::vector<uint8_t> unclosed = NestedTo(100);
  unclosed.pop_back();
  EXPECT_EQ(Rewrite(unclosed, &out), RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

TEST(ProtoRewriterTest, MalformedTagsAndValuesAreRejected) {
  std::vector<uint8_t> out;
  const std::vector<std::vector<uint8_t>> inputs = {
      // Field id zero.
      Bytes({0x00}),
      // Wire types 6 and 7 do not exist.
      Bytes({0x0e}),
      Bytes({0x0f}),
      // A truncated tag.
      Bytes({0x80}),
      // An overlong tag: eleven bytes cannot be a varint.
      Bytes({0x88, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00}),
      // A truncated varint value.
      Bytes({0x08, 0x80}),
      // A truncated fixed64.
      Bytes({0x11, 1, 2, 3}),
      // A truncated fixed32.
      Bytes({0x25, 1, 2}),
      // A length-delimited field claiming more bytes than are present.
      Bytes({0x1a, 0x10, 'a'}),
      // A truncated length.
      Bytes({0x1a, 0x80}),
      // An overlong length.
      Bytes({0x1a, 0x81, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
             0x00, 'a'}),
      // The same faults inside a nested message.
      Bytes({0x0b, 0x00, 0x04}),
      Bytes({0x0b, 0x08, 0x80}),
      Bytes({0x0b, 0x1a, 0x10, 'a', 0x04}),
  };
  for (const std::vector<uint8_t>& in : inputs) {
    EXPECT_EQ(Rewrite(in, &out), RewriteResult::kMalformedInput);
    EXPECT_TRUE(out.empty());
  }
}

// A tenth varint byte with more than one payload bit overflows the shift and
// reads as a small legal value. Reject it wherever it appears.
TEST(ProtoRewriterTest, OverflowingTenByteVarintsAreRejected) {
  std::vector<uint8_t> out;
  // A tag carrying bit 65: unchecked, it wraps to field 1, wire type 0.
  EXPECT_EQ(Rewrite(Bytes({0x88, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                           0x02, 0x2a}),
                    &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
  // A scalar value that wraps to zero.
  EXPECT_EQ(Rewrite(Bytes({0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                           0x80, 0x02}),
                    &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
  // A length that wraps to zero, followed by what would then be a valid field.
  EXPECT_EQ(Rewrite(Bytes({0x0a, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                           0x80, 0x02, 0x08, 0x01}),
                    &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
  // The same value inside a nested message.
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
                           0x80, 0x80, 0x02, 0x04}),
                    &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());

  // A legal ten-byte value and redundant short encodings still pass.
  const std::vector<uint8_t> legal = Bytes({
      // 2^64 - 1, ten bytes.
      0x08,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0x01,
      // Zero in two bytes.
      0x08,
      0x80,
      0x00,
      // Field 1, wire type 2, with a two-byte tag.
      0x8a,
      0x00,
      0x01,
      'x',
  });
  ASSERT_EQ(Rewrite(legal, &out), RewriteResult::kSuccess);
  EXPECT_EQ(out, legal);
}

// Arbitrary producer bytes: any result is fine, and a rejection leaves
// nothing behind.
TEST(ProtoRewriterTest, ArbitraryBytesNeverLeavePartialOutput) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> in;
  uint32_t state = 0x9e3779b9;
  for (uint32_t i = 0; i < 4096; ++i) {
    state = state * 1664525u + 1013904223u;
    in.push_back(static_cast<uint8_t>(state >> 24));
    const RewriteResult result = Rewrite(in, &out, /*max_output=*/256);
    if (result != RewriteResult::kSuccess)
      EXPECT_TRUE(out.empty()) << i;
    else
      EXPECT_LE(out.size(), 256u) << i;
  }
}

// ---------------------------------------------------------------------------
// Output limits.
// ---------------------------------------------------------------------------

TEST(ProtoRewriterTest, OutputLimitIsExactAndClearsOutput) {
  std::vector<uint8_t> out;
  // One empty nested message costs five output bytes.
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x04}), &out, /*max_output=*/5),
            RewriteResult::kSuccess);
  EXPECT_EQ(out.size(), 5u);
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x04}), &out, /*max_output=*/4),
            RewriteResult::kOutputTooLarge);
  EXPECT_TRUE(out.empty());

  // Four input bytes expand to ten. The limit is about the output.
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x13, 0x04, 0x04}), &out, /*max_output=*/10),
            RewriteResult::kSuccess);
  EXPECT_EQ(out.size(), 10u);
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x13, 0x04, 0x04}), &out, /*max_output=*/9),
            RewriteResult::kOutputTooLarge);
  EXPECT_TRUE(out.empty());

  // A scalar that does not fit after a nested message already went out.
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x04, 0x08, 0x01}), &out, /*max_output=*/6),
            RewriteResult::kOutputTooLarge);
  EXPECT_TRUE(out.empty());
}

// ---------------------------------------------------------------------------
// A real packet.
// ---------------------------------------------------------------------------

TEST(ProtoRewriterTest, GeneratedTracePacketRoundTrips) {
  protozero::HeapBuffered<protos::pbzero::TracePacket> packet;
  packet.Reset(protozero::NestedMessageEncoding::kProtoGroup);
  packet->set_timestamp(1234567);
  auto* test_event = packet->set_for_testing();
  test_event->set_str("hello");
  auto* payload = test_event->set_payload();
  payload->set_single_int(42);
  payload->add_str("nested");
  packet->set_trusted_packet_sequence_id(9);
  const std::vector<uint8_t> private_bytes = packet.SerializeAsArray();

  std::vector<uint8_t> canonical;
  ASSERT_EQ(Rewrite(private_bytes, &canonical), RewriteResult::kSuccess);

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
