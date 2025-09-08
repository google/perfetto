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

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/message.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"

namespace perfetto::trace_processor {
namespace {
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
  protos::pbzero::FieldOptions::Decoder opt(f_decoder.options());
  std::optional<std::string> default_value;
  if (f_decoder.has_default_value()) {
    default_value = f_decoder.default_value().ToStdString();
  }
  return {
      base::StringView(f_decoder.name()).ToStdString(),
      static_cast<uint32_t>(f_decoder.number()),
      type,
      std::move(type_name),
      std::vector<uint8_t>(f_decoder.options().data,
                           f_decoder.options().data + f_decoder.options().size),
      default_value,
      f_decoder.label() == FieldDescriptorProto::LABEL_REPEATED,
      opt.packed(),
      is_extension,
  };
}

base::Status CheckExtensionField(const ProtoDescriptor& proto_descriptor,
                                 const FieldDescriptor& field) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  const auto* existing_field = proto_descriptor.FindFieldByTag(field.number());
  if (existing_field) {
    if (field.type() != existing_field->type()) {
      return base::ErrStatus("Field %s is re-introduced with different type",
                             field.name().c_str());
    }
    if ((field.type() == FieldDescriptorProto::TYPE_MESSAGE ||
         field.type() == FieldDescriptorProto::TYPE_ENUM) &&
        field.raw_type_name() != existing_field->raw_type_name()) {
      return base::ErrStatus(
          "Field %s is re-introduced with different type %s (was %s)",
          field.name().c_str(), field.raw_type_name().c_str(),
          existing_field->raw_type_name().c_str());
    }
  }
  return base::OkStatus();
}

static void PrintStatusError(const std::string& context,
                             const base::Status& status) {
  if (!status.ok()) {
    std::cerr << "  ERROR in " << context << ": " << status.message()
              << std::endl;
  }
}

}  // namespace

std::optional<uint32_t> DescriptorPool::FindDescriptorIdx(
    const std::string& full_name) const {
  auto it = full_name_to_descriptor_index_.find(full_name);
  if (it != full_name_to_descriptor_index_.end()) {
    return it->second;
  }
  return std::nullopt;
}

// Resolves a potentially short type name to a descriptor index.
std::optional<uint32_t> DescriptorPool::ResolveShortType(
    const std::string& parent_path,
    const std::string& short_type) {
  PERFETTO_DCHECK(!short_type.empty());

  if (short_type[0] == '.') {
    return FindDescriptorIdx(short_type);
  }

  auto dot_idx = short_type.rfind('.');
  if (dot_idx != std::string::npos) {
    auto opt_idx = FindDescriptorIdx(short_type);
    if (opt_idx) {
      return opt_idx;
    }
  }

  std::string current_path = parent_path;
  if (!current_path.empty() && current_path[0] != '.') {
    current_path = "." + current_path;
  }

  while (true) {
    std::string search_path =
        current_path + (current_path == "." ? "" : ".") + short_type;
    if (current_path.empty()) {
      search_path = "." + short_type;
    }

    auto opt_idx = FindDescriptorIdx(search_path);
    if (opt_idx)
      return opt_idx;

    if (current_path.empty() || current_path == ".")
      break;

    auto parent_dot_idx = current_path.rfind('.');
    if (parent_dot_idx == std::string::npos || parent_dot_idx == 0) {
      current_path = "";
    } else {
      current_path = current_path.substr(0, parent_dot_idx);
    }
  }
  return std::nullopt;
}

base::Status DescriptorPool::AddExtensionField(
    const std::string& package_name,
    protozero::ConstBytes field_desc_proto) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  FieldDescriptorProto::Decoder f_decoder(field_desc_proto);
  auto field = CreateFieldFromDecoder(f_decoder, true);

  std::string extendee_name = f_decoder.extendee().ToStdString();
  if (extendee_name.empty()) {
    return base::ErrStatus("Extendee name is empty");
  }

  std::optional<uint32_t> extendee =
      ResolveShortType(package_name, extendee_name);
  if (!extendee.has_value()) {
    // Fallback: Try just the extendee name if it looked fully qualified.
    if (extendee_name[0] == '.') {
      extendee = FindDescriptorIdx(extendee_name);
    }
    if (!extendee.has_value()) {
      return base::ErrStatus(
          "Extendee does not exist %s (resolved from %s in package %s)",
          extendee_name.c_str(), f_decoder.extendee().ToStdString().c_str(),
          package_name.c_str());
    }
  }

  ProtoDescriptor& extendee_desc = descriptors_[extendee.value()];
  RETURN_IF_ERROR(CheckExtensionField(extendee_desc, field));
  extendee_desc.AddField(field);
  return base::OkStatus();
}

base::Status DescriptorPool::AddNestedProtoDescriptors(
    const std::string& file_name,
    const std::string& package_name,
    std::optional<uint32_t> parent_idx,
    protozero::ConstBytes descriptor_proto,
    std::vector<ExtensionInfo>* extensions,
    bool merge_existing_messages) {
  protos::pbzero::DescriptorProto::Decoder decoder(descriptor_proto);

  auto parent_name =
      parent_idx ? descriptors_[*parent_idx].full_name() : package_name;

  std::string name = base::StringView(decoder.name()).ToStdString();
  auto full_name = (parent_name == "." || parent_name.empty())
                       ? parent_name + name
                       : parent_name + "." + name;

  auto idx = FindDescriptorIdx(full_name);
  if (idx.has_value() && !merge_existing_messages) {
    return base::OkStatus();  // Skip redefinition
  }
  if (!idx.has_value()) {
    ProtoDescriptor proto_descriptor(file_name, package_name, full_name,
                                     ProtoDescriptor::Type::kMessage,
                                     parent_idx);
    idx = AddProtoDescriptor(std::move(proto_descriptor));
  }
  ProtoDescriptor& proto_descriptor = descriptors_[*idx];
  if (proto_descriptor.type() != ProtoDescriptor::Type::kMessage) {
    return base::ErrStatus("%s was enum, redefined as message",
                           full_name.c_str());
  }

  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  for (auto it = decoder.field(); it; ++it) {
    FieldDescriptorProto::Decoder f_decoder(*it);
    auto field = CreateFieldFromDecoder(f_decoder, /*is_extension=*/false);
    RETURN_IF_ERROR(CheckExtensionField(proto_descriptor, field));
    proto_descriptor.AddField(std::move(field));
  }

  for (auto it = decoder.enum_type(); it; ++it) {
    RETURN_IF_ERROR(AddEnumProtoDescriptors(file_name, full_name, idx, *it,
                                            merge_existing_messages));
  }
  for (auto it = decoder.nested_type(); it; ++it) {
    RETURN_IF_ERROR(AddNestedProtoDescriptors(
        file_name, full_name, idx, *it, extensions, merge_existing_messages));
  }
  for (auto ext_it = decoder.extension(); ext_it; ++ext_it) {
    extensions->emplace_back(
        full_name, *ext_it);  // Use full_name of the message as the scope
  }
  return base::OkStatus();
}

base::Status DescriptorPool::AddEnumProtoDescriptors(
    const std::string& file_name,
    const std::string& package_name,
    std::optional<uint32_t> parent_idx,
    protozero::ConstBytes descriptor_proto,
    bool merge_existing_messages) {
  protos::pbzero::EnumDescriptorProto::Decoder decoder(descriptor_proto);

  auto parent_name =
      parent_idx ? descriptors_[*parent_idx].full_name() : package_name;

  std::string name = base::StringView(decoder.name()).ToStdString();
  auto full_name = (parent_name == "." || parent_name.empty())
                       ? parent_name + name
                       : parent_name + "." + name;

  auto prev_idx = FindDescriptorIdx(full_name);
  if (prev_idx.has_value() && !merge_existing_messages) {
    return base::OkStatus();  // Skip redefinition
  }
  if (!prev_idx.has_value()) {
    ProtoDescriptor proto_descriptor(file_name, package_name, full_name,
                                     ProtoDescriptor::Type::kEnum,
                                     parent_idx);  // Pass parent_idx
    prev_idx = AddProtoDescriptor(std::move(proto_descriptor));
  }
  ProtoDescriptor& proto_descriptor = descriptors_[*prev_idx];
  if (proto_descriptor.type() != ProtoDescriptor::Type::kEnum) {
    return base::ErrStatus("%s was message, redefined as enum",
                           full_name.c_str());
  }

  for (auto it = decoder.value(); it; ++it) {
    protos::pbzero::EnumValueDescriptorProto::Decoder enum_value(it->data(),
                                                                 it->size());
    proto_descriptor.AddEnumValue(enum_value.number(),
                                  enum_value.name().ToStdString());
  }

  return base::OkStatus();
}

base::Status DescriptorPool::AddFromFileDescriptorSet(
    const uint8_t* file_descriptor_set_proto,
    size_t size,
    const std::vector<std::string>& skip_prefixes,
    bool merge_existing_messages) {
  protos::pbzero::FileDescriptorSet::Decoder proto(file_descriptor_set_proto,
                                                   size);
  if (size > 0 && file_descriptor_set_proto == nullptr) {
    std::cerr << "--- ERROR: FileDescriptorSet data is null ---" << std::endl;
    return base::ErrStatus("Invalid FileDescriptorSet data: null");
  }
  if (size == 0) {
    std::cout << "--- INFO: Empty FileDescriptorSet data ---" << std::endl;
    return base::OkStatus();
  }

  std::vector<ExtensionInfo> extensions;
  std::cout << "--- Starting AddFromFileDescriptorSet ---" << std::endl;

  bool has_files = false;
  for (auto it = proto.file(); it; ++it) {
    has_files = true;
    protos::pbzero::FileDescriptorProto::Decoder file(*it);
    const std::string file_name = file.name().ToStdString();
    std::cout << "Processing file: " << file_name << std::endl;

    if (base::StartsWithAny(file_name, skip_prefixes)) {
      std::cout << "  INFO: Skipping '" << file_name
                << "' due to skip_prefixes." << std::endl;
      continue;
    }

    bool already_processed = processed_files_.count(file_name);
    if (!merge_existing_messages && already_processed) {
      std::cout
          << "  INFO: Skipping '" << file_name
          << "' as it's already processed and merge_existing_messages is false."
          << std::endl;
      continue;
    }

    processed_files_.insert(file_name);
    std::string package = "." + base::StringView(file.package()).ToStdString();

    for (auto message_it = file.message_type(); message_it; ++message_it) {
      base::Status status = AddNestedProtoDescriptors(
          file_name, package, std::nullopt, *message_it, &extensions,
          merge_existing_messages);
      PrintStatusError("AddNestedProtoDescriptors for file " + file_name,
                       status);
      RETURN_IF_ERROR(status);
    }

    for (auto enum_it = file.enum_type(); enum_it; ++enum_it) {
      base::Status status = AddEnumProtoDescriptors(
          file_name, package, std::nullopt, *enum_it, merge_existing_messages);
      PrintStatusError("AddEnumProtoDescriptors for file " + file_name, status);
      RETURN_IF_ERROR(status);
    }

    for (auto ext_it = file.extension(); ext_it; ++ext_it) {
      extensions.emplace_back(package, *ext_it);
    }
  }

  if (!has_files) {
    std::cout << "--- INFO: No files found in this FileDescriptorSet ---"
              << std::endl;
  }

  if (!extensions.empty()) {
    std::cout << "--- Second Pass: Adding " << extensions.size()
              << " Extensions ---" << std::endl;
    for (const auto& extension : extensions) {
      base::Status status =
          AddExtensionField(extension.first, extension.second);
      PrintStatusError("AddExtensionField", status);
      RETURN_IF_ERROR(status);
    }
  }

  // Debug logs
  std::cout << "--- Before Third Pass: Checking for Target Type ---"
            << std::endl;
  const std::string target_type =
      ".logs.proto.wireless.android.stats.platform.westworld.battery."
      "RawBatteryGaugeStatsReported";
  if (full_name_to_descriptor_index_.count(target_type)) {
    std::cout << "    SUCCESS: Target type '" << target_type
              << "' IS in the map." << std::endl;
  } else {
    std::cout << "    FAILURE: Target type '" << target_type
              << "' IS NOT in the map." << std::endl;
  }

  std::cout << "--- Third Pass: Resolving Field Types ---" << std::endl;
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  for (ProtoDescriptor& descriptor : descriptors_) {
    for (auto& entry : *descriptor.mutable_fields()) {
      uint32_t field_id = entry.first;
      FieldDescriptor& field = entry.second;
      bool needs_resolution =
          field.resolved_type_name().empty() &&
          (field.type() == FieldDescriptorProto::TYPE_MESSAGE ||
           field.type() == FieldDescriptorProto::TYPE_ENUM);

      if (needs_resolution) {
        auto opt_desc =
            ResolveShortType(descriptor.package_name(), field.raw_type_name());
        if (!opt_desc) {
          opt_desc =
              ResolveShortType(descriptor.full_name(), field.raw_type_name());
        }

        if (!opt_desc.has_value()) {
          std::cerr << "         ERROR: Unable to resolve short type '"
                    << field.raw_type_name() << "' for field '" << field.name()
                    << "' (ID: " << field_id << ") in message '"
                    << descriptor.full_name() << "'" << std::endl;
          return base::ErrStatus(
              "Unable to find short type %s in field '%s' (ID: %u) inside "
              "message %s",
              field.raw_type_name().c_str(), field.name().c_str(), field_id,
              descriptor.full_name().c_str());
        }
        field.set_resolved_type_name(
            descriptors_[opt_desc.value()].full_name());
      }
    }
  }
  std::cout << "--- Finished AddFromFileDescriptorSet ---" << std::endl;
  return base::OkStatus();
}

std::vector<uint8_t> DescriptorPool::SerializeAsDescriptorSet() const {
  protozero::HeapBuffered<protos::pbzero::DescriptorSet> descs;
  for (const auto& desc : descriptors()) {
    protos::pbzero::DescriptorProto* proto_descriptor =
        descs->add_descriptors();
    proto_descriptor->set_name(desc.full_name());
    for (const auto& entry : desc.fields()) {
      const auto& field = entry.second;
      protos::pbzero::FieldDescriptorProto* field_descriptor =
          proto_descriptor->add_field();
      field_descriptor->set_name(field.name());
      field_descriptor->set_number(static_cast<int32_t>(field.number()));
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

uint32_t DescriptorPool::AddProtoDescriptor(ProtoDescriptor descriptor) {
  uint32_t idx = static_cast<uint32_t>(descriptors_.size());
  full_name_to_descriptor_index_[descriptor.full_name()] = idx;
  descriptors_.emplace_back(std::move(descriptor));
  return idx;
}

ProtoDescriptor::ProtoDescriptor(std::string file_name,
                                 std::string package_name,
                                 std::string full_name,
                                 Type type,
                                 std::optional<uint32_t> parent_id)
    : file_name_(std::move(file_name)),
      package_name_(std::move(package_name)),
      full_name_(std::move(full_name)),
      type_(type),
      parent_id_(parent_id) {}

FieldDescriptor::FieldDescriptor(std::string name,
                                 uint32_t number,
                                 uint32_t type,
                                 std::string raw_type_name,
                                 std::vector<uint8_t> options,
                                 std::optional<std::string> default_value,
                                 bool is_repeated,
                                 bool is_packed,
                                 bool is_extension)
    : name_(std::move(name)),
      number_(number),
      type_(type),
      raw_type_name_(std::move(raw_type_name)),
      options_(std::move(options)),
      default_value_(std::move(default_value)),
      is_repeated_(is_repeated),
      is_packed_(is_packed),
      is_extension_(is_extension) {}

}  // namespace perfetto::trace_processor
