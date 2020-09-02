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

#include "src/trace_processor/util/descriptors.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto {
namespace trace_processor {

FieldDescriptor CreateFieldFromDecoder(
    const protos::pbzero::FieldDescriptorProto::Decoder& f_decoder,
    bool is_extension) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  std::string type_name =
      f_decoder.has_type_name()
          ? base::StringView(f_decoder.type_name()).ToStdString()
          : "";
  // TODO(lalitm): add support for enums here.
  uint32_t type =
      f_decoder.has_type()
          ? static_cast<uint32_t>(f_decoder.type())
          : static_cast<uint32_t>(FieldDescriptorProto::TYPE_MESSAGE);
  return FieldDescriptor(
      base::StringView(f_decoder.name()).ToStdString(),
      static_cast<uint32_t>(f_decoder.number()), type, std::move(type_name),
      f_decoder.label() == FieldDescriptorProto::LABEL_REPEATED, is_extension);
}

base::Optional<uint32_t> DescriptorPool::ResolveShortType(
    const std::string& parent_path,
    const std::string& short_type) {
  PERFETTO_DCHECK(!short_type.empty());

  std::string search_path = short_type[0] == '.'
                                ? parent_path + short_type
                                : parent_path + '.' + short_type;
  auto opt_idx = FindDescriptorIdx(search_path);
  if (opt_idx)
    return opt_idx;

  if (parent_path.empty())
    return base::nullopt;

  auto parent_dot_idx = parent_path.rfind('.');
  auto parent_substr = parent_dot_idx == std::string::npos
                           ? ""
                           : parent_path.substr(0, parent_dot_idx);
  return ResolveShortType(parent_substr, short_type);
}

util::Status DescriptorPool::AddExtensionField(
    const std::string& package_name,
    protozero::ConstBytes field_desc_proto) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  FieldDescriptorProto::Decoder f_decoder(field_desc_proto);
  auto field = CreateFieldFromDecoder(f_decoder, true);

  auto extendee_name = base::StringView(f_decoder.extendee()).ToStdString();
  PERFETTO_CHECK(!extendee_name.empty());
  if (extendee_name[0] != '.') {
    // Only prepend if the extendee is not fully qualified
    extendee_name = package_name + "." + extendee_name;
  }
  auto extendee = FindDescriptorIdx(extendee_name);
  if (!extendee.has_value()) {
    return util::ErrStatus("Extendee does not exist %s", extendee_name.c_str());
  }
  descriptors_[extendee.value()].AddField(field);
  return util::OkStatus();
}

void DescriptorPool::CheckPreviousDefinition(
    const std::string& file_name,
    const std::string& descriptor_name) {
  auto prev_idx = FindDescriptorIdx(descriptor_name);
  if (prev_idx.has_value()) {
    auto prev_file = descriptors_[*prev_idx].file_name();
    // We should already make sure we process each file once, so if we're
    // hitting this path, it means the same message was defined in multiple
    // files.
    PERFETTO_FATAL("%s: %s was already defined in file %s", file_name.c_str(),
                   descriptor_name.c_str(), prev_file.c_str());
  }
}

void DescriptorPool::AddNestedProtoDescriptors(
    const std::string& file_name,
    const std::string& package_name,
    base::Optional<uint32_t> parent_idx,
    protozero::ConstBytes descriptor_proto,
    std::vector<ExtensionInfo>* extensions) {
  protos::pbzero::DescriptorProto::Decoder decoder(descriptor_proto);

  auto parent_name =
      parent_idx ? descriptors_[*parent_idx].full_name() : package_name;
  auto full_name =
      parent_name + "." + base::StringView(decoder.name()).ToStdString();

  CheckPreviousDefinition(file_name, full_name);

  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  ProtoDescriptor proto_descriptor(file_name, package_name, full_name,
                                   ProtoDescriptor::Type::kMessage, parent_idx);
  for (auto it = decoder.field(); it; ++it) {
    FieldDescriptorProto::Decoder f_decoder(*it);
    proto_descriptor.AddField(CreateFieldFromDecoder(f_decoder, false));
  }
  descriptors_.emplace_back(std::move(proto_descriptor));

  auto idx = static_cast<uint32_t>(descriptors_.size()) - 1;
  for (auto it = decoder.enum_type(); it; ++it) {
    AddEnumProtoDescriptors(file_name, package_name, idx, *it);
  }
  for (auto it = decoder.nested_type(); it; ++it) {
    AddNestedProtoDescriptors(file_name, package_name, idx, *it, extensions);
  }
  for (auto ext_it = decoder.extension(); ext_it; ++ext_it) {
    extensions->emplace_back(package_name, *ext_it);
  }
}

void DescriptorPool::AddEnumProtoDescriptors(
    const std::string& file_name,
    const std::string& package_name,
    base::Optional<uint32_t> parent_idx,
    protozero::ConstBytes descriptor_proto) {
  protos::pbzero::EnumDescriptorProto::Decoder decoder(descriptor_proto);

  auto parent_name =
      parent_idx ? descriptors_[*parent_idx].full_name() : package_name;
  auto full_name =
      parent_name + "." + base::StringView(decoder.name()).ToStdString();

  CheckPreviousDefinition(file_name, full_name);

  ProtoDescriptor proto_descriptor(file_name, package_name, full_name,
                                   ProtoDescriptor::Type::kEnum, base::nullopt);
  for (auto it = decoder.value(); it; ++it) {
    protos::pbzero::EnumValueDescriptorProto::Decoder enum_value(it->data(),
                                                                 it->size());
    proto_descriptor.AddEnumValue(enum_value.number(),
                                  enum_value.name().ToStdString());
  }
  descriptors_.emplace_back(std::move(proto_descriptor));
}

util::Status DescriptorPool::AddFromFileDescriptorSet(
    const uint8_t* file_descriptor_set_proto,
    size_t size) {
  // First pass: extract all the message descriptors from the file and add them
  // to the pool.
  protos::pbzero::FileDescriptorSet::Decoder proto(file_descriptor_set_proto,
                                                   size);
  std::vector<ExtensionInfo> extensions;
  for (auto it = proto.file(); it; ++it) {
    protos::pbzero::FileDescriptorProto::Decoder file(*it);
    std::string file_name = file.name().ToStdString();
    if (processed_files_.find(file_name) != processed_files_.end()) {
      // This file has been loaded once already. Skip.
      continue;
    }
    processed_files_.insert(file_name);
    std::string package = "." + base::StringView(file.package()).ToStdString();
    for (auto message_it = file.message_type(); message_it; ++message_it) {
      AddNestedProtoDescriptors(file_name, package, base::nullopt, *message_it,
                                &extensions);
    }
    for (auto enum_it = file.enum_type(); enum_it; ++enum_it) {
      AddEnumProtoDescriptors(file_name, package, base::nullopt, *enum_it);
    }
    for (auto ext_it = file.extension(); ext_it; ++ext_it) {
      extensions.emplace_back(package, *ext_it);
    }
  }

  // Second pass: Add extension fields to the real protos.
  for (auto extension : extensions) {
    auto status = AddExtensionField(extension.first, extension.second);
    if (!status.ok())
      return status;
  }

  // Third pass: resolve the types of all the fields to the correct indiices.
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  for (auto& descriptor : descriptors_) {
    for (auto& field : *descriptor.mutable_fields()) {
      if (!field.resolved_type_name().empty())
        continue;

      if (field.type() == FieldDescriptorProto::TYPE_MESSAGE ||
          field.type() == FieldDescriptorProto::TYPE_ENUM) {
        auto opt_desc =
            ResolveShortType(descriptor.full_name(), field.raw_type_name());
        if (!opt_desc.has_value()) {
          return util::ErrStatus(
              "Unable to find short type %s in field inside message %s",
              field.raw_type_name().c_str(), descriptor.full_name().c_str());
        }
        field.set_resolved_type_name(
            descriptors_[opt_desc.value()].full_name());
      }
    }
  }
  return util::OkStatus();
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

std::vector<uint8_t> DescriptorPool::SerializeAsDescriptorSet() {
  protozero::HeapBuffered<protos::pbzero::DescriptorSet> descs;
  for (auto& desc : descriptors()) {
    protos::pbzero::DescriptorProto* proto_descriptor =
        descs->add_descriptors();
    proto_descriptor->set_name(desc.full_name());
    for (auto& field : desc.fields()) {
      protos::pbzero::FieldDescriptorProto* field_descriptor =
          proto_descriptor->add_field();
      field_descriptor->set_name(field.name());
      field_descriptor->set_number(static_cast<int32_t>(field.number()));
      // We do not support required fields. They will show up as optional
      // after serialization.
      field_descriptor->set_label(
          field.is_repeated()
              ? protos::pbzero::FieldDescriptorProto::LABEL_REPEATED
              : protos::pbzero::FieldDescriptorProto::LABEL_OPTIONAL);
      field_descriptor->set_type_name(field.resolved_type_name());
      field_descriptor->set_type(
          static_cast<protos::pbzero::FieldDescriptorProto_Type>(field.type()));
    }
  }
  return descs.SerializeAsArray();
}

ProtoDescriptor::ProtoDescriptor(std::string file_name,
                                 std::string package_name,
                                 std::string full_name,
                                 Type type,
                                 base::Optional<uint32_t> parent_id)
    : file_name_(std::move(file_name)),
      package_name_(std::move(package_name)),
      full_name_(std::move(full_name)),
      type_(type),
      parent_id_(parent_id) {}

FieldDescriptor::FieldDescriptor(std::string name,
                                 uint32_t number,
                                 uint32_t type,
                                 std::string raw_type_name,
                                 bool is_repeated,
                                 bool is_extension)
    : name_(std::move(name)),
      number_(number),
      type_(type),
      raw_type_name_(std::move(raw_type_name)),
      is_repeated_(is_repeated),
      is_extension_(is_extension) {}

}  // namespace trace_processor
}  // namespace perfetto
