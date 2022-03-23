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

#include "src/trace_processor/util/protozero_to_text.h"

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/track_event/chrome_compositor_scheduler_state.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "src/protozero/test/example_proto/test_messages.pbzero.h"
#include "src/trace_processor/importers/track_event.descriptor.h"
#include "src/trace_processor/test_messages.descriptor.h"
#include "src/trace_processor/util/descriptors.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace protozero_to_text {

namespace {

constexpr size_t kChunkSize = 42;

using ::protozero::test::protos::pbzero::EveryField;
using ::protozero::test::protos::pbzero::PackedRepeatedFields;
using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::StartsWith;

TEST(ProtozeroToTextTest, TrackEventBasic) {
  using perfetto::protos::pbzero::TrackEvent;
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  msg->set_track_uuid(4);
  msg->set_timestamp_delta_us(3);
  auto binary_proto = msg.SerializeAsArray();
  EXPECT_EQ(
      "track_uuid: 4\ntimestamp_delta_us: 3",
      DebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));
  EXPECT_EQ(
      "track_uuid: 4 timestamp_delta_us: 3",
      ShortDebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));
}

TEST(ProtozeroToTextTest, TrackEventNestedMsg) {
  using perfetto::protos::pbzero::TrackEvent;
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  msg->set_track_uuid(4);
  auto* state = msg->set_cc_scheduler_state();
  state->set_deadline_us(7);
  auto* machine = state->set_state_machine();
  auto* minor_state = machine->set_minor_state();
  minor_state->set_commit_count(8);
  state->set_observing_begin_frame_source(true);
  msg->set_timestamp_delta_us(3);
  auto binary_proto = msg.SerializeAsArray();

  EXPECT_EQ(
      R"(track_uuid: 4
cc_scheduler_state: {
  deadline_us: 7
  state_machine: {
    minor_state: {
      commit_count: 8
    }
  }
  observing_begin_frame_source: true
}
timestamp_delta_us: 3)",
      DebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));

  EXPECT_EQ(
      "track_uuid: 4 cc_scheduler_state: { deadline_us: 7 state_machine: { "
      "minor_state: { commit_count: 8 } } observing_begin_frame_source: true } "
      "timestamp_delta_us: 3",
      ShortDebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));
}

TEST(ProtozeroToTextTest, TrackEventEnumNames) {
  using perfetto::protos::pbzero::TrackEvent;
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  msg->set_type(TrackEvent::TYPE_SLICE_BEGIN);
  auto binary_proto = msg.SerializeAsArray();
  EXPECT_EQ(
      "type: TYPE_SLICE_BEGIN",
      DebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));
  EXPECT_EQ(
      "type: TYPE_SLICE_BEGIN",
      DebugTrackEventProtozeroToText(
          ".perfetto.protos.TrackEvent",
          protozero::ConstBytes{binary_proto.data(), binary_proto.size()}));
}

TEST(ProtozeroToTextTest, CustomDescriptorPoolBasic) {
  using perfetto::protos::pbzero::TrackEvent;
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  msg->set_track_uuid(4);
  msg->set_timestamp_delta_us(3);
  auto binary_proto = msg.SerializeAsArray();
  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTrackEventDescriptor.data(),
                                              kTrackEventDescriptor.size());
  ASSERT_TRUE(status.ok());
  EXPECT_EQ("track_uuid: 4\ntimestamp_delta_us: 3",
            ProtozeroToText(pool, ".perfetto.protos.TrackEvent", binary_proto,
                            kIncludeNewLines));
  EXPECT_EQ("track_uuid: 4 timestamp_delta_us: 3",
            ProtozeroToText(pool, ".perfetto.protos.TrackEvent", binary_proto,
                            kSkipNewLines));
}

TEST(ProtozeroToTextTest, CustomDescriptorPoolNestedMsg) {
  using perfetto::protos::pbzero::TrackEvent;
  protozero::HeapBuffered<TrackEvent> msg{kChunkSize, kChunkSize};
  msg->set_track_uuid(4);
  auto* state = msg->set_cc_scheduler_state();
  state->set_deadline_us(7);
  auto* machine = state->set_state_machine();
  auto* minor_state = machine->set_minor_state();
  minor_state->set_commit_count(8);
  state->set_observing_begin_frame_source(true);
  msg->set_timestamp_delta_us(3);
  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTrackEventDescriptor.data(),
                                              kTrackEventDescriptor.size());
  ASSERT_TRUE(status.ok());

  EXPECT_EQ(
      R"(track_uuid: 4
cc_scheduler_state: {
  deadline_us: 7
  state_machine: {
    minor_state: {
      commit_count: 8
    }
  }
  observing_begin_frame_source: true
}
timestamp_delta_us: 3)",
      ProtozeroToText(pool, ".perfetto.protos.TrackEvent", binary_proto,
                      kIncludeNewLines));

  EXPECT_EQ(
      "track_uuid: 4 cc_scheduler_state: { deadline_us: 7 state_machine: { "
      "minor_state: { commit_count: 8 } } observing_begin_frame_source: true } "
      "timestamp_delta_us: 3",
      ProtozeroToText(pool, ".perfetto.protos.TrackEvent", binary_proto,
                      kSkipNewLines));
}

TEST(ProtozeroToTextTest, ProtozeroEnumToText) {
  using perfetto::protos::pbzero::TrackEvent;
  EXPECT_EQ("TYPE_SLICE_END",
            ProtozeroEnumToText(".perfetto.protos.TrackEvent.Type",
                                TrackEvent::TYPE_SLICE_END));
}

TEST(ProtozeroToTextTest, BytesToHexEncodedString) {
  EXPECT_EQ(BytesToHexEncodedStringForTesting("abc"), R"(\x61\x62\x63)");
}

// Sets up a descriptor pool with all the messages from
// "src/protozero/test/example_proto/test_messages.proto"
class ProtozeroToTextTestMessageTest : public testing::Test {
 protected:
  void SetUp() override {
    auto status = pool_.AddFromFileDescriptorSet(
        kTestMessagesDescriptor.data(), kTestMessagesDescriptor.size());
    ASSERT_TRUE(status.ok());
  }

  DescriptorPool pool_;
};

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntInt32) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_int32(42);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "field_int32: 42");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntSint32) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_sint32(-42);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "field_sint32: -42");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntUint32) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_uint32(3000000000);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "field_uint32: 3000000000");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntInt64) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_int64(3000000000);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "field_int64: 3000000000");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntSint64) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_sint64(-3000000000);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "field_sint64: -3000000000");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntBool) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_bool(true);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "field_bool: true");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntSmallEnum) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_small_enum(protozero::test::protos::pbzero::TO_BE);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "small_enum: TO_BE");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntSignedEnum) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_signed_enum(protozero::test::protos::pbzero::NEGATIVE);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "signed_enum: NEGATIVE");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntBigEnum) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_big_enum(protozero::test::protos::pbzero::END);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "big_enum: END");
}

// TODO(b/224800278): Fix ProtozeroToText() crash and reenable.
TEST_F(ProtozeroToTextTestMessageTest, DISABLED_FieldVarIntEnumUnknown) {
  protozero::HeapBuffered<EveryField> msg;
  msg->AppendVarInt(EveryField::kSmallEnumFieldNumber, 42);
  ASSERT_EQ(EveryField::kSmallEnumFieldNumber, 51);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "51: 42");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntUnknown) {
  protozero::HeapBuffered<EveryField> msg;
  msg->AppendVarInt(/*field_id=*/9999, /*value=*/42);

  // TODO(b/224800278): "protoc --decode" also prints the varint value, which is
  // more useful.
  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "# Ignoring unknown field with id: 9999");
}

// TODO(b/224800278): Fix ProtozeroToText() crash and reenable.
TEST_F(ProtozeroToTextTestMessageTest, DISABLED_FieldVarIntMismatch) {
  protozero::HeapBuffered<EveryField> msg;
  ASSERT_EQ(EveryField::kFieldStringFieldNumber, 500);
  msg->AppendVarInt(EveryField::kFieldStringFieldNumber, 42);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "500: 42");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldVarIntForPacked) {
  // Even though field_int32 has [packed = true], it still accepts a non-packed
  // representation.
  protozero::HeapBuffered<PackedRepeatedFields> msg;
  msg->AppendVarInt(PackedRepeatedFields::kFieldInt32FieldNumber, 42);

  EXPECT_EQ(
      ProtozeroToText(pool_, ".protozero.test.protos.PackedRepeatedFields",
                      msg.SerializeAsArray(), kIncludeNewLines),
      "field_int32: 42");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldFixed32Signed) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_sfixed32(-42);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "field_sfixed32: -42");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldFixed32Unsigned) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_fixed32(3000000000);

  // TODO(b/224800278): This is a bug. fixed32 is supposed to be unsigned, but
  // the code prints it as signed.
  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "field_fixed32: -1294967296");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldFixed32Float) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_float(24.125);

  EXPECT_THAT(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                              msg.SerializeAsArray(), kIncludeNewLines),
              StartsWith("field_float: 24.125"));
}

TEST_F(ProtozeroToTextTestMessageTest, FieldFixed32Unknown) {
  protozero::HeapBuffered<EveryField> msg;
  msg->AppendFixed<uint32_t>(/*field_id=*/9999, /*value=*/0x1);

  // TODO(b/224800278): "protoc --decode" also prints the 32-bit value, which is
  // more useful.
  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "# Ignoring unknown field with id: 9999");
}

// TODO(b/224800278): Fix ProtozeroToText() crash and reenable.
TEST_F(ProtozeroToTextTestMessageTest, DISABLED_FieldFixed32Mismatch) {
  protozero::HeapBuffered<EveryField> msg;
  ASSERT_EQ(EveryField::kFieldStringFieldNumber, 500);
  msg->AppendFixed<uint32_t>(EveryField::kFieldStringFieldNumber, 0x1);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "500: 0x00000001");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldFixed64Signed) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_sfixed64(-42);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "field_sfixed64: -42");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldFixed64Unsigned) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_fixed64(3000000000);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "field_fixed64: 3000000000");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldFixed64Double) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_double(24.125);

  EXPECT_THAT(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                              msg.SerializeAsArray(), kIncludeNewLines),
              StartsWith("field_double: 24.125"));
}

TEST_F(ProtozeroToTextTestMessageTest, FieldFixed64Unknown) {
  protozero::HeapBuffered<EveryField> msg;
  msg->AppendFixed<uint64_t>(/*field_id=*/9999, /*value=*/0x1);

  // TODO(b/224800278): "protoc --decode" also prints the 64-bit value, which is
  // more useful.
  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "# Ignoring unknown field with id: 9999");
}

// TODO(b/224800278): Fix ProtozeroToText() crash and reenable.
TEST_F(ProtozeroToTextTestMessageTest, DISABLED_FieldFixed64Mismatch) {
  protozero::HeapBuffered<EveryField> msg;
  ASSERT_EQ(EveryField::kFieldStringFieldNumber, 500);
  msg->AppendFixed<uint64_t>(EveryField::kFieldStringFieldNumber, 0x1);

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "500: 0x0000000000000001");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldLengthLimitedString) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_string("Hello");

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            R"(field_string: "Hello")");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldLengthLimitedBytes) {
  protozero::HeapBuffered<EveryField> msg;
  msg->set_field_bytes("Hello");

  // TODO(b/224800278): "protoc --decode" always tries to print the value as
  // ASCII.
  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            R"(field_bytes: "\x48\x65\x6c\x6c\x6f")");
}

TEST_F(ProtozeroToTextTestMessageTest, FieldLengthLimitedUnknown) {
  protozero::HeapBuffered<EveryField> msg;
  msg->AppendString(9999, "Hello");

  // TODO(b/224800278): "protoc --decode" also prints the string value, which is
  // more useful.
  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            "# Ignoring unknown field with id: 9999");
}

// TODO(b/224800278): Fix ProtozeroToText() crash and reenable.
TEST_F(ProtozeroToTextTestMessageTest, DISABLED_FieldLengthLimitedMismatch) {
  protozero::HeapBuffered<EveryField> msg;
  ASSERT_EQ(EveryField::kFieldBoolFieldNumber, 13);
  msg->AppendString(EveryField::kFieldBoolFieldNumber, "Hello");

  EXPECT_EQ(ProtozeroToText(pool_, ".protozero.test.protos.EveryField",
                            msg.SerializeAsArray(), kIncludeNewLines),
            R"(13: "Hello")");
}

}  // namespace
}  // namespace protozero_to_text
}  // namespace trace_processor
}  // namespace perfetto
