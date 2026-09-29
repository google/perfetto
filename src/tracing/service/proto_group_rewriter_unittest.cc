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

#include "src/tracing/service/proto_group_rewriter.h"

#include <stdint.h>

#include <algorithm>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "perfetto/ext/tracing/core/slice.h"
#include "perfetto/protozero/root_message.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "src/tracing/service/proto_group_rewriter_reference_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.gen.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::tracing_v2 {
namespace {

std::vector<uint8_t> Bytes(std::initializer_list<int> values) {
  std::vector<uint8_t> out;
  for (int value : values)
    out.push_back(static_cast<uint8_t>(value));
  return out;
}

// |depth| empty groups of field 1 inside each other.
std::vector<uint8_t> NestedGroups(size_t depth) {
  std::vector<uint8_t> in(depth, 0x0b);
  in.insert(in.end(), depth, 0x04);
  return in;
}

// The expected output for NestedGroups(depth), built from the inside out with
// the v1 length rule.
std::vector<uint8_t> ExpectedNestedMessages(size_t depth) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < depth; ++i) {
    std::vector<uint8_t> level = {0x0a};
    if (out.size() <= 127) {
      level.push_back(static_cast<uint8_t>(out.size()));
    } else {
      uint8_t length[4];
      protozero::proto_utils::WriteRedundantVarInt(
          static_cast<uint32_t>(out.size()), length);
      level.insert(level.end(), length, length + 4);
    }
    level.insert(level.end(), out.begin(), out.end());
    out = std::move(level);
  }
  return out;
}

// How the test splits the input into slices, as TBv2 does for a packet that
// spans fragments.
enum class Split {
  kWhole,              // One slice.
  kEveryByte,          // One slice for each byte.
  kThirdsWithEmpties,  // Three slices, with empty slices around them.
};

// Runs each case on each kind of split.
class ProtoGroupRewriterTest : public testing::TestWithParam<Split> {
 protected:
  // Rewrites |in|. |*out| receives the output on kRewritten, and the input on
  // kUnchanged. It is cleared on a rejection. Only kRewritten may set the
  // output slice.
  RewriteResult Rewrite(const std::vector<uint8_t>& in,
                        std::vector<uint8_t>* out) {
    std::vector<Slice> slices = SplitInput(in, GetParam());
    std::optional<Slice> output;
    const RewriteResult result = rewriter_.Rewrite(slices, &output);
    out->clear();
    EXPECT_EQ(output.has_value(), result == RewriteResult::kRewritten);
    if (result == RewriteResult::kRewritten) {
      const auto* begin = static_cast<const uint8_t*>(output->start);
      out->assign(begin, begin + output->size);
    } else if (result == RewriteResult::kUnchanged) {
      *out = in;
    }
    return result;
  }

  static std::vector<Slice> SplitInput(const std::vector<uint8_t>& in,
                                       Split split) {
    std::vector<Slice> slices;
    switch (split) {
      case Split::kWhole:
        if (!in.empty())
          slices.emplace_back(in.data(), in.size());
        break;
      case Split::kEveryByte:
        for (size_t i = 0; i < in.size(); ++i)
          slices.emplace_back(&in[i], 1);
        break;
      case Split::kThirdsWithEmpties: {
        const size_t a = in.size() / 3;
        const size_t b = 2 * in.size() / 3;
        slices.emplace_back(in.data(), 0);
        slices.emplace_back(in.data(), a);
        slices.emplace_back(in.data() + a, 0);
        slices.emplace_back(in.data() + a, b - a);
        slices.emplace_back(in.data() + b, in.size() - b);
        slices.emplace_back(in.data() + in.size(), 0);
        break;
      }
    }
    return slices;
  }

  ProtoGroupRewriter rewriter_;
};

INSTANTIATE_TEST_SUITE_P(
    All,
    ProtoGroupRewriterTest,
    testing::Values(Split::kWhole,
                    Split::kEveryByte,
                    Split::kThirdsWithEmpties),
    [](const testing::TestParamInfo<ProtoGroupRewriterTest::ParamType>& info) {
      switch (info.param) {
        case Split::kWhole:
          return std::string("Whole");
        case Split::kEveryByte:
          return std::string("EveryByte");
        case Split::kThirdsWithEmpties:
          return std::string("ThirdsWithEmpties");
      }
      return std::string();
    });

// ---------------------------------------------------------------------------
// The two worked examples from the encoding contract.
// ---------------------------------------------------------------------------

TEST_P(ProtoGroupRewriterTest, EmptyGroup) {
  // 0b = start field 1, 04 = end field 1.
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(Bytes({0x0b, 0x04}), &out), RewriteResult::kRewritten);
  // 0a = length-delimited field 1, then a one-byte zero length.
  EXPECT_EQ(out, Bytes({0x0a, 0x00}));
}

TEST_P(ProtoGroupRewriterTest, GroupInsideGroup) {
  // 0b = start field 1, 13 = start field 2, then two end bytes.
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(Bytes({0x0b, 0x13, 0x04, 0x04}), &out),
            RewriteResult::kRewritten);
  // Field 1 holds two bytes: field 2's tag and its zero length.
  EXPECT_EQ(out, Bytes({0x0a, 0x02, 0x12, 0x00}));
}

// ---------------------------------------------------------------------------
// Ordinary fields pass through untouched.
// ---------------------------------------------------------------------------

TEST_P(ProtoGroupRewriterTest, EmptyInput) {
  std::vector<uint8_t> out = Bytes({0xff});
  EXPECT_EQ(Rewrite({}, &out), RewriteResult::kUnchanged);
  EXPECT_TRUE(out.empty());
}

TEST_P(ProtoGroupRewriterTest, ScalarFieldsAreCopiedVerbatim) {
  const std::vector<uint8_t> in = Bytes({
      0x08, 0x2a,                             // field 1 varint 42
      0x11, 1,    2,   3,   4,   5, 6, 7, 8,  // field 2 fixed64
      0x1a, 0x03, 'a', 'b', 'c',              // field 3 bytes "abc"
      0x25, 9,    10,  11,  12,               // field 4 fixed32
  });
  // Without groups, the input is already protobuf: zero copy.
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(in, &out), RewriteResult::kUnchanged);
  EXPECT_EQ(out, in);
}

TEST_P(ProtoGroupRewriterTest, ScalarFieldsInsideAndAroundGroups) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(Bytes({
                        0x08, 0x01,  // field 1 varint 1
                        0x13,        // start field 2
                        0x18, 0x02,  // field 3 varint 2 (nested)
                        0x04,        // end field 2
                        0x20, 0x03,  // field 4 varint 3
                    }),
                    &out),
            RewriteResult::kRewritten);
  EXPECT_EQ(out, Bytes({
                     0x08, 0x01,  // field 1
                     0x12, 0x02,  // field 2, len 2
                     0x18, 0x02,  // field 3
                     0x20, 0x03,  // field 4
                 }));
}

// The end byte is only special at a field boundary. A payload byte that
// happens to be 0x04 is data.
TEST_P(ProtoGroupRewriterTest, ProtoGroupEndByteInsideAPayloadStaysOpaque) {
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(Bytes({
                        0x0b,                          // start field 1
                        0x12, 0x03, 0x04, 0x04, 0x04,  // field 2 bytes 04 04 04
                        0x04,                          // end field 1
                    }),
                    &out),
            RewriteResult::kRewritten);
  EXPECT_EQ(out, Bytes({
                     0x0a, 0x05,                    // field 1, len 5
                     0x12, 0x03, 0x04, 0x04, 0x04,  // field 2, verbatim
                 }));
}

TEST_P(ProtoGroupRewriterTest, FieldIdBoundaries) {
  std::vector<uint8_t> out;
  // Field 15 is the largest id whose tag fits in one byte: (15 << 3) | 3.
  ASSERT_EQ(Rewrite(Bytes({0x7b, 0x04}), &out), RewriteResult::kRewritten);
  EXPECT_EQ(out, Bytes({0x7a, 0x00}));

  // Field 16 needs two tag bytes: (16 << 3) | 3 = 0x83 -> 83 01.
  ASSERT_EQ(Rewrite(Bytes({0x83, 0x01, 0x04}), &out),
            RewriteResult::kRewritten);
  EXPECT_EQ(out, Bytes({0x82, 0x01, 0x00}));

  // Field id 1000: tag = (1000 << 3) | 3 = 8003, encoded as c3 3e.
  ASSERT_EQ(Rewrite(Bytes({0xc3, 0x3e, 0x04}), &out),
            RewriteResult::kRewritten);
  EXPECT_EQ(out, Bytes({0xc2, 0x3e, 0x00}));

  // The largest legal id, 2^29 - 1, as a varint field: tag 0xfffffff8.
  const std::vector<uint8_t> max_id_field =
      Bytes({0xf8, 0xff, 0xff, 0xff, 0x0f, 0x2a});
  ASSERT_EQ(Rewrite(max_id_field, &out), RewriteResult::kUnchanged);
  // And as a group: the rewritten tag is 0xfffffffa.
  ASSERT_EQ(Rewrite(Bytes({0xfb, 0xff, 0xff, 0xff, 0x0f, 0x04}), &out),
            RewriteResult::kRewritten);
  EXPECT_EQ(out, Bytes({0xfa, 0xff, 0xff, 0xff, 0x0f, 0x00}));

  // One past it, id 2^29 with wire type 0: tag 2^32 -> 80 80 80 80 10.
  EXPECT_EQ(Rewrite(Bytes({0x80, 0x80, 0x80, 0x80, 0x10, 0x2a}), &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

// A tag keeps its input bytes, even in a longer encoding than needed. Only
// bit 0 of its first byte changes.
TEST_P(ProtoGroupRewriterTest, NonMinimalTagKeepsItsBytes) {
  std::vector<uint8_t> out;
  // Field 1, wire type 3, in three bytes instead of one: 8b 80 00.
  ASSERT_EQ(Rewrite(Bytes({0x8b, 0x80, 0x00, 0x04}), &out),
            RewriteResult::kRewritten);
  EXPECT_EQ(out, Bytes({0x8a, 0x80, 0x00, 0x00}));
}

TEST_P(ProtoGroupRewriterTest, VarintValueBoundaries) {
  std::vector<uint8_t> out;
  const std::vector<uint8_t> in = Bytes({
      0x08, 0x7f,                                // one byte: 127
      0x08, 0x80, 0x01,                          // two bytes: 128
      0x08, 0xff, 0xff, 0xff, 0xff, 0x0f,        // five bytes: 2^32 - 1
      0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // ten bytes: 2^64 - 1
      0xff, 0xff, 0xff, 0x01,
  });
  ASSERT_EQ(Rewrite(in, &out), RewriteResult::kUnchanged);

  // Eleven bytes is more than a 64-bit varint can take.
  EXPECT_EQ(Rewrite(Bytes({0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                           0xff, 0xff, 0x01}),
                    &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

TEST_P(ProtoGroupRewriterTest, SiblingsAndDeepNestingRoundTrip) {
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
            RewriteResult::kRewritten);
  EXPECT_EQ(out, Bytes({
                     0x0a, 0x00,  // empty field 1
                     0x0a, 0x00,  // empty field 1
                     0x12, 0x04,  // field 2, len 4
                     0x0a, 0x02,  // field 1, len 2
                     0x08, 0x07,  // field 1 varint 7
                 }));
}

// kMaxNestingDepth open groups is the limit. The inner levels get one-byte
// lengths. The outer levels, above 127 bytes, get 4-byte lengths.
// It also makes the output buffer grow: 128 open slots need 640 bytes, more
// than the first allocation of 336 bytes for 256 input bytes.
TEST_P(ProtoGroupRewriterTest, NestingUpToTheLimit) {
  constexpr size_t kDepth = ProtoGroupRewriter::kMaxNestingDepth;
  std::vector<uint8_t> out;
  ASSERT_EQ(Rewrite(NestedGroups(kDepth), &out), RewriteResult::kRewritten);
  EXPECT_EQ(out, ExpectedNestedMessages(kDepth));
}

// One more open group is malformed, even if the packet closes all its groups.
TEST_P(ProtoGroupRewriterTest, NestingBeyondTheLimitIsRejected) {
  constexpr size_t kDepth = ProtoGroupRewriter::kMaxNestingDepth;
  std::vector<uint8_t> out;
  EXPECT_EQ(Rewrite(NestedGroups(kDepth + 1), &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());

  // Start tags only. Without the limit, each tag adds a nesting stack entry.
  EXPECT_EQ(Rewrite(std::vector<uint8_t>(4096, 0x0b), &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

// The v1 rule: at most 127 content bytes take a one-byte length. One more
// takes a 4-byte redundant varint.
TEST_P(ProtoGroupRewriterTest, LengthFieldFollowsV1Rule) {
  for (size_t content_size : {size_t{127}, size_t{128}, size_t{300}}) {
    SCOPED_TRACE(content_size);
    // Field 1 { field 2: bytes }, where field 2 takes |content_size| bytes.
    const size_t payload_size = content_size - 3;  // tag + 2-byte length
    std::vector<uint8_t> in = {0x0b, 0x12,
                               static_cast<uint8_t>(0x80 | payload_size),
                               static_cast<uint8_t>(payload_size >> 7)};
    in.insert(in.end(), payload_size, 'x');
    in.push_back(0x04);

    std::vector<uint8_t> out;
    ASSERT_EQ(Rewrite(in, &out), RewriteResult::kRewritten);
    std::vector<uint8_t> expected = {0x0a};
    if (content_size <= 127) {
      expected.push_back(static_cast<uint8_t>(content_size));
    } else {
      uint8_t length[4];
      protozero::proto_utils::WriteRedundantVarInt(
          static_cast<uint32_t>(content_size), length);
      expected.insert(expected.end(), length, length + 4);
    }
    expected.insert(expected.end(), in.begin() + 1, in.end() - 1);
    EXPECT_EQ(out, expected);
  }
}

// ---------------------------------------------------------------------------
// Malformed input.
// ---------------------------------------------------------------------------

TEST_P(ProtoGroupRewriterTest, StandardEndGroupTagIsRejected) {
  std::vector<uint8_t> out;
  // 0b = start field 1, 0c = the standard end-group tag for field 1. The
  // proto group end byte is 0x04, so this is not a close.
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x0c}), &out), RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

TEST_P(ProtoGroupRewriterTest, ProtoGroupEndAtRootIsRejected) {
  std::vector<uint8_t> out;
  EXPECT_EQ(Rewrite(Bytes({0x04}), &out), RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x04, 0x04}), &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

TEST_P(ProtoGroupRewriterTest, UnclosedGroupIsRejected) {
  std::vector<uint8_t> out;
  EXPECT_EQ(Rewrite(Bytes({0x0b}), &out), RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(Rewrite(Bytes({0x0b, 0x13, 0x04}), &out),
            RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
  // Even a hundred levels deep, one missing close is still malformed.
  std::vector<uint8_t> unclosed = NestedGroups(100);
  unclosed.pop_back();
  EXPECT_EQ(Rewrite(unclosed, &out), RewriteResult::kMalformedInput);
  EXPECT_TRUE(out.empty());
}

TEST_P(ProtoGroupRewriterTest, MalformedTagsAndValuesAreRejected) {
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
      // The same faults inside a group.
      Bytes({0x0b, 0x00, 0x04}),
      Bytes({0x0b, 0x08, 0x80}),
      Bytes({0x0b, 0x1a, 0x10, 'a', 0x04}),
  };
  for (const std::vector<uint8_t>& in : inputs) {
    EXPECT_EQ(Rewrite(in, &out), RewriteResult::kMalformedInput);
    EXPECT_TRUE(out.empty());
  }
}

// Reject invalid tags and varints even when more fields follow.
// Groups without end bytes must also be rejected.
TEST_P(ProtoGroupRewriterTest, MalformedInputWithTrailingFieldsIsRejected) {
  std::vector<uint8_t> padding;
  for (int i = 0; i < 20; ++i)
    padding.insert(padding.end(), {0x08, 0x01});  // field 1 varint 1
  const std::vector<std::vector<uint8_t>> faults = {
      Bytes({0x00}),        // field id zero
      Bytes({0x0e}),        // wire type 6
      Bytes({0x0c}),        // standard end-group tag
      Bytes({0x04}),        // end byte at the root
      Bytes({0x0b, 0x13}),  // two groups that never close
      // An overlong tag and an overlong value.
      Bytes({0x88, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00}),
      Bytes({0x08, 0x81, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
             0x00}),
  };
  for (const std::vector<uint8_t>& fault : faults) {
    std::vector<uint8_t> in = fault;
    in.insert(in.end(), padding.begin(), padding.end());
    std::vector<uint8_t> out;
    EXPECT_EQ(Rewrite(in, &out), RewriteResult::kMalformedInput);
    EXPECT_TRUE(out.empty());
  }
}

// Arbitrary producer bytes must not crash the rewriter. A rejection must leave
// the output slice unchanged: Rewrite() checks that.
TEST_P(ProtoGroupRewriterTest, ArbitraryBytesNeverLeavePartialOutput) {
  std::vector<uint8_t> out;
  std::vector<uint8_t> in;
  uint32_t state = 0x9e3779b9;
  for (uint32_t i = 0; i < 4096; ++i) {
    state = state * 1664525u + 1013904223u;
    in.push_back(static_cast<uint8_t>(state >> 24));
    Rewrite(in, &out);
  }
}

TEST_P(ProtoGroupRewriterTest, OrdinaryFieldsAreCopiedUnchanged) {
  const std::vector<std::vector<uint8_t>> fields = {
      Bytes({0x08, 0x01}),                    // varint
      Bytes({0x09, 1, 2, 3, 4, 5, 6, 7, 8}),  // fixed64
      Bytes({0x0a, 0x01, 0x04}),              // length-delimited
      Bytes({0x0d, 1, 2, 3, 4}),              // fixed32
  };
  for (const auto& field : fields) {
    std::vector<uint8_t> in = Bytes({0x08, 0x2a});
    in.insert(in.end(), field.begin(), field.end());
    std::vector<uint8_t> out;
    ASSERT_EQ(Rewrite(in, &out), RewriteResult::kUnchanged);
  }
}

// ---------------------------------------------------------------------------
// A real packet.
// ---------------------------------------------------------------------------

TEST_P(ProtoGroupRewriterTest, GeneratedTracePacketRoundTrips) {
  protozero::ScatteredHeapBuffer buffer;
  protozero::ScatteredStreamWriter writer(&buffer);
  buffer.set_writer(&writer);
  protozero::RootMessage<protos::pbzero::TracePacket> packet;
  packet.Reset(&writer, protozero::Message::Encoding::kProtoGroup);
  packet.set_timestamp(1234567);
  auto* test_event = packet.set_for_testing();
  test_event->set_str("hello");
  auto* payload = test_event->set_payload();
  payload->set_single_int(42);
  payload->add_str("nested");
  packet.set_trusted_packet_sequence_id(9);
  packet.Finalize();
  const std::vector<uint8_t> private_bytes = buffer.StitchSlices();

  std::vector<uint8_t> canonical;
  ASSERT_EQ(Rewrite(private_bytes, &canonical), RewriteResult::kRewritten);

  protos::gen::TracePacket decoded;
  ASSERT_TRUE(decoded.ParseFromArray(canonical.data(), canonical.size()));
  EXPECT_EQ(decoded.timestamp(), 1234567u);
  EXPECT_EQ(decoded.trusted_packet_sequence_id(), 9u);
  ASSERT_TRUE(decoded.has_for_testing());
  EXPECT_EQ(decoded.for_testing().str(), "hello");
  EXPECT_EQ(decoded.for_testing().payload().single_int(), 42);
  ASSERT_EQ(decoded.for_testing().payload().str().size(), 1u);
  EXPECT_EQ(decoded.for_testing().payload().str()[0], "nested");

  // The end bytes are invalid in standard protobuf. Decode only after the
  // rewriter replaces the group framing.
  protos::gen::TracePacket not_decoded;
  EXPECT_FALSE(
      not_decoded.ParseFromArray(private_bytes.data(), private_bytes.size()));
}

// ---------------------------------------------------------------------------
// Slices and reuse.
// ---------------------------------------------------------------------------

// A length-delimited field starts in one slice and ends in the next slice.
TEST(ProtoGroupRewriterSliceTest, LongValueCrossesSliceBoundary) {
  // field 1 { field 2: 100 bytes }, field 3 varint.
  std::vector<uint8_t> in = {0x0b, 0x12, 100};
  in.insert(in.end(), 100, 'x');
  in.insert(in.end(), {0x04, 0x18, 0x07});

  // The first slice contains the field's tag, length, and part of its value.
  std::vector<Slice> slices;
  slices.emplace_back(in.data(), 40);
  slices.emplace_back(in.data() + 40, in.size() - 40);

  ProtoGroupRewriter rewriter;
  std::optional<Slice> output;
  ASSERT_EQ(rewriter.Rewrite(slices, &output), RewriteResult::kRewritten);

  std::vector<uint8_t> expected;
  ASSERT_EQ(ReferenceRewriteForTesting(in, &expected),
            RewriteResult::kRewritten);
  const auto* begin = static_cast<const uint8_t*>(output->start);
  EXPECT_EQ(std::vector<uint8_t>(begin, begin + output->size), expected);
}

// A rejected packet can leave groups open. The next packet must not see them.
TEST(ProtoGroupRewriterSliceTest, ReuseAfterRejectedPacket) {
  ProtoGroupRewriter rewriter;
  const std::vector<uint8_t> unclosed = Bytes({0x0b, 0x13, 0x0b});
  const std::vector<uint8_t> valid = Bytes({0x0b, 0x08, 0x01, 0x04});
  std::optional<Slice> output;

  std::vector<Slice> slices;
  slices.emplace_back(unclosed.data(), unclosed.size());
  ASSERT_EQ(rewriter.Rewrite(slices, &output), RewriteResult::kMalformedInput);
  ASSERT_FALSE(output.has_value());

  slices.clear();
  slices.emplace_back(valid.data(), valid.size());
  ASSERT_EQ(rewriter.Rewrite(slices, &output), RewriteResult::kRewritten);
  const auto* begin = static_cast<const uint8_t*>(output->start);
  EXPECT_EQ(std::vector<uint8_t>(begin, begin + output->size),
            Bytes({0x0a, 0x02, 0x08, 0x01}));
}

// ---------------------------------------------------------------------------
// Random packets against the reference rewriter.
// ---------------------------------------------------------------------------

// Appends one random field to |out|. Groups recurse |depth| levels.
// The mix covers 1- to 5-byte tags, every wire type, values that contain
// 0x04, and groups above 127 bytes.
void AppendRandomField(std::minstd_rand* rng,
                       int depth,
                       std::vector<uint8_t>* out) {
  auto rand = [rng](uint32_t n) { return static_cast<uint32_t>((*rng)() % n); };
  auto put_varint = [out](uint64_t value) {
    uint8_t buf[10];
    out->insert(out->end(), buf,
                protozero::proto_utils::WriteVarInt(value, buf));
  };
  auto put_bytes = [&](size_t n) {
    for (size_t i = 0; i < n; ++i)
      out->push_back(rand(4) == 0 ? 0x04 : static_cast<uint8_t>(rand(256)));
  };

  static constexpr uint32_t kFieldIds[] = {1, 2, 15, 16, 1000, (1u << 29) - 1};
  const uint64_t field_id = kFieldIds[rand(6)];
  const uint32_t kind = depth > 0 ? rand(5) : rand(4);
  switch (kind) {
    case 0:  // varint
      put_varint(field_id << 3);
      put_varint((static_cast<uint64_t>((*rng)()) << 32) >> rand(64));
      break;
    case 1:  // fixed64
      put_varint((field_id << 3) | 1);
      put_bytes(8);
      break;
    case 2:  // fixed32
      put_varint((field_id << 3) | 5);
      put_bytes(4);
      break;
    case 3: {  // length-delimited, sometimes long
      const size_t size = rand(4) == 0 ? rand(300) : rand(8);
      put_varint((field_id << 3) | 2);
      put_varint(size);
      put_bytes(size);
      break;
    }
    case 4: {  // group
      put_varint((field_id << 3) | 3);
      const uint32_t fields = rand(6);
      for (uint32_t i = 0; i < fields; ++i)
        AppendRandomField(rng, depth - 1, out);
      out->push_back(0x04);
      break;
    }
  }
}

// Random packets, a quarter of them damaged by one random byte, rewritten from
// random splits. Every result and every output must match the reference.
TEST(ProtoGroupRewriterRandomTest, MatchesReferenceOnRandomPacketsAndSplits) {
  std::minstd_rand rng(1234);
  auto rand = [&rng](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
  ProtoGroupRewriter rewriter;
  size_t successes = 0;
  size_t rejections = 0;

  for (uint32_t i = 0; i < 3000; ++i) {
    // The packet.
    std::vector<uint8_t> in;
    const uint32_t fields = rand(8);
    for (uint32_t f = 0; f < fields; ++f)
      AppendRandomField(&rng, 4, &in);
    if (!in.empty() && rand(4) == 0)
      in[rand(static_cast<uint32_t>(in.size()))] =
          static_cast<uint8_t>(rand(256));

    // Slices of 0 to 64 bytes. Empty ones too.
    std::vector<Slice> slices;
    for (size_t begin = 0; begin < in.size();) {
      const size_t n = std::min<size_t>(rand(65), in.size() - begin);
      slices.emplace_back(in.data() + begin, n);
      begin += n;
    }

    std::vector<uint8_t> expected;
    const RewriteResult expected_result =
        ReferenceRewriteForTesting(in, &expected);
    std::optional<Slice> output;
    ASSERT_EQ(rewriter.Rewrite(slices, &output), expected_result) << i;
    ASSERT_EQ(output.has_value(), expected_result == RewriteResult::kRewritten)
        << i;
    if (output) {
      const auto* begin = static_cast<const uint8_t*>(output->start);
      ASSERT_EQ(std::vector<uint8_t>(begin, begin + output->size), expected)
          << i;
      ++successes;
    } else if (expected_result == RewriteResult::kMalformedInput) {
      ++rejections;
    }
  }

  // The mix must exercise both outcomes.
  EXPECT_GT(successes, 1000u);
  EXPECT_GT(rejections, 100u);
}

}  // namespace
}  // namespace perfetto::tracing_v2
