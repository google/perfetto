/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "perfetto/protozero/field.h"
#include "test/gtest_and_gmock.h"

#include "src/protovm/error_handling.h"
#include "src/protovm/ro_cursor.h"
#include "src/protovm/test/protos/incremental_trace.pb.h"
#include "src/protovm/test/sample_packets.h"
#include "src/protovm/test/utils.h"

namespace perfetto {
namespace protovm {
namespace test {

class RoCursorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cursor_empty_ = RoCursor{AsConstBytes(proto_empty_)};
    cursor_trace_entry_with_two_elements_ =
        RoCursor{AsConstBytes(proto_trace_entry_with_two_elements_)};
  }

  const std::string proto_empty_{};
  const std::string proto_trace_entry_with_two_elements_{
      SamplePackets::TraceEntryWithTwoElements().SerializeAsString()};

  RoCursor cursor_empty_;
  RoCursor cursor_trace_entry_with_two_elements_;
};

TEST_F(RoCursorTest, EnterField_IncompatibleWireType) {
  auto proto = SamplePackets::TraceEntryWithTwoElements().SerializeAsString();
  RoCursor cursor(AsConstBytes(proto));
  ASSERT_TRUE(
      cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsOk());
  ASSERT_TRUE(cursor.EnterField(protos::Element::kIdFieldNumber).IsOk());
  // attempt enter operation on a scalar field
  ASSERT_TRUE(cursor.EnterField(0).IsAbort());
}

TEST_F(RoCursorTest, EnterField_FieldNotAvailable) {
  {
    auto cursor = cursor_empty_;
    ASSERT_TRUE(
        cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsError());
  }
  {
    auto proto = SamplePackets::TraceEntryWithTwoElements().SerializeAsString();
    RoCursor cursor(AsConstBytes(proto));
    ASSERT_TRUE(cursor.EnterField(0).IsError());
  }
}

TEST_F(RoCursorTest, EnterField_FieldAvailable) {
  auto cursor = cursor_trace_entry_with_two_elements_;
  ASSERT_TRUE(
      cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsOk());

  auto cursor_id = cursor;
  ASSERT_TRUE(cursor_id.EnterField(protos::Element::kIdFieldNumber).IsOk());
  ASSERT_EQ(cursor_id.GetScalar().value(), Scalar::VarInt(0));

  auto cursor_value = cursor;
  ASSERT_TRUE(
      cursor_value.EnterField(protos::Element::kValueFieldNumber).IsOk());
  ASSERT_EQ(cursor_value.GetScalar().value(), Scalar::VarInt(10));
}

TEST_F(RoCursorTest, EnterIndexedRepeatedField_IncompatibleWireType) {
  auto cursor = cursor_trace_entry_with_two_elements_;
  ASSERT_TRUE(
      cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsOk());
  ASSERT_TRUE(cursor.EnterField(protos::Element::kIdFieldNumber).IsOk());
  // attempt enter operation on a scalar field
  ASSERT_TRUE(cursor.EnterRepeatedFieldAt(0, 0).IsAbort());
}

TEST_F(RoCursorTest, EnterIndexedRepeatedField_FieldNotAvailable) {
  {
    auto cursor = cursor_empty_;
    ASSERT_TRUE(
        cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0)
            .IsError());
  }
  {
    auto proto = SamplePackets::TraceEntryWithTwoElements().SerializeAsString();
    RoCursor cursor(AsConstBytes(proto));
    ASSERT_TRUE(cursor.EnterRepeatedFieldAt(0, 0).IsError());
  }
}

TEST_F(RoCursorTest, EnterIndexedRepeatedField_FieldAvailable) {
  auto cursor_element0 = cursor_trace_entry_with_two_elements_;
  ASSERT_TRUE(
      cursor_element0
          .EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0)
          .IsOk());
  ASSERT_TRUE(
      cursor_element0.EnterField(protos::Element::kIdFieldNumber).IsOk());
  ASSERT_EQ(cursor_element0.GetScalar().value(), Scalar::VarInt(0));

  auto cursor_element1 = cursor_trace_entry_with_two_elements_;
  ASSERT_TRUE(
      cursor_element1
          .EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 1)
          .IsOk());
  ASSERT_TRUE(
      cursor_element1.EnterField(protos::Element::kIdFieldNumber).IsOk());
  ASSERT_EQ(cursor_element1.GetScalar().value(), Scalar::VarInt(1));
}

TEST_F(RoCursorTest, IterateRepeatedField_IncompatibleWireType) {
  auto cursor = cursor_trace_entry_with_two_elements_;
  ASSERT_TRUE(
      cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsOk());
  ASSERT_TRUE(cursor.EnterField(protos::Element::kIdFieldNumber).IsOk());
  // attempt to iterate scalar field
  auto status_or_it =
      cursor.IterateRepeatedField(protos::TraceEntry::kElementsFieldNumber);
  ASSERT_TRUE(status_or_it.IsAbort());
}

TEST_F(RoCursorTest, IterateRepeatedField_FieldsNotAvailable) {
  {
    auto cursor = cursor_empty_;
    auto status_or_it =
        cursor.IterateRepeatedField(protos::TraceEntry::kElementsFieldNumber);
    ASSERT_TRUE(status_or_it.IsOk());
    ASSERT_FALSE(*status_or_it);
  }
  {
    auto proto = SamplePackets::TraceEntryWithTwoElements().SerializeAsString();
    RoCursor cursor(AsConstBytes(proto));
    auto status_or_it = cursor.IterateRepeatedField(0);
    ASSERT_TRUE(status_or_it.IsOk());
    ASSERT_FALSE(*status_or_it);
  }
}

TEST_F(RoCursorTest, IterateRepeatedField_FieldsAvailable) {
  auto cursor = cursor_trace_entry_with_two_elements_;
  auto it =
      *cursor.IterateRepeatedField(protos::TraceEntry::kElementsFieldNumber);
  // elements[0]
  {
    ASSERT_TRUE(it);
    auto cursor_element = *it;
    ASSERT_TRUE(
        cursor_element.EnterField(protos::Element::kIdFieldNumber).IsOk());
    ASSERT_EQ(cursor_element.GetScalar().value(), Scalar::VarInt(0));
  }
  ++it;
  // elements[1]
  {
    ASSERT_TRUE(it);
    auto cursor_element = *it;
    ASSERT_TRUE(
        cursor_element.EnterField(protos::Element::kIdFieldNumber).IsOk());
    ASSERT_EQ(cursor_element.GetScalar().value(), Scalar::VarInt(1));
  }
  ++it;
  ASSERT_FALSE(it);
}

TEST_F(RoCursorTest, IsScalar) {
  protos::TraceEntry entry;
  auto* element = entry.add_elements();
  element->set_id(1);
  element->set_value(2);
  element->set_value_fixed32(3);
  element->set_value_fixed64(4);

  auto proto = entry.SerializeAsString();
  auto cursor = RoCursor{AsConstBytes(proto)};

  EXPECT_FALSE(cursor.IsScalar());

  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  EXPECT_FALSE(cursor.IsScalar());

  auto id = cursor;
  id.EnterField(protos::Element::kIdFieldNumber);
  EXPECT_TRUE(id.IsScalar());

  auto value = cursor;
  value.EnterField(protos::Element::kValueFieldNumber);
  EXPECT_TRUE(value.IsScalar());

  auto value32 = cursor;
  value32.EnterField(protos::Element::kValueFixed32FieldNumber);
  EXPECT_TRUE(value32.IsScalar());

  auto value64 = cursor;
  value64.EnterField(protos::Element::kValueFixed64FieldNumber);
  EXPECT_TRUE(value64.IsScalar());
}

TEST_F(RoCursorTest, IsBytes) {
  auto cursor = cursor_trace_entry_with_two_elements_;
  EXPECT_TRUE(cursor.IsBytes());

  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  EXPECT_TRUE(cursor.IsBytes());

  cursor.EnterField(protos::Element::kIdFieldNumber);
  EXPECT_FALSE(cursor.IsBytes());
}

TEST_F(RoCursorTest, GetScalar_IncompatibleWireType) {
  {
    auto cursor = cursor_empty_;
    ASSERT_TRUE(cursor.GetScalar().IsAbort());
  }
  {
    auto cursor = cursor_trace_entry_with_two_elements_;
    ASSERT_TRUE(cursor.GetScalar().IsAbort());
    ASSERT_TRUE(
        cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsOk());
    ASSERT_TRUE(cursor.GetScalar().IsAbort());
  }
}

TEST_F(RoCursorTest, GetScalar_Success) {
  auto cursor = cursor_trace_entry_with_two_elements_;
  cursor.EnterField(protos::TraceEntry::kElementsFieldNumber);

  auto id = cursor;
  id.EnterField(protos::Element::kIdFieldNumber);
  ASSERT_EQ(id.GetScalar().value(), Scalar::VarInt(0));

  auto value = cursor;
  value.EnterField(protos::Element::kValueFieldNumber);
  ASSERT_EQ(value.GetScalar().value(), Scalar::VarInt(10));

  auto value_fixed32 = cursor;
  value_fixed32.EnterField(protos::Element::kValueFixed32FieldNumber);
  ASSERT_EQ(value_fixed32.GetScalar().value(), Scalar::Fixed32(32));

  auto value_fixed64 = cursor;
  value_fixed64.EnterField(protos::Element::kValueFixed64FieldNumber);
  ASSERT_EQ(value_fixed64.GetScalar().value(), Scalar::Fixed64(64));
}

TEST_F(RoCursorTest, GetBytes_IncompatibleWireType) {
  auto cursor = cursor_trace_entry_with_two_elements_;
  ASSERT_TRUE(
      cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsOk());
  ASSERT_TRUE(cursor.EnterField(protos::Element::kValueFieldNumber).IsOk());

  ASSERT_TRUE(cursor.GetBytes().IsAbort());
}

TEST_F(RoCursorTest, GetBytes_Success) {
  auto cursor = cursor_trace_entry_with_two_elements_;

  // full trace entry
  {
    auto bytes = *cursor.GetBytes();

    protos::TraceEntry entry;
    entry.ParseFromString(bytes.ToStdString());

    ASSERT_EQ(entry.elements_size(), 2);
    ASSERT_EQ(entry.elements(0).id(), 0);
    ASSERT_EQ(entry.elements(0).value(), 10);
    ASSERT_EQ(entry.elements(1).id(), 1);
    ASSERT_EQ(entry.elements(1).value(), 11);
  }

  // elements[0]
  {
    ASSERT_TRUE(
        cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsOk());

    auto bytes = *cursor.GetBytes();

    protos::Element element;
    element.ParseFromString(bytes.ToStdString());

    ASSERT_EQ(element.id(), 0);
    ASSERT_EQ(element.value(), 10);
  }
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
