/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/util/proto_to_args_parser.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ios>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/base/test//status_matchers.h"
#include "src/protozero/test/example_proto/test_messages.pbzero.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_builder.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/test_messages.descriptor.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/interned_message_view.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"

namespace perfetto::trace_processor::util {
namespace {

constexpr size_t kChunkSize = 42;

protozero::ConstChars ToChars(const char* str) {
  return protozero::ConstChars{str, strlen(str)};
}

class ProtoToArgsParserTest : public ::testing::Test,
                              public ProtoToArgsParser::Delegate {
 protected:
  ProtoToArgsParserTest() {}

  const std::vector<std::string>& args() const { return args_; }

  void AddInternedSourceLocation(uint64_t iid, TraceBlobView data) {
    interned_source_locations_[iid] = std::unique_ptr<InternedMessageView>(
        new InternedMessageView(std::move(data)));
  }

  template <typename T, typename... Ts>
  void CreatedPackedVarint(protozero::PackedVarInt& var, T p, Ts... ps) {
    var.Reset();
    std::array<T, sizeof...(ps) + 1> list = {p, ps...};
    for (T v : list) {
      var.Append(v);
    }
  }

 private:
  using Key = ProtoToArgsParser::Key;

  void AddInteger(const Key& key, int64_t value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddUnsignedInteger(const Key& key, uint64_t value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddString(const Key& key, const protozero::ConstChars& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value.ToStdString();
    args_.push_back(ss.str());
  }

  void AddString(const Key& key, const std::string& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddBytes(const Key& key, const protozero::ConstBytes& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " <bytes size=" << value.size
       << ">";
    args_.push_back(ss.str());
  }

  void AddDouble(const Key& key, double value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddPointer(const Key& key, uint64_t value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << std::hex << value
       << std::dec;
    args_.push_back(ss.str());
  }

  void AddBoolean(const Key& key, bool value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << (value ? "true" : "false");
    args_.push_back(ss.str());
  }

  bool AddJson(const Key& key, const protozero::ConstChars& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << std::hex
       << value.ToStdString() << std::dec;
    args_.push_back(ss.str());
    return true;
  }

  void AddNull(const Key& key) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " [NULL]";
    args_.push_back(ss.str());
  }

  size_t GetArrayEntryIndex(const std::string&) final { return 0; }

  size_t IncrementArrayEntryIndex(const std::string&) final { return 0; }

  InternedMessageView* GetInternedMessageView(uint32_t field_id,
                                              uint64_t iid) override {
    if (field_id != protos::pbzero::InternedData::kSourceLocationsFieldNumber)
      return nullptr;
    return interned_source_locations_.at(iid).get();
  }

  PacketSequenceStateGeneration* seq_state() final { return nullptr; }

  std::vector<std::string> args_;
  std::map<uint64_t, std::unique_ptr<InternedMessageView>>
      interned_source_locations_;
};

TEST_F(ProtoToArgsParserTest, EnsureTestMessageProtoParses) {
  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  EXPECT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();
}

TEST_F(ProtoToArgsParserTest, BasicSingleLayerProto) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<EveryField> msg{kChunkSize, kChunkSize};
  msg->set_field_int32(-1);
  msg->set_field_int64(-333123456789ll);
  msg->set_field_uint32(600);
  msg->set_field_uint64(333123456789ll);
  msg->set_field_sint32(-5);
  msg->set_field_sint64(-9000);
  msg->set_field_fixed32(12345);
  msg->set_field_fixed64(444123450000ll);
  msg->set_field_sfixed32(-69999);
  msg->set_field_sfixed64(-200);
  msg->set_field_double(0.5555);
  msg->set_field_bool(true);
  msg->set_small_enum(SmallEnum::TO_BE);
  msg->set_signed_enum(SignedEnum::NEGATIVE);
  msg->set_big_enum(BigEnum::BEGIN);
  msg->set_nested_enum(EveryField::PONG);
  msg->set_field_float(3.14f);
  msg->set_field_string("FizzBuzz");
  msg->add_repeated_int32(1);
  msg->add_repeated_int32(-1);
  msg->add_repeated_int32(100);
  msg->add_repeated_int32(2000000);
  msg->set_field_bytes({0, 1, 2});

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.EveryField", nullptr, *this);

  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();

  EXPECT_THAT(
      args(),
      testing::ElementsAre(
          "field_int32 field_int32 -1", "field_int64 field_int64 -333123456789",
          "field_uint32 field_uint32 600",
          "field_uint64 field_uint64 333123456789",
          "field_sint32 field_sint32 -5", "field_sint64 field_sint64 -9000",
          "field_fixed32 field_fixed32 12345",
          "field_fixed64 field_fixed64 444123450000",
          "field_sfixed32 field_sfixed32 -69999",
          "field_sfixed64 field_sfixed64 -200",
          "field_double field_double 0.5555", "field_bool field_bool true",
          "small_enum small_enum TO_BE", "signed_enum signed_enum NEGATIVE",
          "big_enum big_enum BEGIN", "nested_enum nested_enum PONG",
          "field_float field_float 3.14", "field_string field_string FizzBuzz",
          "repeated_int32 repeated_int32[0] 1",
          "repeated_int32 repeated_int32[1] -1",
          "repeated_int32 repeated_int32[2] 100",
          "repeated_int32 repeated_int32[3] 2000000",
          "field_bytes field_bytes <bytes size=3>"));
}

TEST_F(ProtoToArgsParserTest, PackedEncodingWithoutDescriptorPackedFlag) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<protozero::Message> raw{kChunkSize, kChunkSize};
  protozero::PackedVarInt packed;
  packed.Append(int32_t{10});
  packed.Append(int32_t{20});
  raw->AppendBytes(EveryField::kRepeatedInt32FieldNumber, packed.data(),
                   packed.size());
  auto binary_proto = raw.SerializeAsArray();

  DescriptorPool pool;
  ASSERT_OK(pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                          kTestMessagesDescriptor.size()));

  ProtoToArgsParser parser(pool);
  ASSERT_OK(parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.EveryField", nullptr, *this));
  EXPECT_THAT(args(),
              testing::ElementsAre("repeated_int32 repeated_int32[0] 10",
                                   "repeated_int32 repeated_int32[1] 20"));
}

TEST_F(ProtoToArgsParserTest, NestedProto) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested()->set_value_c(3);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(args(), testing::ElementsAre(
                          "super_nested.value_c super_nested.value_c 3"));
}

TEST_F(ProtoToArgsParserTest, CamelCaseFieldsProto) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<CamelCaseFields> msg{kChunkSize, kChunkSize};
  msg->set_barbaz(true);
  msg->set_moomoo(true);
  msg->set___bigbang(true);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.CamelCaseFields", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(args(),
              testing::ElementsAre("barBaz barBaz true", "MooMoo MooMoo true",
                                   "__bigBang __bigBang true"));
}

TEST_F(ProtoToArgsParserTest, NestedProtoParsingOverrideHandled) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested()->set_value_c(3);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  parser.AddParsingOverrideForField(
      "super_nested.value_c",
      [](const protozero::Field& field, ProtoToArgsParser::Delegate& writer) {
        EXPECT_EQ(field.type(), protozero::proto_utils::ProtoWireType::kVarInt);
        std::string key = "super_nested.value_b.replaced";
        writer.AddInteger({key, key}, field.as_int32());
        // We've handled this field by adding the desired args.
        return base::OkStatus();
      });

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(
      args(),
      testing::ElementsAre(
          "super_nested.value_b.replaced super_nested.value_b.replaced 3"));
}

TEST_F(ProtoToArgsParserTest, NestedProtoParsingOverrideSkipped) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested()->set_value_c(3);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  int nested_val = 0;
  parser.AddParsingOverrideForField(
      "super_nested.value_c", [&nested_val](const protozero::Field& field,
                                            ProtoToArgsParser::Delegate&) {
        ++nested_val;
        EXPECT_EQ(1, nested_val);
        EXPECT_EQ(field.type(), protozero::proto_utils::ProtoWireType::kVarInt);
        return std::nullopt;
      });

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(args(), testing::ElementsAre(
                          "super_nested.value_c super_nested.value_c 3"));
}

TEST_F(ProtoToArgsParserTest, LookingUpInternedStateParsingOverride) {
  using namespace protozero::test::protos::pbzero;
  // The test proto, we will use |value_c| as the source_location iid.
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested()->set_value_c(3);
  auto binary_proto = msg.SerializeAsArray();

  // The interned source location.
  protozero::HeapBuffered<protos::pbzero::SourceLocation> src_loc{kChunkSize,
                                                                  kChunkSize};
  const uint64_t kIid = 3;
  src_loc->set_iid(kIid);
  src_loc->set_file_name("test_file_name");
  // We need to update sequence_state to point to it.
  auto binary_data = src_loc.SerializeAsArray();
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[binary_data.size()]);
  for (size_t i = 0; i < binary_data.size(); ++i) {
    buffer.get()[i] = binary_data[i];
  }
  TraceBlob blob =
      TraceBlob::TakeOwnership(std::move(buffer), binary_data.size());
  AddInternedSourceLocation(kIid, TraceBlobView(std::move(blob)));

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  ProtoToArgsParser parser(pool);
  // Now we override the behaviour of |value_c| so we can expand the iid into
  // multiple args rows.
  parser.AddParsingOverrideForField(
      "super_nested.value_c",
      [](const protozero::Field& field,
         ProtoToArgsParser::Delegate& delegate) -> std::optional<base::Status> {
        auto* decoder = delegate.GetInternedMessage(
            protos::pbzero::InternedData::kSourceLocations, field.as_uint64());
        if (!decoder) {
          // Lookup failed fall back on default behaviour.
          return std::nullopt;
        }
        delegate.AddString(ProtoToArgsParser::Key("file_name"),
                           protozero::ConstChars{"file", 4});
        delegate.AddInteger(ProtoToArgsParser::Key("line_number"), 2);
        return base::OkStatus();
      });

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(args(), testing::ElementsAre("file_name file_name file",
                                           "line_number line_number 2"));
}

TEST_F(ProtoToArgsParserTest, OverrideForType) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested()->set_value_c(3);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  ProtoToArgsParser parser(pool);

  parser.AddParsingOverrideForType(
      ".protozero.test.protos.NestedA.NestedB.NestedC",
      [](ProtoToArgsParser::ScopedNestedKeyContext&,
         const protozero::ConstBytes&, Delegate& delegate) {
        delegate.AddInteger(ProtoToArgsParser::Key("arg"), 42);
        return base::OkStatus();
      });

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(args(), testing::ElementsAre("arg arg 42"));
}

TEST_F(ProtoToArgsParserTest, FieldOverrideTakesPrecedence) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested()->set_value_c(3);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  ProtoToArgsParser parser(pool);

  parser.AddParsingOverrideForField(
      "super_nested",
      [](const protozero::Field&, ProtoToArgsParser::Delegate& writer) {
        writer.AddString(ProtoToArgsParser::Key("arg"),
                         ToChars("override-for-field"));
        return base::OkStatus();
      });

  parser.AddParsingOverrideForType(
      ".protozero.test.protos.NestedA.NestedB.NestedC",
      [](ProtoToArgsParser::ScopedNestedKeyContext&,
         const protozero::ConstBytes&, Delegate& delegate) {
        delegate.AddString(ProtoToArgsParser::Key("arg"),
                           ToChars("override-for-type"));
        return base::OkStatus();
      });

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(args(), testing::ElementsAre("arg arg override-for-field"));
}

TEST_F(ProtoToArgsParserTest, EmptyMessage) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested();

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  ProtoToArgsParser parser(pool);
  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this);
  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();
  EXPECT_THAT(args(), testing::ElementsAre("super_nested super_nested [NULL]"));
}

TEST_F(ProtoToArgsParserTest, WidthAndSignednessOfScalars) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<EveryField> msg{kChunkSize, kChunkSize};

  // Set fields to values with the top bit set, and check that the parser
  // retains the full value with the correct sign.
  msg->set_field_int32(-0x80000000ll);
  msg->set_field_sint32(-0x80000000ll);
  msg->set_field_sfixed32(-0x80000000ll);

  msg->set_field_uint32(0x80000000ull);
  msg->set_field_fixed32(0x80000000ull);

  msg->set_field_int64(-0x7FFFFFFFFFFFFFFFll - 1);
  msg->set_field_sint64(-0x7FFFFFFFFFFFFFFFll - 1);
  msg->set_field_sfixed64(-0x7FFFFFFFFFFFFFFFll - 1);

  msg->set_field_uint64(0x8000000000000000ull);
  msg->set_field_fixed64(0x8000000000000000ull);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.EveryField", nullptr, *this);

  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();

  EXPECT_THAT(args(), testing::ElementsAre(
                          "field_int32 field_int32 -2147483648",
                          "field_sint32 field_sint32 -2147483648",
                          "field_sfixed32 field_sfixed32 -2147483648",
                          "field_uint32 field_uint32 2147483648",
                          "field_fixed32 field_fixed32 2147483648",
                          "field_int64 field_int64 -9223372036854775808",
                          "field_sint64 field_sint64 -9223372036854775808",
                          "field_sfixed64 field_sfixed64 -9223372036854775808",
                          "field_uint64 field_uint64 9223372036854775808",
                          "field_fixed64 field_fixed64 9223372036854775808"));
}

TEST_F(ProtoToArgsParserTest, PackedFields) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<PackedRepeatedFields> msg{kChunkSize, kChunkSize};

  protozero::PackedVarInt varint;
  CreatedPackedVarint(varint, 0, std::numeric_limits<int32_t>::min(),
                      std::numeric_limits<int32_t>::max());
  msg->set_field_int32(varint);

  CreatedPackedVarint(varint, 0ll, std::numeric_limits<int64_t>::min(),
                      std::numeric_limits<int64_t>::max());
  msg->set_field_int64(varint);

  CreatedPackedVarint(varint, 0u, std::numeric_limits<uint32_t>::min(),
                      std::numeric_limits<uint32_t>::max());
  msg->set_field_uint32(varint);

  CreatedPackedVarint(varint, 0ull, std::numeric_limits<uint64_t>::min(),
                      std::numeric_limits<uint64_t>::max());
  msg->set_field_uint64(varint);

  CreatedPackedVarint(varint, BigEnum::BEGIN, BigEnum::END);
  msg->set_big_enum(varint);

  protozero::PackedFixedSizeInt<uint32_t> fixed32;
  fixed32.Append(0);
  fixed32.Append(std::numeric_limits<uint32_t>::min());
  fixed32.Append(std::numeric_limits<uint32_t>::max());
  msg->set_field_fixed32(fixed32);

  protozero::PackedFixedSizeInt<int32_t> sfixed32;
  sfixed32.Append(0);
  sfixed32.Append(std::numeric_limits<int32_t>::min());
  sfixed32.Append(std::numeric_limits<int32_t>::max());
  msg->set_field_sfixed32(sfixed32);

  protozero::PackedFixedSizeInt<float> pfloat;
  pfloat.Append(0);
  pfloat.Append(-4839.349f);
  pfloat.Append(std::numeric_limits<float>::min());
  pfloat.Append(std::numeric_limits<float>::max());
  msg->set_field_float(pfloat);

  protozero::PackedFixedSizeInt<uint64_t> fixed64;
  fixed64.Append(0);
  fixed64.Append(std::numeric_limits<uint64_t>::min());
  fixed64.Append(std::numeric_limits<uint64_t>::max());
  msg->set_field_fixed64(fixed64);

  protozero::PackedFixedSizeInt<int64_t> sfixed64;
  sfixed64.Append(0);
  sfixed64.Append(std::numeric_limits<int64_t>::min());
  sfixed64.Append(std::numeric_limits<int64_t>::max());
  msg->set_field_sfixed64(sfixed64);

  protozero::PackedFixedSizeInt<double> pdouble;
  pdouble.Append(0);
  pdouble.Append(-48948908.349);
  pdouble.Append(std::numeric_limits<double>::min());
  pdouble.Append(std::numeric_limits<double>::max());
  msg->set_field_double(pdouble);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.PackedRepeatedFields", nullptr, *this);

  EXPECT_TRUE(status.ok()) << "ParseMessage failed with error: "
                           << status.message();

  EXPECT_THAT(
      args(),
      testing::ElementsAre(
          "field_int32 field_int32[0] 0",
          "field_int32 field_int32[1] -2147483648",
          "field_int32 field_int32[2] 2147483647",
          "field_int64 field_int64[0] 0",
          "field_int64 field_int64[1] -9223372036854775808",
          "field_int64 field_int64[2] 9223372036854775807",
          "field_uint32 field_uint32[0] 0", "field_uint32 field_uint32[1] 0",
          "field_uint32 field_uint32[2] 4294967295",
          "field_uint64 field_uint64[0] 0", "field_uint64 field_uint64[1] 0",
          "field_uint64 field_uint64[2] 18446744073709551615",
          "big_enum big_enum[0] BEGIN", "big_enum big_enum[1] END",
          "field_fixed32 field_fixed32[0] 0",
          "field_fixed32 field_fixed32[1] 0",
          "field_fixed32 field_fixed32[2] 4294967295",
          "field_sfixed32 field_sfixed32[0] 0",
          "field_sfixed32 field_sfixed32[1] -2147483648",
          "field_sfixed32 field_sfixed32[2] 2147483647",
          "field_float field_float[0] 0", "field_float field_float[1] -4839.35",
          "field_float field_float[2] 1.17549e-38",
          "field_float field_float[3] 3.40282e+38",
          "field_fixed64 field_fixed64[0] 0",
          "field_fixed64 field_fixed64[1] 0",
          "field_fixed64 field_fixed64[2] 18446744073709551615",
          "field_sfixed64 field_sfixed64[0] 0",
          "field_sfixed64 field_sfixed64[1] -9223372036854775808",
          "field_sfixed64 field_sfixed64[2] 9223372036854775807",
          "field_double field_double[0] 0",
          "field_double field_double[1] -4.89489e+07",
          "field_double field_double[2] 2.22507e-308",
          "field_double field_double[3] 1.79769e+308"));
}

TEST_F(ProtoToArgsParserTest, AllowedFieldsOnlyTopLevel) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested()->set_value_c(42);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  std::vector<uint32_t> allowed_fields = {
      NestedA::kSuperNestedFieldNumber,
  };
  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", &allowed_fields, *this);

  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();

  EXPECT_THAT(args(), testing::ElementsAre(
                          "super_nested.value_c super_nested.value_c 42"));
}

TEST_F(ProtoToArgsParserTest, AddsDefaultsNested) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<NestedA> msg{kChunkSize, kChunkSize};
  msg->set_super_nested();

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.NestedA", nullptr, *this, nullptr,
      /* add_defaults= */ true);

  EXPECT_TRUE(status.ok())
      << "InternProtoFieldsIntoArgsTable failed with error: "
      << status.message();

  EXPECT_THAT(args(), testing::ElementsAre(
                          "super_nested.value_c super_nested.value_c 0",
                          "repeated_a repeated_a [NULL]"));
}

TEST_F(ProtoToArgsParserTest, AddsDefaults) {
  using namespace protozero::test::protos::pbzero;
  protozero::HeapBuffered<EveryField> msg{kChunkSize, kChunkSize};
  msg->set_field_int32(-1);
  msg->add_repeated_string("test");
  msg->add_repeated_sfixed32(1);
  msg->add_repeated_fixed64(1);
  msg->set_nested_enum(EveryField::PONG);

  auto binary_proto = msg.SerializeAsArray();

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ProtoToArgsParser parser(pool);
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  status = parser.ParseMessage(
      protozero::ConstBytes{binary_proto.data(), binary_proto.size()},
      ".protozero.test.protos.EveryField", nullptr, *this, nullptr, true);

  EXPECT_TRUE(status.ok()) << "AddsDefaults failed with error: "
                           << status.message();

  EXPECT_THAT(
      args(),
      testing::UnorderedElementsAre(
          "field_int32 field_int32 -1",  // exists in message
          "repeated_string repeated_string[0] test",
          "repeated_sfixed32 repeated_sfixed32[0] 1",
          "repeated_fixed64 repeated_fixed64[0] 1",
          "nested_enum nested_enum PONG",
          "field_bytes field_bytes <bytes size=0>",
          "field_string field_string [NULL]",  // null if no string default
          "field_nested field_nested [NULL]",  // no defaults for inner fields
          "field_bool field_bool false",
          "repeated_int32 repeated_int32 [NULL]",  // null for repeated fields
          "field_double field_double 0", "field_float field_float 0",
          "field_sfixed64 field_sfixed64 0", "field_sfixed32 field_sfixed32 0",
          "field_fixed64 field_fixed64 0", "field_sint64 field_sint64 0",
          "big_enum big_enum 0", "field_fixed32 field_fixed32 0",
          "field_sint32 field_sint32 0",
          "signed_enum signed_enum NEUTRAL",  // translates default enum
          "small_enum small_enum NOT_TO_BE",
          "very_negative_enum very_negative_enum DEF",
          "field_uint64 field_uint64 0", "field_uint32 field_uint32 0",
          "field_int64 field_int64 0"));
}

// ===========================================================================
// DebugAnnotation tests
// ===========================================================================

base::Status ParseDebugAnnotation(
    ProtoToArgsParser& parser,
    protozero::HeapBuffered<protos::pbzero::DebugAnnotation>& msg,
    ProtoToArgsParser::Delegate& delegate) {
  std::vector<uint8_t> data = msg.SerializeAsArray();
  return parser.ParseDebugAnnotation(
      protozero::ConstBytes{data.data(), data.size()}, delegate);
}

class DebugAnnotationParserTest : public ::testing::Test,
                                  public ProtoToArgsParser::Delegate {
 protected:
  DebugAnnotationParserTest() {
    context_.storage.reset(new TraceStorage());
    context_.machine_tracker.reset(
        new MachineTracker(&context_, kDefaultMachineId));
  }

  const std::vector<std::string>& args() const { return args_; }

  void InternMessage(uint32_t field_id, TraceBlobView message) {
    state_builder_.InternMessage(field_id, std::move(message));
  }

 private:
  using Key = ProtoToArgsParser::Key;

  void AddInteger(const Key& key, int64_t value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddUnsignedInteger(const Key& key, uint64_t value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddString(const Key& key, const protozero::ConstChars& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value.ToStdString();
    args_.push_back(ss.str());
  }

  void AddString(const Key& key, const std::string& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddDouble(const Key& key, double value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddPointer(const Key& key, uint64_t value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << std::hex << value
       << std::dec;
    args_.push_back(ss.str());
  }

  void AddBoolean(const Key& key, bool value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << (value ? "true" : "false");
    args_.push_back(ss.str());
  }

  bool AddJson(const Key& key, const protozero::ConstChars& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << std::hex
       << value.ToStdString() << std::dec;
    args_.push_back(ss.str());
    return true;
  }

  void AddNull(const Key& key) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " [NULL]";
    args_.push_back(ss.str());
  }

  size_t GetArrayEntryIndex(const std::string& array_key) final {
    return array_indices_[array_key];
  }

  size_t IncrementArrayEntryIndex(const std::string& array_key) final {
    return ++array_indices_[array_key];
  }

  InternedMessageView* GetInternedMessageView(uint32_t field_id,
                                              uint64_t iid) override {
    return state_builder_.current_generation()->GetInternedMessageView(field_id,
                                                                       iid);
  }

  PacketSequenceStateGeneration* seq_state() final {
    return state_builder_.current_generation().get();
  }

  std::vector<std::string> args_;
  std::map<std::string, size_t> array_indices_;

  TraceProcessorContext context_;
  PacketSequenceStateBuilder state_builder_{&context_};
};

// This test checks that in when an array is nested inside a dict which is
// nested inside an array which is nested inside a dict, flat keys and non-flat
// keys are parsed correctly.
TEST_F(DebugAnnotationParserTest, DeeplyNestedDictsAndArrays) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;

  msg->set_name("root");
  auto* dict1 = msg->add_dict_entries();
  dict1->set_name("k1");
  auto* array1 = dict1->add_array_values();
  auto* dict2 = array1->add_dict_entries();
  dict2->set_name("k2");
  auto* array2 = dict2->add_array_values();
  array2->set_int_value(42);

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  EXPECT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  ProtoToArgsParser args_parser(pool);

  status = ParseDebugAnnotation(args_parser, msg, *this);
  EXPECT_TRUE(status.ok()) << "ParseDebugAnnotation failed with error:"
                           << status.message();

  EXPECT_THAT(args(), testing::ElementsAre("root.k1.k2 root.k1[0].k2[0] 42"));
}

// This test checks that array indexes are correctly merged across messages.
TEST_F(DebugAnnotationParserTest, MergeArrays) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg1;
  msg1->set_name("root");
  auto* item1 = msg1->add_array_values();
  item1->set_int_value(1);

  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg2;
  msg2->set_name("root");
  auto* item2 = msg1->add_array_values();
  item2->set_int_value(2);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);

  base::Status status = ParseDebugAnnotation(args_parser, msg1, *this);
  EXPECT_TRUE(status.ok()) << "ParseDebugAnnotation failed with error:"
                           << status.message();

  status = ParseDebugAnnotation(args_parser, msg2, *this);
  EXPECT_TRUE(status.ok()) << "ParseDebugAnnotation failed with error:"
                           << status.message();

  EXPECT_THAT(args(), testing::ElementsAre("root root[0] 1", "root root[1] 2"));
}

// This test checks that nested empty dictionaries / arrays do not cause array
// index to be incremented.
TEST_F(DebugAnnotationParserTest, EmptyArrayIndexIsSkipped) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");

  msg->add_array_values()->set_int_value(1);

  // Empty item.
  msg->add_array_values();

  msg->add_array_values()->set_int_value(3);

  // Empty dict.
  msg->add_array_values()->add_dict_entries()->set_name("key1");

  auto* nested_dict_entry = msg->add_array_values()->add_dict_entries();
  nested_dict_entry->set_name("key2");
  nested_dict_entry->set_string_value("value");

  msg->add_array_values()->set_int_value(5);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);

  base::Status status = ParseDebugAnnotation(args_parser, msg, *this);
  EXPECT_TRUE(status.ok()) << "ParseDebugAnnotation failed with error:"
                           << status.message();

  EXPECT_THAT(args(), testing::ElementsAre("root root[0] 1", "root root[1] 3",
                                           "root.key2 root[3].key2 value",
                                           "root root[4] 5"));
}

TEST_F(DebugAnnotationParserTest, NestedArrays) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");
  auto* item1 = msg->add_array_values();
  item1->add_array_values()->set_int_value(1);
  item1->add_array_values()->set_int_value(2);
  auto* item2 = msg->add_array_values();
  item2->add_array_values()->set_int_value(3);
  item2->add_array_values()->set_int_value(4);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);

  base::Status status = ParseDebugAnnotation(args_parser, msg, *this);
  EXPECT_TRUE(status.ok()) << "ParseDebugAnnotation failed with error:"
                           << status.message();

  EXPECT_THAT(args(),
              testing::ElementsAre("root root[0][0] 1", "root root[0][1] 2",
                                   "root root[1][0] 3", "root root[1][1] 4"));
}

TEST_F(DebugAnnotationParserTest, TypedMessageInsideUntyped) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");

  protozero::HeapBuffered<protozero::test::protos::pbzero::EveryField> message;
  message->set_field_string("value");

  msg->set_proto_type_name(message->GetName());
  msg->set_proto_value(message.SerializeAsString());

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  EXPECT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  ProtoToArgsParser args_parser(pool);

  status = ParseDebugAnnotation(args_parser, msg, *this);
  EXPECT_TRUE(status.ok()) << "ParseDebugAnnotation failed with error:"
                           << status.message();

  EXPECT_THAT(args(), testing::ElementsAre(
                          "root.field_string root.field_string value"));
}

// Verifies that a DebugAnnotation whose proto_value is itself a
// DebugAnnotation (an arbitrarily deep cycle) is parsed iteratively without
// consuming C++ stack per level.
TEST_F(DebugAnnotationParserTest, DeeplyNestedProtoValueCycle) {
  // Build N nested DebugAnnotation messages, each carrying the next as a
  // proto_value with proto_type_name = ".perfetto.protos.DebugAnnotation".
  // The innermost annotation has a leaf int_value so we can verify the cycle
  // was traversed end-to-end.
  constexpr int kDepth = 1000;
  std::vector<std::vector<uint8_t>> serialized;
  serialized.reserve(kDepth);

  // Innermost annotation: just a name + int value.
  {
    protozero::HeapBuffered<protos::pbzero::DebugAnnotation> inner;
    inner->set_name("leaf");
    inner->set_int_value(42);
    serialized.push_back(inner.SerializeAsArray());
  }

  // Wrap it kDepth-1 times, each wrapper has proto_value = the previous.
  for (int i = 1; i < kDepth; ++i) {
    protozero::HeapBuffered<protos::pbzero::DebugAnnotation> outer;
    outer->set_name("wrap");
    outer->set_proto_type_name(".perfetto.protos.DebugAnnotation");
    const std::vector<uint8_t>& prev = serialized.back();
    outer->set_proto_value(prev.data(), prev.size());
    serialized.push_back(outer.SerializeAsArray());
  }

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  ASSERT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  ProtoToArgsParser args_parser(pool);

  const std::vector<uint8_t>& outermost = serialized.back();
  status = args_parser.ParseDebugAnnotation(
      protozero::ConstBytes{outermost.data(), outermost.size()}, *this);
  EXPECT_TRUE(status.ok()) << "ParseDebugAnnotation failed with error: "
                           << status.message();
}

TEST_F(DebugAnnotationParserTest, NestedValueDictMismatchedKeysAndValues) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");
  auto* nested = msg->set_nested_value();
  nested->set_nested_type(protos::pbzero::DebugAnnotation::NestedValue::DICT);
  nested->add_dict_keys("k1");
  nested->add_dict_keys("k2");
  nested->add_dict_keys("k3");
  auto* v1 = nested->add_dict_values();
  v1->set_int_value(1);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);

  base::Status status = ParseDebugAnnotation(args_parser, msg, *this);
  EXPECT_FALSE(status.ok());
}

TEST_F(DebugAnnotationParserTest, NestedValueDictMoreValuesThanKeys) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");
  auto* nested = msg->set_nested_value();
  nested->set_nested_type(protos::pbzero::DebugAnnotation::NestedValue::DICT);
  nested->add_dict_keys("k1");
  auto* v1 = nested->add_dict_values();
  v1->set_int_value(1);
  auto* v2 = nested->add_dict_values();
  v2->set_int_value(2);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);

  base::Status status = ParseDebugAnnotation(args_parser, msg, *this);
  EXPECT_FALSE(status.ok());
}

TEST_F(DebugAnnotationParserTest, NestedValueDictMatchedKeysAndValues) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");
  auto* nested = msg->set_nested_value();
  nested->set_nested_type(protos::pbzero::DebugAnnotation::NestedValue::DICT);
  nested->add_dict_keys("k1");
  nested->add_dict_keys("k2");
  auto* v1 = nested->add_dict_values();
  v1->set_int_value(1);
  auto* v2 = nested->add_dict_values();
  v2->set_int_value(2);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);

  base::Status status = ParseDebugAnnotation(args_parser, msg, *this);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_THAT(args(),
              testing::ElementsAre("root.k1 root.k1 1", "root.k2 root.k2 2"));
}

// A failed parse must not leave stale work items behind for the next call to
// resume (which would dereference dangling pointers).
TEST_F(DebugAnnotationParserTest, ErrorClearsPersistentWorkState) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> bad_msg;
  bad_msg->set_name("bad_root");
  auto* nested = bad_msg->set_nested_value();
  nested->set_nested_type(protos::pbzero::DebugAnnotation::NestedValue::DICT);
  nested->add_dict_keys("k1");
  nested->add_dict_keys("k2");
  auto* v1 = nested->add_dict_values();
  v1->set_int_value(1);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);

  base::Status status = ParseDebugAnnotation(args_parser, bad_msg, *this);
  EXPECT_FALSE(status.ok());

  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> good_msg;
  good_msg->set_name("good_root");
  good_msg->set_int_value(42);

  status = ParseDebugAnnotation(args_parser, good_msg, *this);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_THAT(args(), testing::ElementsAre("good_root good_root 42"));
}

TEST_F(DebugAnnotationParserTest, InternedString) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");

  protozero::HeapBuffered<protos::pbzero::InternedString> string;
  string->set_iid(1);
  string->set_str("foo");
  std::vector<uint8_t> data_serialized = string.SerializeAsArray();

  InternMessage(
      protos::pbzero::InternedData::kDebugAnnotationStringValuesFieldNumber,
      TraceBlobView(
          TraceBlob::CopyFrom(data_serialized.data(), data_serialized.size())));

  msg->set_string_value_iid(1);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);

  auto status = ParseDebugAnnotation(args_parser, msg, *this);
  EXPECT_TRUE(status.ok()) << "ParseDebugAnnotation failed with error:"
                           << status.message();

  EXPECT_THAT(args(), testing::ElementsAre("root root foo"));
}

}  // namespace
}  // namespace perfetto::trace_processor::util
