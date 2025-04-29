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

#include <cstdint>

#include "perfetto/protozero/field.h"
#include "src/protovm/test/utils.h"
#include "test/gtest_and_gmock.h"

#include "src/protovm/error_handling.h"
#include "src/protovm/rw_proto.h"
#include "src/protovm/test/protos/incremental_trace.pb.h"
#include "src/protovm/test/sample_packets.h"
#include "src/protovm/test/utils.h"

namespace perfetto {
namespace protovm {
namespace test {

class RwProtoTest : public ::testing::Test {
 protected:
  void SetUp() override { PopulateRwProtoWithTwoElements(); }

  void PopulateRwProtoWithTwoElements() {
    auto root = data_trace_entry_with_two_elements_.GetRoot();

    // elements[0]
    {
      auto element = root;
      element.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);

      // id field
      auto id = element;
      id.EnterField(protos::Element::kIdFieldNumber);
      id.SetScalar(Scalar::VarInt(0));

      // value field
      auto value = element;
      value.EnterField(protos::Element::kValueFieldNumber);
      value.SetScalar(Scalar::VarInt(10));

      // value_fixed32 field
      auto value_fixed32 = element;
      value_fixed32.EnterField(protos::Element::kValueFixed32FieldNumber);
      value_fixed32.SetScalar(Scalar::Fixed32(32));

      // value_fixed64 field
      auto value_fixed64 = element;
      value_fixed64.EnterField(protos::Element::kValueFixed64FieldNumber);
      value_fixed64.SetScalar(Scalar::Fixed64(64));
    }

    // elements[1]
    {
      auto element = root;
      element.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 1);

      // id field
      auto id = element;
      id.EnterField(protos::Element::kIdFieldNumber);
      id.SetScalar(Scalar::VarInt(1));

      // value field
      auto value = element;
      value.EnterField(protos::Element::kValueFieldNumber);
      value.SetScalar(Scalar::VarInt(11));
    }
  }

  void CheckProtoWithTwoElements(const std::string& proto) const {
    protos::TraceEntry entry;
    entry.ParseFromString(proto);

    ASSERT_EQ(entry.elements_size(), 2);

    ASSERT_EQ(entry.elements(0).id(), 0);
    ASSERT_EQ(entry.elements(0).value(), 10);
    ASSERT_EQ(entry.elements(0).value_fixed32(), static_cast<uint32_t>(32));
    ASSERT_EQ(entry.elements(0).value_fixed64(), static_cast<uint64_t>(64));

    ASSERT_EQ(entry.elements(1).id(), 1);
    ASSERT_EQ(entry.elements(1).value(), 11);
  }

  Allocator allocator_{10 * 1024 * 1024};
  RwProto data_empty_{&allocator_};
  RwProto data_trace_entry_with_two_elements_{&allocator_};
  protozero::ConstBytes bytes_empty_{nullptr, 0};
};

TEST_F(RwProtoTest, HasField_IncompatibleWireType) {
  RwProto::Cursor cursor = data_trace_entry_with_two_elements_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  cursor.EnterField(protos::Element::kIdFieldNumber);
  ASSERT_TRUE(cursor.HasField(0).IsAbort());
}

TEST_F(RwProtoTest, HasField_FieldNotAvailable) {
  {
    RwProto::Cursor cursor = data_empty_.GetRoot();
    ASSERT_EQ(*cursor.HasField(protos::TraceEntry::kElementsFieldNumber),
              false);
  }
  {
    RwProto::Cursor cursor = data_trace_entry_with_two_elements_.GetRoot();
    ASSERT_EQ(*cursor.HasField(0), false);
  }
  {
    RwProto::Cursor cursor = data_trace_entry_with_two_elements_.GetRoot();
    cursor.EnterField(protos::TraceEntry::kElementsFieldNumber);
    ASSERT_EQ(*cursor.HasField(0), false);
  }
}

TEST_F(RwProtoTest, HasField_FieldAvailable) {
  {
    RwProto::Cursor cursor = data_trace_entry_with_two_elements_.GetRoot();
    ASSERT_EQ(*cursor.HasField(protos::TraceEntry::kElementsFieldNumber), true);
  }
  {
    RwProto::Cursor cursor = data_trace_entry_with_two_elements_.GetRoot();
    cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
    ASSERT_EQ(*cursor.HasField(protos::Element::kIdFieldNumber), true);
    ASSERT_EQ(*cursor.HasField(protos::Element::kValueFieldNumber), true);
  }
}

TEST_F(RwProtoTest, HasField_FieldNotAvailableAsBytes) {
  RwProto::Cursor cursor = data_empty_.GetRoot();
  auto proto = SamplePackets::TraceEntryWithTwoElements().SerializeAsString();
  cursor.SetBytes(AsConstBytes(proto));
  ASSERT_EQ(*cursor.HasField(0), false);
}

TEST_F(RwProtoTest, HasField_FieldAvailableAsBytes) {
  RwProto::Cursor cursor = data_empty_.GetRoot();
  auto proto = SamplePackets::TraceEntryWithTwoElements().SerializeAsString();
  cursor.SetBytes(AsConstBytes(proto));
  ASSERT_EQ(*cursor.HasField(protos::TraceEntry::kElementsFieldNumber), true);
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  ASSERT_EQ(*cursor.HasField(protos::Element::kIdFieldNumber), true);
  ASSERT_EQ(*cursor.HasField(protos::Element::kValueFieldNumber), true);
}

TEST_F(RwProtoTest, EnterField_IncompatibleWireType) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  ASSERT_TRUE(
      cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0)
          .IsOk());
  ASSERT_TRUE(cursor.EnterField(protos::Element::kIdFieldNumber).IsOk());
  ASSERT_TRUE(cursor.EnterField(0).IsAbort());
}

TEST_F(RwProtoTest, EnterField_IncompatibleFieldType_IndexedRepeatedField) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  ASSERT_TRUE(
      cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsAbort());
}

TEST_F(RwProtoTest, EnterField_IncompatibleFieldType_MappedRepeatedField) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();

  {
    // Trigger internal node conversion from "indexed repeated field" to "mapped
    // repeated field"
    auto element = cursor;
    ASSERT_TRUE(
        element
            .EnterRepeatedFieldByKey(protos::TraceEntry::kElementsFieldNumber,
                                     protos::Element::kIdFieldNumber, 0)
            .IsOk());
  }

  ASSERT_TRUE(
      cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsAbort());
}

TEST_F(RwProtoTest, EnterField_FieldNotAvailableGetsCreated) {
  RwProto::Cursor cursor = data_empty_.GetRoot();
  ASSERT_TRUE(
      cursor.EnterField(protos::TraceEntry::kElementsFieldNumber).IsOk());
  ASSERT_TRUE(cursor.EnterField(protos::Element::kIdFieldNumber).IsOk());
  cursor.SetScalar(Scalar::VarInt(10));

  protos::TraceEntry entry;
  entry.ParseFromString(data_empty_.SerializeAsString());
  ASSERT_EQ(entry.elements_size(), 1);
  ASSERT_EQ(entry.elements(0).id(), 10);
}

TEST_F(RwProtoTest, EnterField_FieldAvailable) {
  RwProto::Cursor cursor = data_trace_entry_with_two_elements_.GetRoot();
  ASSERT_TRUE(
      cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0)
          .IsOk());
  ASSERT_TRUE(cursor.EnterField(protos::Element::kIdFieldNumber).IsOk());

  CheckProtoWithTwoElements(
      data_trace_entry_with_two_elements_.SerializeAsString());
}

TEST_F(RwProtoTest, EnterField_FieldNotAvailableAsBytesGetsCreated) {
  RwProto::Cursor cursor = data_empty_.GetRoot();

  auto entry = SamplePackets::TraceEntryWithTwoElements();
  entry.mutable_elements(0)->clear_value();
  auto proto = entry.SerializeAsString();
  cursor.SetBytes(AsConstBytes(proto));

  // elements[0].value = 10
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  cursor.EnterField(protos::Element::kValueFieldNumber);
  cursor.SetScalar(Scalar::VarInt(10));

  CheckProtoWithTwoElements(data_empty_.SerializeAsString());
}

TEST_F(RwProtoTest, EnterField_FieldAvailableAsBytes) {
  auto cursor = data_empty_.GetRoot();
  auto proto = SamplePackets::TraceEntryWithTwoElements().SerializeAsString();
  cursor.SetBytes(AsConstBytes(proto));

  ASSERT_TRUE(
      cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0)
          .IsOk());
  ASSERT_TRUE(cursor.EnterField(protos::Element::kIdFieldNumber).IsOk());
  ASSERT_EQ(cursor.GetScalar().value(), Scalar::VarInt(0));
}

TEST_F(RwProtoTest, EnterIndexedRepeatedField_IncompatibleWireType) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  cursor.EnterField(protos::Element::kIdFieldNumber);
  ASSERT_TRUE(cursor.EnterRepeatedFieldAt(0, 0).IsAbort());
}

// TODO: support accessing repeated fields by both index and key.
// Currently, a repeated field can be organized internally as either "indexed"
// or "mapped", but not both. This means that once a field is accessed using
// a key (EnterRepeatedFieldByKey), it can no longer be accessed by
// index (EnterRepeatedFieldAt). While it's technically possible to allow
// both access methods (same internal node inserted into two intrusive maps),
// it's a low priority as current use cases don't require it.
TEST_F(RwProtoTest,
       EnterIndexedRepeatedField_IncompatibleFieldType_MappedRepeatedField) {
  // Trigger creation of internal "mapped repeated field" node
  {
    auto cursor = data_trace_entry_with_two_elements_.GetRoot();
    ASSERT_TRUE(
        cursor
            .EnterRepeatedFieldByKey(protos::TraceEntry::kElementsFieldNumber,
                                     protos::Element::kIdFieldNumber, 0)
            .IsOk());
  }

  // Attempt to access as "indexed repeated field"
  {
    auto cursor = data_trace_entry_with_two_elements_.GetRoot();
    ASSERT_TRUE(
        cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0)
            .IsAbort());
  }
}

// Only append operations (insert at index == elements.size()) are supported.
// Attempting to insert a field with index > elements.size() causes an
// abort.
TEST_F(RwProtoTest,
       EnterIndexedRepeatedField_FieldNotAvailable_AbortIfNotSimpleAppend) {
  // Attempt to enter field_index (1) > elements.size() (0) => abort
  {
    auto cursor = data_empty_.GetRoot();
    ASSERT_TRUE(
        cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 1)
            .IsAbort());
  }
  // Attempt to enter field_index (3) > elements.size() (2) => abort
  {
    auto cursor = data_trace_entry_with_two_elements_.GetRoot();
    ASSERT_TRUE(
        cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 3)
            .IsAbort());
  }
}

TEST_F(RwProtoTest, EnterIndexedRepeatedField_FieldNotAvailable_Append) {
  auto cursor = data_empty_.GetRoot();

  // append elements[0]
  {
    auto element = cursor;
    ASSERT_TRUE(
        element
            .EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0)
            .IsOk());
    auto proto = SamplePackets::TraceEntryWithTwoElements()
                     .elements(0)
                     .SerializeAsString();
    element.SetBytes(AsConstBytes(proto));
  }

  // append elements[1]
  {
    auto element = cursor;
    ASSERT_TRUE(
        element
            .EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 1)
            .IsOk());
    auto proto = SamplePackets::TraceEntryWithTwoElements()
                     .elements(1)
                     .SerializeAsString();
    element.SetBytes(AsConstBytes(proto));
  }

  CheckProtoWithTwoElements(data_empty_.SerializeAsString());
}

TEST_F(RwProtoTest, EnterIndexedRepeatedField_FieldAvailable) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();

  // elements[0].id = 100
  {
    auto id = cursor;
    ASSERT_TRUE(
        id.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0)
            .IsOk());
    id.EnterField(protos::Element::kIdFieldNumber);
    id.SetScalar(Scalar::VarInt(100));
  }

  // elements[1].id = 101
  {
    auto id = cursor;
    ASSERT_TRUE(
        id.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 1)
            .IsOk());
    id.EnterField(protos::Element::kIdFieldNumber);
    id.SetScalar(Scalar::VarInt(101));
  }

  auto proto = data_trace_entry_with_two_elements_.SerializeAsString();
  protos::TraceEntry entry;
  entry.ParseFromString(proto);
  ASSERT_EQ(entry.elements_size(), 2);
  ASSERT_EQ(entry.elements(0).id(), 100);
  ASSERT_EQ(entry.elements(0).value(), 10);
  ASSERT_EQ(entry.elements(1).id(), 101);
  ASSERT_EQ(entry.elements(1).value(), 11);
}

TEST_F(RwProtoTest,
       EnterIndexedRepeatedField_FieldAvailable_NotDetectedAsRepeatedYet) {
  auto cursor = data_empty_.GetRoot();

  // set only elements[0] -> initially considered a simple field (not repeated)
  auto proto = SamplePackets::TraceEntryWithOneElement().SerializeAsString();
  cursor.SetBytes(AsConstBytes(proto));

  // append elements[1] -> detect elements is an indexed repeated field and
  // reorganize the internal nodes accordingly
  ASSERT_TRUE(
      cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 1)
          .IsOk());
  auto proto_element1 = SamplePackets::TraceEntryWithTwoElements()
                            .elements(1)
                            .SerializeAsString();
  cursor.SetBytes(AsConstBytes(proto_element1));

  CheckProtoWithTwoElements(data_empty_.SerializeAsString());
}

TEST_F(RwProtoTest, IterateRepeatedField_IncompatibleWireType) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  cursor.EnterField(protos::Element::kIdFieldNumber);
  ASSERT_TRUE(cursor.IterateRepeatedField(0).IsAbort());
}

TEST_F(RwProtoTest, IterateRepeatedField_FieldsNotAvailable) {
  auto cursor = data_empty_.GetRoot();
  auto status_or_it =
      cursor.IterateRepeatedField(protos::TraceEntry::kElementsFieldNumber);
  ASSERT_FALSE(static_cast<bool>(*status_or_it));
}

TEST_F(RwProtoTest, IterateRepeatedField_FieldsAvailable) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  auto it =
      *cursor.IterateRepeatedField(protos::TraceEntry::kElementsFieldNumber);

  // elements[0]
  {
    ASSERT_TRUE(static_cast<bool>(it));
    auto value = it.GetCursor();
    value.EnterField(protos::Element::kValueFieldNumber);
    value.SetScalar(Scalar::VarInt(100));
  }

  // elements[1]
  {
    ++it;
    ASSERT_TRUE(static_cast<bool>(it));
    auto value = it.GetCursor();
    value.EnterField(protos::Element::kValueFieldNumber);
    value.SetScalar(Scalar::VarInt(101));
  }

  // elements[one_past_end]
  {
    ++it;
    ASSERT_FALSE(static_cast<bool>(it));
  }

  auto proto = data_trace_entry_with_two_elements_.SerializeAsString();
  protos::TraceEntry entry;
  entry.ParseFromString(proto);

  ASSERT_EQ(entry.elements_size(), 2);
  ASSERT_EQ(entry.elements(0).id(), 0);
  ASSERT_EQ(entry.elements(0).value(), 100);
  ASSERT_EQ(entry.elements(1).id(), 1);
  ASSERT_EQ(entry.elements(1).value(), 101);
}

TEST_F(RwProtoTest,
       IterateRepeatedField_FieldsAvailable_NotDetectedAsRepeatedYet) {
  auto cursor = data_empty_.GetRoot();

  // set only elements[0] -> initially considered a simple field (not repeated)
  auto proto = SamplePackets::TraceEntryWithOneElement().SerializeAsString();
  cursor.SetBytes(AsConstBytes(proto));

  // request iteration of elements -> detect elements is an indexed repeated
  // field and reorganize the internal nodes accordingly
  auto it =
      *cursor.IterateRepeatedField(protos::TraceEntry::kElementsFieldNumber);

  // element[0]
  {
    ASSERT_TRUE(static_cast<bool>(it));
    auto value = it.GetCursor();
    value.EnterField(protos::Element::kValueFieldNumber);
    value.SetScalar(Scalar::VarInt(100));
  }

  // element[end_past_one]
  {
    ++it;
    ASSERT_FALSE(static_cast<bool>(it));
  }

  protos::TraceEntry entry;
  entry.ParseFromString(data_empty_.SerializeAsString());

  ASSERT_EQ(entry.elements_size(), 1);
  ASSERT_EQ(entry.elements(0).id(), 0);
  ASSERT_EQ(entry.elements(0).value(), 100);
}

TEST_F(RwProtoTest, EnterMappedRepeatedField_IncompatibleWireType) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  cursor.EnterField(protos::Element::kIdFieldNumber);
  ASSERT_TRUE(cursor.EnterRepeatedFieldByKey(0, 0, 0).IsAbort());
}

TEST_F(RwProtoTest, EnterMappedRepeatedField_FieldNotAvailable) {
  auto cursor = data_empty_.GetRoot();
  ASSERT_TRUE(
      cursor
          .EnterRepeatedFieldByKey(protos::TraceEntry::kElementsFieldNumber,
                                   protos::Element::kIdFieldNumber, 10)
          .IsOk());

  // elements[10].value = 100
  cursor.EnterField(protos::Element::kValueFieldNumber);
  cursor.SetScalar(Scalar::VarInt(100));

  protos::TraceEntry entry;
  entry.ParseFromString(data_empty_.SerializeAsString());

  ASSERT_EQ(entry.elements_size(), 1);
  ASSERT_EQ(entry.elements(0).value(), 100);
}

TEST_F(RwProtoTest, EnterMappedRepeatedField_FieldAvailable) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  ASSERT_TRUE(
      cursor
          .EnterRepeatedFieldByKey(protos::TraceEntry::kElementsFieldNumber,
                                   protos::Element::kIdFieldNumber, 0)
          .IsOk());

  // elements[0].value = 100
  cursor.EnterField(protos::Element::kValueFieldNumber);
  cursor.SetScalar(Scalar::VarInt(100));

  protos::TraceEntry entry;
  entry.ParseFromString(
      data_trace_entry_with_two_elements_.SerializeAsString());

  ASSERT_EQ(entry.elements_size(), 2);
  ASSERT_EQ(entry.elements(0).id(), 0);
  ASSERT_EQ(entry.elements(0).value(), 100);
  ASSERT_EQ(entry.elements(1).id(), 1);
  ASSERT_EQ(entry.elements(1).value(), 11);
}

TEST_F(RwProtoTest, EnterMappedRepeatedField_FieldAvailableAsBytes) {
  auto cursor = data_empty_.GetRoot();

  auto proto = SamplePackets::TraceEntryWithTwoElements().SerializeAsString();
  cursor.SetBytes(AsConstBytes(proto));

  ASSERT_TRUE(
      cursor
          .EnterRepeatedFieldByKey(protos::TraceEntry::kElementsFieldNumber,
                                   protos::Element::kIdFieldNumber, 0)
          .IsOk());

  // elements[0].value = 100
  cursor.EnterField(protos::Element::kValueFieldNumber);
  cursor.SetScalar(Scalar::VarInt(100));

  protos::TraceEntry entry;
  entry.ParseFromString(data_empty_.SerializeAsString());

  ASSERT_EQ(entry.elements_size(), 2);
  ASSERT_EQ(entry.elements(0).id(), 0);
  ASSERT_EQ(entry.elements(0).value(), 100);
  ASSERT_EQ(entry.elements(1).id(), 1);
  ASSERT_EQ(entry.elements(1).value(), 11);
}

TEST_F(RwProtoTest,
       EnterMappedRepeatedField_FieldAvailable_NotDetectedAsRepeatedYet) {
  auto cursor = data_empty_.GetRoot();

  auto proto = SamplePackets::TraceEntryWithOneElement().SerializeAsString();
  cursor.SetBytes(AsConstBytes(proto));

  // elements[0].value = 100
  {
    auto value = cursor;
    ASSERT_TRUE(
        value
            .EnterRepeatedFieldByKey(protos::TraceEntry::kElementsFieldNumber,
                                     protos::Element::kIdFieldNumber, 0)
            .IsOk());

    value.EnterField(protos::Element::kValueFieldNumber);
    value.SetScalar(Scalar::VarInt(100));
  }

  // elements[1].value = 101
  {
    auto value = cursor;
    ASSERT_TRUE(
        value
            .EnterRepeatedFieldByKey(protos::TraceEntry::kElementsFieldNumber,
                                     protos::Element::kIdFieldNumber, 1)
            .IsOk());

    value.EnterField(protos::Element::kValueFieldNumber);
    value.SetScalar(Scalar::VarInt(101));
  }

  protos::TraceEntry entry;
  entry.ParseFromString(data_empty_.SerializeAsString());

  ASSERT_EQ(entry.elements_size(), 2);
  ASSERT_EQ(entry.elements(0).value(), 100);
  ASSERT_EQ(entry.elements(1).value(), 101);
}

TEST_F(RwProtoTest, GetScalar_IncompatibleWireType) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  ASSERT_TRUE(cursor.GetScalar().IsAbort());

  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  ASSERT_TRUE(cursor.GetScalar().IsAbort());

  cursor.EnterField(1000);
  ASSERT_TRUE(cursor.GetScalar().IsAbort());
}

TEST_F(RwProtoTest, GetScalar_Success) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  cursor.EnterField(protos::Element::kValueFieldNumber);
  ASSERT_EQ(cursor.GetScalar().value(), Scalar::VarInt(10));
}

TEST_F(RwProtoTest, Delete_RootMessage) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  ASSERT_TRUE(cursor.Delete().IsOk());

  protos::TraceEntry entry;
  entry.ParseFromString(
      data_trace_entry_with_two_elements_.SerializeAsString());

  ASSERT_EQ(entry.elements_size(), 0);
}

TEST_F(RwProtoTest, Delete_Scalar) {
  // delete elements[0].id
  {
    auto cursor = data_trace_entry_with_two_elements_.GetRoot();
    cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
    cursor.EnterField(protos::Element::kIdFieldNumber);
    ASSERT_TRUE(cursor.Delete().IsOk());
  }
  // delete elements[1].value
  {
    auto cursor = data_trace_entry_with_two_elements_.GetRoot();
    cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 1);
    cursor.EnterField(protos::Element::kValueFieldNumber);
    ASSERT_TRUE(cursor.Delete().IsOk());
  }

  protos::TraceEntry entry;
  entry.ParseFromString(
      data_trace_entry_with_two_elements_.SerializeAsString());

  ASSERT_EQ(entry.elements_size(), 2);
  ASSERT_FALSE(entry.elements(0).has_id());
  ASSERT_EQ(entry.elements(0).value(), 10);
  ASSERT_EQ(entry.elements(1).id(), 1);
  ASSERT_FALSE(entry.elements(1).has_value());
}

TEST_F(RwProtoTest, Delete_Message) {
  auto cursor = data_empty_.GetRoot();
  auto proto = SamplePackets::TraceEntryWithOneElement().SerializeAsString();
  cursor.SetBytes(AsConstBytes(proto));

  // field detect elements[0] as simple (not repeated) field
  cursor.EnterField(protos::TraceEntry::kElementsFieldNumber);

  cursor.Delete();

  protos::TraceEntry entry;
  entry.ParseFromString(data_empty_.SerializeAsString());
}

// TODO: deleting an element from an indexed repeated field currently creates a
// "hole" in the array, but ideally we should shift subsequent elements to
// the left. This is low-priority though as the "array delete" operation is not
// required by the current use cases.
TEST_F(RwProtoTest, Delete_IndexedRepeatedField) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  ASSERT_TRUE(cursor.Delete().IsOk());

  protos::TraceEntry entry;
  entry.ParseFromString(
      data_trace_entry_with_two_elements_.SerializeAsString());

  ASSERT_EQ(entry.elements_size(), 1);
  ASSERT_EQ(entry.elements(0).id(), 1);
  ASSERT_EQ(entry.elements(0).value(), 11);
}

TEST_F(RwProtoTest, Delete_MappedRepeatedField) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  cursor.EnterRepeatedFieldByKey(protos::TraceEntry::kElementsFieldNumber,
                                 protos::Element::kIdFieldNumber, 0);
  ASSERT_TRUE(cursor.Delete().IsOk());

  protos::TraceEntry entry;
  entry.ParseFromString(
      data_trace_entry_with_two_elements_.SerializeAsString());
  ASSERT_EQ(entry.elements_size(), 1);
  ASSERT_EQ(entry.elements(0).id(), 1);
  ASSERT_EQ(entry.elements(0).value(), 11);
}

TEST_F(RwProtoTest, Merge_IncompatibleWireType) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  cursor.EnterField(protos::Element::kIdFieldNumber);
  ASSERT_TRUE(cursor.Merge(bytes_empty_).IsAbort());
}

TEST_F(RwProtoTest, Merge_EmptySrc) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  ASSERT_TRUE(cursor.Merge(bytes_empty_).IsOk());
  CheckProtoWithTwoElements(
      data_trace_entry_with_two_elements_.SerializeAsString());
}

TEST_F(RwProtoTest, Merge_EmptyDst) {
  auto cursor = data_empty_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);

  protos::Element element;
  element.set_id(1);
  element.set_value(11);
  auto proto = element.SerializeAsString();
  ASSERT_TRUE(cursor.Merge(AsConstBytes(proto)).IsOk());

  protos::TraceEntry entry;
  entry.ParseFromString(data_empty_.SerializeAsString());
  ASSERT_EQ(entry.elements_size(), 1);
  ASSERT_EQ(entry.elements(0).id(), 1);
  ASSERT_EQ(entry.elements(0).value(), 11);
}

TEST_F(RwProtoTest, Merge_FieldsUnion) {
  auto cursor = data_empty_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);

  // initialize element = {id: 1}
  {
    protos::Element element;
    element.set_id(1);
    auto proto = element.SerializeAsString();
    ASSERT_TRUE(cursor.Merge(AsConstBytes(proto)).IsOk());
  }

  // merge with element = {value: 11}
  {
    protos::Element element;
    element.set_value(11);
    auto proto = element.SerializeAsString();
    ASSERT_TRUE(cursor.Merge(AsConstBytes(proto)).IsOk());
  }

  protos::TraceEntry entry;
  entry.ParseFromString(data_empty_.SerializeAsString());
  ASSERT_EQ(entry.elements_size(), 1);
  ASSERT_EQ(entry.elements(0).id(), 1);
  ASSERT_EQ(entry.elements(0).value(), 11);
}

TEST_F(RwProtoTest, Merge_FieldsReplacement) {
  auto cursor = data_empty_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);

  // initialize element = {id: 0, value: 10}
  {
    protos::Element element;
    element.set_id(0);
    element.set_value(10);
    auto bytes = element.SerializeAsString();
    cursor.SetBytes(AsConstBytes(bytes));
  }

  // merge with element = {id: 1, value: 11}
  {
    protos::Element element;
    element.set_id(1);
    element.set_value(11);
    auto bytes = element.SerializeAsString();
    ASSERT_TRUE(cursor.Merge(AsConstBytes(bytes)).IsOk());
  }

  protos::TraceEntry entry;
  entry.ParseFromString(data_empty_.SerializeAsString());
  ASSERT_EQ(entry.elements_size(), 1);
  ASSERT_EQ(entry.elements(0).id(), 1);
  ASSERT_EQ(entry.elements(0).value(), 11);
}

TEST_F(RwProtoTest, Merge_RepeatedField) {
  auto cursor = data_empty_.GetRoot();

  // initialize elements = [{id: 0, value: 1}]
  {
    protos::Element element;
    element.set_id(0);
    element.set_value(1);
    auto bytes = element.SerializeAsString();

    auto element0 = cursor;
    element0.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
    element0.SetBytes(AsConstBytes(bytes));
  }

  // merge with elements = [{id: 1, value: 10}, {id: 2, value: 20}]
  // (fully replace original elements)
  {
    protos::TraceEntry entry;

    auto* element0 = entry.add_elements();
    element0->set_id(1);
    element0->set_value(10);

    auto* element1 = entry.add_elements();
    element1->set_id(2);
    element1->set_value(20);

    auto bytes = entry.SerializeAsString();
    ASSERT_TRUE(cursor.Merge(AsConstBytes(bytes)).IsOk());
  }

  // check
  {
    protos::TraceEntry entry;
    entry.ParseFromString(data_empty_.SerializeAsString());
    ASSERT_EQ(entry.elements_size(), 2);
    ASSERT_EQ(entry.elements(0).id(), 1);
    ASSERT_EQ(entry.elements(0).value(), 10);
    ASSERT_EQ(entry.elements(1).id(), 2);
    ASSERT_EQ(entry.elements(1).value(), 20);
  }

  // merge with elements = [{id: 0, value: 1}]
  // (fully replace original elements)
  {
    protos::TraceEntry entry;

    auto* element0 = entry.add_elements();
    element0->set_id(0);
    element0->set_value(1);

    auto bytes = entry.SerializeAsString();
    ASSERT_TRUE(cursor.Merge(AsConstBytes(bytes)).IsOk());
  }

  // check
  {
    protos::TraceEntry entry;
    entry.ParseFromString(data_empty_.SerializeAsString());
    ASSERT_EQ(entry.elements_size(), 1);
    ASSERT_EQ(entry.elements(0).id(), 0);
    ASSERT_EQ(entry.elements(0).value(), 1);
  }
}

TEST_F(RwProtoTest, SetBytes_IncompatibleWireType) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  cursor.EnterField(protos::Element::kIdFieldNumber);

  // bytes represent a message, hence replacing a Scalar with bytes means we are
  // changing the proto schema which is a logic error
  ASSERT_TRUE(cursor.SetBytes(protozero::ConstBytes{}).IsAbort());
}

TEST_F(RwProtoTest, SetBytes_CanHandleEmptyPayload) {
  // root = <empty bytes>
  {
    auto cursor = data_empty_.GetRoot();
    cursor.SetBytes(AsConstBytes(""));

    protos::TraceEntry entry;
    auto proto = data_empty_.SerializeAsString();
    entry.ParseFromString(proto);

    ASSERT_EQ(entry.elements_size(), 0);
  }
  // elements[0] = <empty bytes>
  {
    auto cursor = data_empty_.GetRoot();
    cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
    cursor.SetBytes(AsConstBytes(""));

    protos::TraceEntry entry;
    auto proto = data_empty_.SerializeAsString();
    entry.ParseFromString(proto);

    ASSERT_EQ(entry.elements_size(), 1);
    ASSERT_FALSE(entry.elements(0).has_id());
    ASSERT_FALSE(entry.elements(0).has_value());
  }
}

TEST_F(RwProtoTest, SetBytes_InitializesEmptyField) {
  auto cursor = data_empty_.GetRoot();
  auto proto = SamplePackets{}.TraceEntryWithTwoElements().SerializeAsString();
  ASSERT_TRUE(cursor.SetBytes(AsConstBytes(proto)).IsOk());
  CheckProtoWithTwoElements(data_empty_.SerializeAsString());
}

TEST_F(RwProtoTest, SetBytes_UpdatesExistingField) {
  auto root = data_empty_.GetRoot();

  // elements[0]
  {
    auto element = root;
    element.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);

    // id field
    {
      auto id = element;
      id.EnterField(protos::Element::kIdFieldNumber);
      id.SetScalar(Scalar::VarInt(0xdeadbeef));
    }

    // overwrite existing id field
    auto proto = SamplePackets::TraceEntryWithTwoElements()
                     .elements(0)
                     .SerializeAsString();
    element.SetBytes(AsConstBytes(proto));
  }

  // elements[1]
  {
    auto element = root;
    element.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 1);

    // value field
    {
      auto value = element;
      value.EnterField(protos::Element::kValueFieldNumber);
      value.SetScalar(Scalar::VarInt(0xdeadbeef));
    }

    // overwrite existing id field
    auto proto = SamplePackets::TraceEntryWithTwoElements()
                     .elements(1)
                     .SerializeAsString();
    element.SetBytes(AsConstBytes(proto));
  }

  CheckProtoWithTwoElements(data_empty_.SerializeAsString());
}

TEST_F(RwProtoTest, SetScalar_IncompatibleWireType) {
  auto cursor = data_trace_entry_with_two_elements_.GetRoot();
  ASSERT_TRUE(cursor.SetScalar(Scalar::VarInt(0)).IsAbort());

  cursor.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);
  ASSERT_TRUE(cursor.SetScalar(Scalar::VarInt(0)).IsAbort());
}

TEST_F(RwProtoTest, SetScalar_Success) {
  auto cursor = data_empty_.GetRoot();

  auto element0 = cursor;
  element0.EnterRepeatedFieldAt(protos::TraceEntry::kElementsFieldNumber, 0);

  // element[0].value = 10
  auto value = element0;
  value.EnterField(protos::Element::kValueFieldNumber);
  value.SetScalar(Scalar::VarInt(10));

  // element[0].value_fixed32 = 32
  auto value_fixed32 = element0;
  value_fixed32.EnterField(protos::Element::kValueFixed32FieldNumber);
  value_fixed32.SetScalar(Scalar::Fixed32(32));

  // element[0].value_fixed64 = 64
  auto value_fixed64 = element0;
  value_fixed64.EnterField(protos::Element::kValueFixed64FieldNumber);
  value_fixed64.SetScalar(Scalar::Fixed64(64));

  protos::TraceEntry entry;
  entry.ParseFromString(data_empty_.SerializeAsString());

  ASSERT_EQ(entry.elements_size(), 1);
  ASSERT_EQ(entry.elements(0).value(), 10);
  ASSERT_EQ(entry.elements(0).value_fixed32(), static_cast<uint32_t>(32));
  ASSERT_EQ(entry.elements(0).value_fixed64(), static_cast<uint64_t>(64));
}

TEST_F(RwProtoTest, SerializeAsString) {
  CheckProtoWithTwoElements(
      data_trace_entry_with_two_elements_.SerializeAsString());
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
