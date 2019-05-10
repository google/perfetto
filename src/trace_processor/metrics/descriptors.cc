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

#include "src/trace_processor/metrics/descriptors.h"

#include "perfetto/common/descriptor.pbzero.h"

namespace perfetto {
namespace trace_processor {
namespace metrics {

void DescriptorPool::AddNestedProtoDescriptors(
    const std::string& package_name,
    base::Optional<uint32_t> parent_idx,
    const uint8_t* descriptor_proto,
    size_t size) {
  protos::pbzero::DescriptorProto::Decoder decoder(descriptor_proto, size);

  auto parent_name =
      parent_idx ? descriptors_[*parent_idx].full_name() : package_name;
  auto full_name =
      parent_name + "." + base::StringView(decoder.name()).ToStdString();

  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  ProtoDescriptor proto_descriptor(package_name, full_name, parent_idx);
  for (auto it = decoder.field(); it; ++it) {
    FieldDescriptorProto::Decoder f_decoder(it->data(), it->size());
    std::string type_name =
        f_decoder.has_type_name()
            ? base::StringView(f_decoder.type_name()).ToStdString()
            : "";
    FieldDescriptor field(
        base::StringView(f_decoder.name()).ToStdString(),
        static_cast<uint32_t>(f_decoder.number()),
        static_cast<uint32_t>(f_decoder.type()), std::move(type_name),
        f_decoder.label() == FieldDescriptorProto::LABEL_REPEATED);
    proto_descriptor.AddField(std::move(field));
  }
  descriptors_.emplace_back(std::move(proto_descriptor));

  auto idx = static_cast<uint32_t>(descriptors_.size()) - 1;
  for (auto it = decoder.nested_type(); it; ++it) {
    AddNestedProtoDescriptors(package_name, idx, it->data(), it->size());
  }
}

void DescriptorPool::AddFromFileDescriptorSet(
    const uint8_t* file_descriptor_set_proto,
    size_t size) {
  // First pass: extract all the message descriptors from the file and add them
  // to the pool.
  protos::pbzero::FileDescriptorSet::Decoder proto(file_descriptor_set_proto,
                                                   size);
  for (auto it = proto.file(); it; ++it) {
    protos::pbzero::FileDescriptorProto::Decoder file(it->data(), it->size());
    std::string package = "." + base::StringView(file.package()).ToStdString();
    for (auto message_it = file.message_type(); message_it; ++message_it) {
      AddNestedProtoDescriptors(package, base::nullopt, message_it->data(),
                                message_it->size());
    }
  }

  // Second pass: resolve the types of all the fields to the correct indiices.
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  for (auto& descriptor : descriptors_) {
    for (auto& field : *descriptor.mutable_fields()) {
      if (field.type() == FieldDescriptorProto::TYPE_MESSAGE ||
          field.type() == FieldDescriptorProto::TYPE_ENUM) {
        field.set_message_type_idx(
            FindDescriptorIdx(field.raw_type_name()).value());
      }
    }
  }
}

base::Optional<uint32_t> DescriptorPool::FindDescriptorIdx(
    const std::string& full_name) const {
  auto it = std::find_if(descriptors_.begin(), descriptors_.end(),
                         [full_name](const ProtoDescriptor& desc) {
                           return desc.full_name() == full_name;
                         });
  auto idx = static_cast<uint32_t>(std::distance(descriptors_.begin(), it));
  return idx < descriptors_.size() ? base::Optional<uint32_t>(idx)
                                   : base::nullopt;
}

ProtoDescriptor::ProtoDescriptor(std::string package_name,
                                 std::string full_name,
                                 base::Optional<uint32_t> parent_id)
    : package_name_(std::move(package_name)),
      full_name_(std::move(full_name)),
      parent_id_(parent_id) {}

FieldDescriptor::FieldDescriptor(std::string name,
                                 uint32_t number,
                                 uint32_t type,
                                 std::string raw_type_name,
                                 bool is_repeated)
    : name_(std::move(name)),
      number_(number),
      type_(type),
      raw_type_name_(std::move(raw_type_name)),
      is_repeated_(is_repeated) {}

}  // namespace metrics
}  // namespace trace_processor
}  // namespace perfetto
