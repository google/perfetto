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

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"
#include "src/protozero/test/example_proto/test_messages.pbzero.h"
#include "src/trace_processor/test_messages.descriptor.h"
#include "src/trace_processor/util/interned_message_view.h"
#include "test/gtest_and_gmock.h"

#include <cstdint>
#include <limits>
#include <sstream>

namespace perfetto {
namespace trace_processor {
namespace util {
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

  void AddPointer(const Key& key, const void* value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << std::hex
       << reinterpret_cast<uintptr_t>(value) << std::dec;
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

  parser.AddParsingOverrideForField(
      "super_nested.value_c",
      [](const protozero::Field& field, ProtoToArgsParser::Delegate&) {
        static int val = 0;
        ++val;
        EXPECT_EQ(1, val);
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
          "small_enum small_enum NOT_TO_BE", "field_uint64 field_uint64 0",
          "field_uint32 field_uint32 0", "field_int64 field_int64 0"));
}

}  // namespace
}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto
