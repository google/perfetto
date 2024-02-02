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

#include "src/trace_processor/metrics/metrics.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/util/descriptors.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace_processor/metrics_impl.pbzero.h"

namespace perfetto::trace_processor::metrics {
namespace {
using base::gtest_matchers::IsError;

using base::gtest_matchers::IsError;
using testing::IsEmpty;

std::string RunTemplateReplace(
    const std::string& str,
    const std::unordered_map<std::string, std::string>& subs) {
  std::string out;
  EXPECT_EQ(TemplateReplace(str, subs, &out), 0);
  return out;
}

TEST(MetricsTest, TemplateReplace) {
  auto res = RunTemplateReplace("no templates here", {});
  ASSERT_EQ(res, "no templates here");

  res = RunTemplateReplace("{{justtemplate}}", {{"justtemplate", "result"}});
  ASSERT_EQ(res, "result");

  res = RunTemplateReplace("{{temp1}} {{temp2}}!",
                           {{"temp1", "hello"}, {"temp2", "world"}});
  ASSERT_EQ(res, "hello world!");

  std::string unused;
  ASSERT_NE(TemplateReplace("{{missing}}", {{}}, &unused), 0);
}

class ProtoBuilderTest : public ::testing::Test {
 protected:
  template <bool repeated>
  protozero::TypedProtoDecoder<1, repeated> DecodeSingleFieldProto(
      const std::vector<uint8_t>& result_ser) {
    protos::pbzero::ProtoBuilderResult::Decoder result(result_ser.data(),
                                                       result_ser.size());
    protos::pbzero::SingleBuilderResult::Decoder single(result.single());
    protozero::ConstBytes proto_ser = single.protobuf();
    return protozero::TypedProtoDecoder<1, repeated>(proto_ser.data,
                                                     proto_ser.size);
  }
};

TEST_F(ProtoBuilderTest, AppendLong) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;

  // Create the descriptor version of the following message:
  // message TestProto {
  //   optional int64 int_value = 1;
  // }
  DescriptorPool pool;
  ProtoDescriptor descriptor("file.proto", ".perfetto.protos",
                             ".perfetto.protos.TestProto",
                             ProtoDescriptor::Type::kMessage, std::nullopt);
  descriptor.AddField(FieldDescriptor("int_value", 1,
                                      FieldDescriptorProto::TYPE_INT64, "",
                                      std::vector<uint8_t>(), false, false));

  ProtoBuilder builder(&pool, &descriptor);
  ASSERT_OK(builder.AppendSqlValue("int_value", SqlValue::Long(12345)));

  auto result_ser = builder.SerializeToProtoBuilderResult();
  auto proto = DecodeSingleFieldProto<false>(result_ser);
  const protozero::Field& int_field = proto.Get(1);
  ASSERT_EQ(int_field.as_int64(), 12345);
}

TEST_F(ProtoBuilderTest, AppendDouble) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;

  // Create the descriptor version of the following message:
  // message TestProto {
  //   optional double double_value = 1;
  // }
  DescriptorPool pool;
  ProtoDescriptor descriptor("file.proto", ".perfetto.protos",
                             ".perfetto.protos.TestProto",
                             ProtoDescriptor::Type::kMessage, std::nullopt);
  descriptor.AddField(FieldDescriptor("double_value", 1,
                                      FieldDescriptorProto::TYPE_DOUBLE, "",
                                      std::vector<uint8_t>(), false, false));

  ProtoBuilder builder(&pool, &descriptor);
  ASSERT_OK(builder.AppendSqlValue("double_value", SqlValue::Double(1.2345)));

  auto result_ser = builder.SerializeToProtoBuilderResult();
  auto proto = DecodeSingleFieldProto<false>(result_ser);
  const protozero::Field& db_field = proto.Get(1);
  ASSERT_DOUBLE_EQ(db_field.as_double(), 1.2345);
}

TEST_F(ProtoBuilderTest, AppendString) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;

  // Create the descriptor version of the following message:
  // message TestProto {
  //   optional string string_value = 1;
  // }
  DescriptorPool pool;
  ProtoDescriptor descriptor("file.proto", ".perfetto.protos",
                             ".perfetto.protos.TestProto",
                             ProtoDescriptor::Type::kMessage, std::nullopt);
  descriptor.AddField(FieldDescriptor("string_value", 1,
                                      FieldDescriptorProto::TYPE_STRING, "",
                                      std::vector<uint8_t>(), false, false));

  ProtoBuilder builder(&pool, &descriptor);
  ASSERT_OK(
      builder.AppendSqlValue("string_value", SqlValue::String("hello world!")));

  auto result_ser = builder.SerializeToProtoBuilderResult();
  auto proto = DecodeSingleFieldProto<false>(result_ser);
  const protozero::Field& str_field = proto.Get(1);
  ASSERT_EQ(str_field.as_std_string(), "hello world!");
}

TEST_F(ProtoBuilderTest, AppendNested) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;

  // Create the descriptor version of the following message:
  // message TestProto {
  //   message NestedProto {
  //     optional int64 nested_int_value = 1;
  //   }
  //   optional NestedProto nested_value = 1;
  // }
  DescriptorPool pool;
  ProtoDescriptor nested("file.proto", ".perfetto.protos",
                         ".perfetto.protos.TestProto.NestedProto",
                         ProtoDescriptor::Type::kMessage, std::nullopt);
  nested.AddField(FieldDescriptor("nested_int_value", 1,
                                  FieldDescriptorProto::TYPE_INT64, "",
                                  std::vector<uint8_t>(), false, false));

  ProtoDescriptor descriptor("file.proto", ".perfetto.protos",
                             ".perfetto.protos.TestProto",
                             ProtoDescriptor::Type::kMessage, std::nullopt);
  auto field =
      FieldDescriptor("nested_value", 1, FieldDescriptorProto::TYPE_MESSAGE,
                      ".perfetto.protos.TestProto.NestedProto",
                      std::vector<uint8_t>(), false, false);
  field.set_resolved_type_name(".perfetto.protos.TestProto.NestedProto");
  descriptor.AddField(field);

  ProtoBuilder nest_builder(&pool, &nested);
  ASSERT_OK(
      nest_builder.AppendSqlValue("nested_int_value", SqlValue::Long(789)));

  auto nest_ser = nest_builder.SerializeToProtoBuilderResult();

  ProtoBuilder builder(&pool, &descriptor);
  ASSERT_OK(builder.AppendSqlValue(
      "nested_value", SqlValue::Bytes(nest_ser.data(), nest_ser.size())));

  auto result_ser = builder.SerializeToProtoBuilderResult();
  auto proto = DecodeSingleFieldProto<false>(result_ser);
  const protozero::Field& nest_field = proto.Get(1);
  ASSERT_EQ(nest_field.type(),
            protozero::proto_utils::ProtoWireType::kLengthDelimited);

  protozero::ConstBytes nest_bytes = nest_field.as_bytes();
  protozero::TypedProtoDecoder<1, false> nest(nest_bytes.data, nest_bytes.size);

  const protozero::Field& nest_int_field = nest.Get(1);
  ASSERT_EQ(nest_int_field.type(),
            protozero::proto_utils::ProtoWireType::kVarInt);
  ASSERT_EQ(nest_int_field.as_int64(), 789);
}

TEST_F(ProtoBuilderTest, AppendRepeatedEmpty) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;

  // Create the descriptor version of the following message:
  // message TestProto {
  //   repeated int64 int_value = 1;
  // }
  DescriptorPool pool;
  ProtoDescriptor descriptor("file.proto", ".perfetto.protos",
                             ".perfetto.protos.TestProto",
                             ProtoDescriptor::Type::kMessage, std::nullopt);
  descriptor.AddField(FieldDescriptor("rep_int_value", 1,
                                      FieldDescriptorProto::TYPE_INT64, "",
                                      std::vector<uint8_t>(), true, false));

  ASSERT_THAT(RepeatedFieldBuilder().SerializeToProtoBuilderResult(),
              IsEmpty());

  ProtoBuilder builder(&pool, &descriptor);
  ASSERT_OK(builder.AppendSqlValue("rep_int_value", SqlValue()));

  auto proto =
      DecodeSingleFieldProto<true>(builder.SerializeToProtoBuilderResult());
  auto it = proto.GetRepeated<int64_t>(1);
  ASSERT_FALSE(it);
}

TEST_F(ProtoBuilderTest, AppendRepeatedPrimitive) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;

  // Create the descriptor version of the following message:
  // message TestProto {
  //   repeated int64 int_value = 1;
  // }
  DescriptorPool pool;
  ProtoDescriptor descriptor("file.proto", ".perfetto.protos",
                             ".perfetto.protos.TestProto",
                             ProtoDescriptor::Type::kMessage, std::nullopt);
  descriptor.AddField(FieldDescriptor("rep_int_value", 1,
                                      FieldDescriptorProto::TYPE_INT64, "",
                                      std::vector<uint8_t>(), true, false));

  RepeatedFieldBuilder rep_builder;
  rep_builder.AddSqlValue(SqlValue::Long(1234));
  rep_builder.AddSqlValue(SqlValue::Long(5678));

  std::vector<uint8_t> rep_ser = rep_builder.SerializeToProtoBuilderResult();

  ProtoBuilder builder(&pool, &descriptor);
  ASSERT_OK(builder.AppendSqlValue(
      "rep_int_value", SqlValue::Bytes(rep_ser.data(), rep_ser.size())));

  auto proto =
      DecodeSingleFieldProto<true>(builder.SerializeToProtoBuilderResult());
  auto it = proto.GetRepeated<int64_t>(1);
  ASSERT_EQ(*it, 1234);
  ASSERT_EQ(*++it, 5678);
  ASSERT_FALSE(++it);
}

TEST_F(ProtoBuilderTest, AppendEnums) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;

  // Create the descriptor version of the following enum and message:
  // enum TestEnum {
  //   FIRST = 1,
  //   SECOND = 2,
  //   THIRD = 3
  // }
  // message TestMessage {
  //   optional TestEnum enum_value = 1;
  // }
  DescriptorPool pool;
  ProtoDescriptor enum_descriptor("file.proto", ".perfetto.protos",
                                  ".perfetto.protos.TestEnum",
                                  ProtoDescriptor::Type::kEnum, std::nullopt);
  enum_descriptor.AddEnumValue(1, "FIRST");
  enum_descriptor.AddEnumValue(2, "SECOND");
  enum_descriptor.AddEnumValue(3, "THIRD");
  pool.AddProtoDescriptorForTesting(enum_descriptor);

  ProtoDescriptor descriptor("file.proto", ".perfetto.protos",
                             ".perfetto.protos.TestMessage",
                             ProtoDescriptor::Type::kMessage, std::nullopt);
  FieldDescriptor enum_field("enum_value", 1, FieldDescriptorProto::TYPE_ENUM,
                             ".perfetto.protos.TestEnum",
                             std::vector<uint8_t>(), false, false);
  enum_field.set_resolved_type_name(".perfetto.protos.TestEnum");
  descriptor.AddField(enum_field);
  pool.AddProtoDescriptorForTesting(descriptor);

  ProtoBuilder value_builder(&pool, &descriptor);
  ASSERT_THAT(value_builder.AppendSqlValue("enum_value", SqlValue::Long(4)),
              IsError());
  ASSERT_OK(value_builder.AppendSqlValue("enum_value", SqlValue::Long(3)));
  ASSERT_THAT(value_builder.AppendSqlValue("enum_value", SqlValue::Long(6)),
              IsError());

  auto value_proto = DecodeSingleFieldProto<false>(
      value_builder.SerializeToProtoBuilderResult());
  ASSERT_EQ(value_proto.Get(1).as_int32(), 3);

  ProtoBuilder str_builder(&pool, &descriptor);
  ASSERT_THAT(
      str_builder.AppendSqlValue("enum_value", SqlValue::String("FOURTH")),
      IsError());
  ASSERT_OK(
      str_builder.AppendSqlValue("enum_value", SqlValue::String("SECOND")));
  ASSERT_THAT(
      str_builder.AppendSqlValue("enum_value", SqlValue::String("OTHER")),
      IsError());

  auto str_proto = DecodeSingleFieldProto<false>(
      str_builder.SerializeToProtoBuilderResult());
  ASSERT_EQ(str_proto.Get(1).as_int32(), 2);
}

}  // namespace
}  // namespace perfetto::trace_processor::metrics
