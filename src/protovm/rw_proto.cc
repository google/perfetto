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

#include "src/protovm/rw_proto.h"

#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/protovm/error_handling.h"

namespace perfetto {
namespace protovm {

RwProto::Cursor::RepeatedFieldIterator::RepeatedFieldIterator()
    : allocator_{nullptr}, it_{} {}

RwProto::Cursor::RepeatedFieldIterator::RepeatedFieldIterator(
    Allocator& allocator,
    IntrusiveMap::Iterator it)
    : allocator_{&allocator}, it_{it} {}

RwProto::Cursor::RepeatedFieldIterator&
RwProto::Cursor::RepeatedFieldIterator::RepeatedFieldIterator::operator++() {
  ++it_;
  return *this;
}

RwProto::Cursor RwProto::Cursor::RepeatedFieldIterator::operator*() {
  return Cursor{it_->value.get(), allocator_};
}

RwProto::Cursor::RepeatedFieldIterator::operator bool() const {
  return static_cast<bool>(it_);
}

RwProto::Cursor::Cursor() = default;

RwProto::Cursor::Cursor(Node* node, Allocator* allocator)
    : node_{node}, allocator_{allocator} {}

StatusOr<bool> RwProto::Cursor::HasField(uint32_t field_id) {
  PERFETTO_DCHECK(node_);

  // Eagerly decompose bytes because the field being tested will be entered
  // later anyways. See Executor::EnterField().
  auto status = ConvertToMessageIfNeeded(*node_);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status);
  auto* message = node_->GetIf<Node::Message>();
  bool found = static_cast<bool>(message->field_id_to_node.Find(field_id));
  return found;
}

StatusOr<void> RwProto::Cursor::EnterField(uint32_t field_id) {
  PERFETTO_DCHECK(node_);

  auto status_or_it = FindOrCreateMessageField(*node_, field_id);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_it);
  auto it = *status_or_it;

  if (it->value->GetIf<Node::IndexedRepeatedField>()) {
    PERFETTO_ABORT(
        "Attempted to enter field (id=%u) as a simple field but it is an "
        "indexed repeated field",
        field_id);
  }

  if (it->value->GetIf<Node::MappedRepeatedField>()) {
    PERFETTO_ABORT(
        "Attempted to enter field (id=%u) as a simple field but it is a "
        "mapped repeated field",
        field_id);
  }

  holding_map_and_node_ = {&node_->GetIf<Node::Message>()->field_id_to_node,
                           std::addressof(*it)};
  node_ = it->value.get();
  return StatusOr<void>::Ok();
}

StatusOr<void> RwProto::Cursor::EnterRepeatedFieldByIndex(uint32_t field_id,
                                                          uint32_t index) {
  PERFETTO_DCHECK(node_);

  auto status_or_message_field = FindOrCreateMessageField(*node_, field_id);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_message_field);
  auto message_field = *status_or_message_field;

  auto status = ConvertToIndexedRepeatedFieldIfNeeded(*message_field->value);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status);

  auto status_or_repeated_field =
      FindOrCreateIndexedRepeatedField(*message_field->value, index);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_repeated_field);

  holding_map_and_node_ = {
      &message_field->value->GetIf<Node::IndexedRepeatedField>()->index_to_node,
      std::addressof(**status_or_repeated_field)};
  node_ = (*status_or_repeated_field)->value.get();

  return StatusOr<void>::Ok();
}

StatusOr<RwProto::Cursor::RepeatedFieldIterator>
RwProto::Cursor::IterateRepeatedField(uint32_t field_id) {
  PERFETTO_DCHECK(node_);

  auto status_convertion_to_message = ConvertToMessageIfNeeded(*node_);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_convertion_to_message);

  auto* message = node_->GetIf<Node::Message>();
  auto it = message->field_id_to_node.Find(field_id);

  if (!it) {
    return RepeatedFieldIterator{};
  }

  auto& field = *it->value;
  auto status_convertion_to_repeated_field =
      ConvertToIndexedRepeatedFieldIfNeeded(field);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_convertion_to_repeated_field);

  return RepeatedFieldIterator{
      *allocator_,
      field.GetIf<Node::IndexedRepeatedField>()->index_to_node.begin()};
}

StatusOr<void> RwProto::Cursor::EnterRepeatedFieldByKey(
    uint32_t field_id,
    uint32_t map_key_field_id,
    uint64_t key) {
  PERFETTO_DCHECK(node_);

  auto status_or_message_field = FindOrCreateMessageField(*node_, field_id);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_message_field);
  auto message_field = *status_or_message_field;

  auto status_conversion = ConvertToMappedRepeatedFieldIfNeeded(
      *message_field->value, map_key_field_id);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_conversion);

  auto status_or_repeated_field =
      FindOrCreateMappedRepeatedField(*message_field->value, key);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_repeated_field);

  holding_map_and_node_ = {
      &message_field->value->GetIf<Node::MappedRepeatedField>()->key_to_node,
      std::addressof(**status_or_repeated_field)};
  node_ = (*status_or_repeated_field)->value.get();

  return StatusOr<void>::Ok();
}

StatusOr<Scalar> RwProto::Cursor::GetScalar() const {
  PERFETTO_DCHECK(node_);

  auto* scalar = node_->GetIf<Scalar>();
  if (!scalar) {
    PERFETTO_ABORT("Attempted \"get scalar\" operation but node has type %s",
                   node_->GetTypeName());
  }
  return *scalar;
}

StatusOr<void> RwProto::Cursor::SetBytes(protozero::ConstBytes data) {
  PERFETTO_DCHECK(node_);

  if (node_->GetIf<Scalar>()) {
    PERFETTO_ABORT(
        "Attempted \"set bytes\" operation but node has Scalar type");
  }

  auto status_or_bytes = allocator_->AllocateAndCopyBytes(data);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_bytes);

  allocator_->DeleteReferencedData(node_);
  node_->value = Node::Bytes{std::move(*status_or_bytes)};

  return StatusOr<void>::Ok();
}

StatusOr<void> RwProto::Cursor::SetScalar(Scalar scalar) {
  PERFETTO_DCHECK(node_);

  if (node_->GetIf<Node::Bytes>() || node_->GetIf<Node::Message>()) {
    PERFETTO_ABORT("Attempted \"set scalar\" operation but node has type %s",
                   node_->GetTypeName());
  }

  node_->value = scalar;
  return StatusOr<void>::Ok();
}

StatusOr<void> RwProto::Cursor::Merge(protozero::ConstBytes data) {
  PERFETTO_DCHECK(node_);

  bool is_compatible_wire_type = node_->GetIf<Node::Empty>() ||
                                 node_->GetIf<Node::Message>() ||
                                 node_->GetIf<Node::Bytes>();
  if (!is_compatible_wire_type) {
    PERFETTO_ABORT("Attempted MERGE operation but node has type %s",
                   node_->GetTypeName());
  }

  if (data.size == 0) {
    return StatusOr<void>::Ok();
  }

  auto status_convertion = ConvertToMessageIfNeeded(*node_);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_convertion);
  auto* message = node_->GetIf<Node::Message>();

  protozero::ProtoDecoder decoder(data);

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    auto status_or_map_value = CreateNodeFromField(field);
    PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_map_value);

    auto it = message->field_id_to_node.Find(field.id());

    if (!it) {
      auto status_or_it = MapInsert(message->field_id_to_node, field.id(),
                                    std::move(*status_or_map_value));
      PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_it);
      continue;
    }

    if (it->value->GetIf<Node::MappedRepeatedField>()) {
      PERFETTO_ABORT(
          "Merge operation of mapped repeated field is not supported (field id "
          "= %u)",
          field.id());
    }

    if (auto* indexed_fields = it->value->GetIf<Node::IndexedRepeatedField>()) {
      // Implements merge semantics for repeated fields: all existing fields are
      // removed and replaced with the newly received fields.
      if (!indexed_fields->has_been_merged) {
        // Optimization opportunity: reuse the existing nodes to avoid N
        // allocation-deallocation pairs, where N is the number of newly
        // received repeated fields.
        allocator_->DeleteReferencedData(it->value.get());
        indexed_fields->has_been_merged = true;
      }
      MapInsert(indexed_fields->index_to_node,
                indexed_fields->index_to_node.Size(),
                std::move(*status_or_map_value));
      continue;
    }

    // Optimization oppurtunity: reuse the existing node to avoid one
    // allocation-deallocation pair
    allocator_->Delete(it->value.release());
    it->value = std::move(*status_or_map_value);
  }

  // Reset the merge state of repeated fields
  for (auto& field : message->field_id_to_node) {
    if (auto* indexed_fields =
            field.value->GetIf<Node::IndexedRepeatedField>()) {
      indexed_fields->has_been_merged = false;
    }
  }

  return StatusOr<void>::Ok();
}

StatusOr<void> RwProto::Cursor::Delete() {
  PERFETTO_DCHECK(node_);

  bool is_root_node = !holding_map_and_node_.first;
  if (is_root_node) {
    node_->value = Node::Empty{};
    return StatusOr<void>::Ok();
  }

  auto [holding_map, map_node] = holding_map_and_node_;
  PERFETTO_DCHECK(holding_map);
  PERFETTO_DCHECK(map_node);
  holding_map->Remove(*map_node);
  allocator_->Delete(&GetOuterNode(*map_node));

  node_ = nullptr;  // Delete operation invalidates cursor

  return StatusOr<void>::Ok();
}

StatusOr<void> RwProto::Cursor::ConvertToMessageIfNeeded(Node& node) {
  if (node.GetIf<Node::Message>()) {
    return StatusOr<void>::Ok();
  }

  if (node.GetIf<Node::Empty>()) {
    node.value = Node::Message{};
    return StatusOr<void>::Ok();
  }

  auto* bytes = node.GetIf<Node::Bytes>();
  if (!bytes) {
    PERFETTO_ABORT("Attempted conversion to message but node has type %s",
                   node.GetTypeName());
  }

  Node::Message message;

  protozero::ProtoDecoder decoder(protozero::ConstBytes{
      static_cast<const uint8_t*>(bytes->data.get()), bytes->size});

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    auto status_or_map_value = CreateNodeFromField(field);
    if (!status_or_map_value.IsOk()) {
      Node node_tmp{message};
      allocator_->DeleteReferencedData(&node_tmp);
      PERFETTO_RETURN(status_or_map_value);
    }

    auto it = message.field_id_to_node.Find(field.id());

    // First occurrence of this field id. Just insert a new field into the
    // map.
    if (!it) {
      auto status_or_it = MapInsert(message.field_id_to_node, field.id(),
                                    std::move(*status_or_map_value));
      if (!status_or_it.IsOk()) {
        Node node_tmp{message};
        allocator_->DeleteReferencedData(&node_tmp);
        PERFETTO_RETURN(status_or_it, "Insert message field (id = %u)",
                        field.id());
      }
      continue;
    }

    // Nth occurrence of this field id:
    // 1. Make sure we have an IndexedRepeatedField node
    // 2. Append into the IndexedRepeatedField's map
    auto status_conversion = ConvertToIndexedRepeatedFieldIfNeeded(*it->value);
    if (!status_conversion.IsOk()) {
      Node node_tmp{message};
      allocator_->DeleteReferencedData(&node_tmp);
      allocator_->Delete(status_or_map_value->release());
      PERFETTO_RETURN(status_conversion);
    }

    auto& index_to_node =
        it->value->GetIf<Node::IndexedRepeatedField>()->index_to_node;
    auto status_or_it = MapInsert(index_to_node, index_to_node.Size(),
                                  std::move(*status_or_map_value));
    if (!status_or_it.IsOk()) {
      Node node_tmp{message};
      allocator_->DeleteReferencedData(&node_tmp);
      PERFETTO_RETURN(status_or_it,
                      "Insert repeated field (id = %u, index = %d)", field.id(),
                      static_cast<int>(index_to_node.Size()));
    }
  }

  allocator_->DeleteReferencedData(&node);
  node.value = message;

  return StatusOr<void>::Ok();
}

StatusOr<UniquePtr<Node>> RwProto::Cursor::CreateNodeFromField(
    protozero::Field field) {
  if (field.type() == protozero::proto_utils::ProtoWireType::kLengthDelimited) {
    auto status_or_bytes = allocator_->AllocateAndCopyBytes(field.as_bytes());
    PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_bytes);
    auto status_or_node =
        allocator_->CreateNode<Node::Bytes>(std::move(*status_or_bytes));
    if (!status_or_node.IsOk()) {
      Node node_tmp{std::move(*status_or_bytes)};
      allocator_->DeleteReferencedData(&node_tmp);
      PERFETTO_RETURN(status_or_node);
    }

    return std::move(*status_or_node);
  }

  auto status_or_node =
      allocator_->CreateNode<Scalar>(field.type(), field.as_uint64());
  if (!status_or_node.IsOk()) {
    PERFETTO_RETURN(status_or_node);
  }

  return std::move(*status_or_node);
}

StatusOr<void> RwProto::Cursor::ConvertToMappedRepeatedFieldIfNeeded(
    Node& node,
    uint32_t map_key_field_id) {
  if (node.GetIf<Node::MappedRepeatedField>()) {
    return StatusOr<void>::Ok();
  }

  if (node.GetIf<Node::Empty>()) {
    node.value = Node::MappedRepeatedField{};
    return StatusOr<void>::Ok();
  }

  if (node.GetIf<Node::Bytes>() || node.GetIf<Node::Message>()) {
    auto status_or_key = ReadScalarField(node, map_key_field_id);
    PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_key);

    auto status_or_map_value = allocator_->CreateNode<Node::Empty>();
    PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_map_value);

    auto map_value = std::move(*status_or_map_value);
    map_value->value = std::move(node.value);
    node.value = Node::MappedRepeatedField{};

    auto status_or_it =
        MapInsert(node.GetIf<Node::MappedRepeatedField>()->key_to_node,
                  *status_or_key, std::move(map_value));
    PERFETTO_RETURN(status_or_it);
  }

  if (auto* indexed = node.GetIf<Node::IndexedRepeatedField>()) {
    IntrusiveMap key_to_node;

    for (auto it = indexed->index_to_node.begin(); it;) {
      auto& map_entry = *it;
      auto& value_node = *it->value;

      it = indexed->index_to_node.Remove(it);

      auto status_or_key = ReadScalarField(value_node, map_key_field_id);
      PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_key);

      map_entry.key = *status_or_key;
      key_to_node.Insert(map_entry);
    }

    node.value = Node::MappedRepeatedField{key_to_node};

    return StatusOr<void>::Ok();
  }

  PERFETTO_ABORT(
      "Attempted to access field as MappedRepeatedField but node has type "
      "%s",
      node.GetTypeName());
}

StatusOr<void> RwProto::Cursor::ConvertToIndexedRepeatedFieldIfNeeded(
    Node& node) {
  if (node.GetIf<Node::IndexedRepeatedField>()) {
    return StatusOr<void>::Ok();
  }

  if (node.GetIf<Node::MappedRepeatedField>()) {
    PERFETTO_ABORT(
        "Attempted \"convert to intexed repeated field\" operation but "
        "node has type %s",
        node.GetTypeName());
  }

  if (node.GetIf<Node::Empty>()) {
    node.value = Node::IndexedRepeatedField{};
    return StatusOr<void>::Ok();
  }

  auto status_or_map_value = allocator_->CreateNode<Node::Empty>();
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_map_value);

  auto map_value = std::move(*status_or_map_value);
  map_value->value = std::move(node.value);
  node.value = Node::IndexedRepeatedField{};

  auto status_or_it =
      MapInsert(node.GetIf<Node::IndexedRepeatedField>()->index_to_node, 0,
                std::move(map_value));
  PERFETTO_RETURN(status_or_it);
}

StatusOr<IntrusiveMap::Iterator> RwProto::Cursor::FindOrCreateMessageField(
    Node& node,
    uint32_t field_id) {
  auto status = ConvertToMessageIfNeeded(node);
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status);

  auto* message = node.GetIf<Node::Message>();
  auto it = message->field_id_to_node.Find(field_id);
  if (it) {
    return it;
  }

  auto status_or_map_value = allocator_->CreateNode<Node::Empty>();
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_map_value);

  auto status_or_it = MapInsert(message->field_id_to_node, field_id,
                                std::move(*status_or_map_value));
  PERFETTO_RETURN(status_or_it);
}

StatusOr<IntrusiveMap::Iterator>
RwProto::Cursor::FindOrCreateIndexedRepeatedField(Node& node, uint32_t index) {
  auto& index_to_node = node.GetIf<Node::IndexedRepeatedField>()->index_to_node;

  auto it = index_to_node.Find(index);
  if (it) {
    return it;
  }

  bool requires_creation_and_is_not_simple_append =
      index > index_to_node.Size();
  if (requires_creation_and_is_not_simple_append) {
    PERFETTO_ABORT(
        "Attempted to insert repeated field at arbitrary position (only "
        "append operation is supported)");
  }

  auto status_or_map_value = allocator_->CreateNode<Node::Empty>();
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_map_value);

  return MapInsert(node.GetIf<Node::IndexedRepeatedField>()->index_to_node,
                   index, std::move(*status_or_map_value));
}

StatusOr<IntrusiveMap::Iterator>
RwProto::Cursor::FindOrCreateMappedRepeatedField(Node& node, uint64_t key) {
  auto it = node.GetIf<Node::MappedRepeatedField>()->key_to_node.Find(key);

  if (it) {
    return it;
  }

  auto status_or_map_value = allocator_->CreateNode<Node::Empty>();
  PERFETTO_RETURN_IF_STATUS_NOT_OK(status_or_map_value);

  return MapInsert(node.GetIf<Node::MappedRepeatedField>()->key_to_node, key,
                   std::move(*status_or_map_value));
}

StatusOr<IntrusiveMap::Iterator> RwProto::Cursor::MapInsert(
    IntrusiveMap& map,
    uint64_t key,
    UniquePtr<Node> map_value) {
  auto status_or_map_node =
      allocator_->CreateNode<Node::MapNode>(key, std::move(map_value));
  if (!status_or_map_node.IsOk()) {
    allocator_->Delete(map_value.release());
    PERFETTO_RETURN(status_or_map_node, "Failed to allocate node");
  }

  auto [it, inserted] =
      map.Insert(*status_or_map_node->release()->GetIf<Node::MapNode>());
  if (!inserted) {
    allocator_->Delete(map_value.release());
    allocator_->Delete(status_or_map_node->release());
    PERFETTO_ABORT(
        "Failed to insert intrusive map entry (key = %d). Duplicated key?",
        static_cast<int>(key));
  }

  return it;
}

StatusOr<uint64_t> RwProto::Cursor::ReadScalarField(Node& node,
                                                    uint32_t field_id) {
  if (auto* bytes = node.GetIf<Node::Bytes>()) {
    protozero::ProtoDecoder decoder(protozero::ConstBytes{
        static_cast<const uint8_t*>(bytes->data.get()), bytes->size});

    auto field = decoder.ReadField();
    while (field.valid() && field.id() != field_id) {
      field = decoder.ReadField();
    }

    if (!field.valid()) {
      PERFETTO_ABORT(
          "Attempted to read scalar field (id=%u) but it is not present",
          field_id);
    }

    if (field.type() ==
        protozero::proto_utils::ProtoWireType::kLengthDelimited) {
      PERFETTO_ABORT("Attempted to length-delimited field (id=%u) as scalar",
                     field_id);
    }

    return field.as_uint64();
  }

  if (auto* message = node.GetIf<Node::Message>()) {
    auto it = message->field_id_to_node.Find(field_id);
    if (!it) {
      PERFETTO_ABORT(
          "Attempted to read scalar field (id=%u) but it is not present",
          field_id);
    }

    auto* scalar = it->value->GetIf<Scalar>();
    if (!scalar) {
      PERFETTO_ABORT(
          "Attempted to read scalar field (id=%u) from node with type %s",
          field_id, it->value->GetTypeName());
    }

    return scalar->value;
  }

  PERFETTO_ABORT(
      "Attempted to read scalar field (id=%u) but parent node has type %s",
      field_id, node.GetTypeName());
}

RwProto::RwProto(Allocator& allocator)
    : allocator_{&allocator}, root_{Node::Empty{}} {}

RwProto::~RwProto() {
  allocator_->DeleteReferencedData(&root_);
}

RwProto::Cursor RwProto::Root() {
  return Cursor{&root_, allocator_};
}

std::string RwProto::SerializeAsString() const {
  if (root_.GetIf<Node::Empty>()) {
    return "";
  }

  if (auto* bytes = root_.GetIf<Node::Bytes>()) {
    return std::string(static_cast<char*>(bytes->data.get()), bytes->size);
  }

  protozero::HeapBuffered<protozero::Message> proto;
  auto* message = root_.GetIf<Node::Message>();
  PERFETTO_DCHECK(message);

  for (auto it = message->field_id_to_node.begin(); it; ++it) {
    auto field_id = static_cast<uint32_t>(it->key);
    SerializeField(field_id, *it->value, *proto.get());
  }

  return proto.SerializeAsString();
}

void RwProto::SerializeField(uint32_t field_id,
                             Node& node,
                             protozero::Message& proto) const {
  if (node.GetIf<Node::Empty>()) {
    return;
  }

  if (auto* bytes = node.GetIf<Node::Bytes>()) {
    proto.AppendBytes(field_id, bytes->data.get(), bytes->size);
    return;
  }

  if (auto* scalar = node.GetIf<Scalar>()) {
    if (scalar->wire_type == protozero::proto_utils::ProtoWireType::kFixed32) {
      proto.AppendFixed(field_id, static_cast<uint32_t>(scalar->value));
      return;
    }

    if (scalar->wire_type == protozero::proto_utils::ProtoWireType::kFixed64) {
      proto.AppendFixed(field_id, static_cast<uint64_t>(scalar->value));
      return;
    }

    proto.AppendVarInt(field_id, scalar->value);
    return;
  }

  if (auto* message = node.GetIf<Node::Message>()) {
    auto* message_proto =
        proto.BeginNestedMessage<protozero::Message>(field_id);

    for (auto it = message->field_id_to_node.begin(); it; ++it) {
      auto message_field_id = static_cast<uint32_t>(it->key);
      SerializeField(message_field_id, *it->value, *message_proto);
    }
  }

  if (node.GetIf<Node::IndexedRepeatedField>() ||
      node.GetIf<Node::MappedRepeatedField>()) {
    IntrusiveMap* map;
    if (auto* indexed = node.GetIf<Node::IndexedRepeatedField>()) {
      map = &indexed->index_to_node;
    } else {
      map = &node.GetIf<Node::MappedRepeatedField>()->key_to_node;
    }

    for (auto it = map->begin(); it; ++it) {
      SerializeField(field_id, *it->value, proto);
    }
  }
}

}  // namespace protovm
}  // namespace perfetto
