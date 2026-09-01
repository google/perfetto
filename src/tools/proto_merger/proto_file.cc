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

#include "src/tools/proto_merger/proto_file.h"

#include <algorithm>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/text_format.h>

#include "perfetto/ext/base/string_utils.h"

namespace perfetto {
namespace proto_merger {
namespace {

const char* const
    kTypeToName[google::protobuf::FieldDescriptor::Type::MAX_TYPE + 1] = {
        "ERROR",  // 0 is reserved for errors

        "double",    // TYPE_DOUBLE
        "float",     // TYPE_FLOAT
        "int64",     // TYPE_INT64
        "uint64",    // TYPE_UINT64
        "int32",     // TYPE_INT32
        "fixed64",   // TYPE_FIXED64
        "fixed32",   // TYPE_FIXED32
        "bool",      // TYPE_BOOL
        "string",    // TYPE_STRING
        "group",     // TYPE_GROUP
        "message",   // TYPE_MESSAGE
        "bytes",     // TYPE_BYTES
        "uint32",    // TYPE_UINT32
        "enum",      // TYPE_ENUM
        "sfixed32",  // TYPE_SFIXED32
        "sfixed64",  // TYPE_SFIXED64
        "sint32",    // TYPE_SINT32
        "sint64",    // TYPE_SINT64
};

std::optional<std::string> MinimizeType(const std::string& a,
                                        const std::string& b) {
  auto a_pieces = base::SplitString(a, ".");
  auto b_pieces = base::SplitString(b, ".");

  size_t skip = 0;
  for (size_t i = 0; i < std::min(a_pieces.size(), b_pieces.size()); ++i) {
    if (a_pieces[i] != b_pieces[i])
      return a.substr(skip);
    skip += a_pieces[i].size() + 1;
  }
  return std::nullopt;
}

bool IsExtensionFile(const google::protobuf::FileDescriptor* file,
                     const std::vector<const google::protobuf::FileDescriptor*>&
                         extension_files) {
  return std::find(extension_files.begin(), extension_files.end(), file) !=
         extension_files.end();
}

template <typename DescriptorType>
std::string TypeNameInScope(
    const std::string& base_package,
    const std::string& scope_full_name,
    const google::protobuf::FieldDescriptor& desc,
    const DescriptorType* type,
    bool packageless_type,
    const std::vector<const google::protobuf::FileDescriptor*>&
        extension_files) {
  std::string full_name = std::string(type->full_name());
  if (type->file() != desc.containing_type()->file() &&
      IsExtensionFile(type->file(), extension_files)) {
    std::string pkg(type->file()->package());
    std::string relative_name =
        pkg.empty() ? full_name : base::StripPrefix(full_name, pkg + ".");
    if (packageless_type) {
      return relative_name;
    }
    full_name = base_package + "." + relative_name;
  } else {
    if (packageless_type) {
      return base::StripPrefix(full_name,
                               std::string(type->file()->package()) + ".");
    }
  }
  return MinimizeType(full_name, scope_full_name)
      .value_or(std::string(type->name()));
}

std::string SimpleFieldTypeInScope(
    const std::string& base_package,
    const std::string& scope_full_name,
    const google::protobuf::FieldDescriptor& desc,
    bool packageless_type,
    const std::vector<const google::protobuf::FileDescriptor*>&
        extension_files) {
  switch (desc.type()) {
    case google::protobuf::FieldDescriptor::TYPE_MESSAGE:
      return TypeNameInScope(base_package, scope_full_name, desc,
                             desc.message_type(), packageless_type,
                             extension_files);
    case google::protobuf::FieldDescriptor::TYPE_ENUM:
      return TypeNameInScope(base_package, scope_full_name, desc,
                             desc.enum_type(), packageless_type,
                             extension_files);
    default:
      return kTypeToName[desc.type()];
  }
}

std::string FieldTypeInScope(
    const std::string& base_package,
    const std::string& scope_full_name,
    const google::protobuf::FieldDescriptor& desc,
    bool packageless_type,
    const std::vector<const google::protobuf::FileDescriptor*>&
        extension_files) {
  if (!desc.is_map())
    return SimpleFieldTypeInScope(base_package, scope_full_name, desc,
                                  packageless_type, extension_files);

  std::string field_type;
  field_type += "map<";
  field_type += FieldTypeInScope(base_package, scope_full_name,
                                 *desc.message_type()->field(0),
                                 packageless_type, extension_files);
  field_type += ",";
  field_type += FieldTypeInScope(base_package, scope_full_name,
                                 *desc.message_type()->field(1),
                                 packageless_type, extension_files);
  field_type += ">";
  return field_type;
}

std::unique_ptr<google::protobuf::Message> NormalizeOptionsMessage(
    const google::protobuf::DescriptorPool& pool,
    google::protobuf::DynamicMessageFactory* factory,
    const google::protobuf::Message& message) {
  const auto* option_descriptor =
      pool.FindMessageTypeByName(message.GetDescriptor()->full_name());
  if (!option_descriptor)
    return nullptr;

  std::unique_ptr<google::protobuf::Message> dynamic_options(
      factory->GetPrototype(option_descriptor)->New());
  PERFETTO_CHECK(dynamic_options->ParseFromString(message.SerializeAsString()));
  return dynamic_options;
}

std::vector<ProtoFile::Option> OptionsFromMessage(
    const google::protobuf::DescriptorPool& pool,
    const google::protobuf::Message& raw_message) {
  google::protobuf::DynamicMessageFactory factory;

  auto normalized = NormalizeOptionsMessage(pool, &factory, raw_message);
  const auto* message = normalized ? normalized.get() : &raw_message;
  const auto* reflection = message->GetReflection();

  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(*message, &fields);

  std::vector<ProtoFile::Option> options;
  for (size_t i = 0; i < fields.size(); i++) {
    int count = 1;
    bool repeated = false;
    if (fields[i]->is_repeated()) {
      count = reflection->FieldSize(*message, fields[i]);
      repeated = true;
    }
    for (int j = 0; j < count; j++) {
      std::string name;
      if (fields[i]->is_extension()) {
        name = "(." + std::string(fields[i]->full_name()) + ")";
      } else {
        name = fields[i]->name();
      }

      std::string fieldval;
      if (fields[i]->cpp_type() ==
          google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        std::string tmp;
        google::protobuf::TextFormat::Printer printer;
        printer.PrintFieldValueToString(*message, fields[i], repeated ? j : -1,
                                        &tmp);
        fieldval.append("{\n");
        fieldval.append(tmp);
        fieldval.append("}");
      } else {
        google::protobuf::TextFormat::PrintFieldValueToString(
            *message, fields[i], repeated ? j : -1, &fieldval);
      }
      options.push_back(ProtoFile::Option{name, fieldval});
    }
  }
  return options;
}

template <typename Output, typename Descriptor>
Output InitFromDescriptor(const Descriptor& desc) {
  google::protobuf::SourceLocation source_loc;
  if (!desc.GetSourceLocation(&source_loc))
    return {};

  Output out;
  out.leading_comments = base::SplitString(source_loc.leading_comments, "\n");
  out.trailing_comments = base::SplitString(source_loc.trailing_comments, "\n");
  return out;
}

ProtoFile::Field FieldFromDescriptor(
    const google::protobuf::Descriptor& parent,
    const google::protobuf::FieldDescriptor& desc,
    const std::vector<const google::protobuf::FileDescriptor*>&
        extension_files = {}) {
  auto field = InitFromDescriptor<ProtoFile::Field>(desc);
  field.is_repeated = desc.is_repeated() && !desc.is_map();
  std::string base_package(parent.file()->package());
  field.packageless_type =
      FieldTypeInScope(base_package, std::string(parent.full_name()), desc,
                       true, extension_files);
  field.type = FieldTypeInScope(base_package, std::string(parent.full_name()),
                                desc, false, extension_files);
  field.name = desc.name();
  field.number = desc.number();
  field.options = OptionsFromMessage(*desc.file()->pool(), desc.options());

  // Protobuf editions: replace legacy "packed" option.
  field.options.erase(std::remove_if(field.options.begin(), field.options.end(),
                                     [](const ProtoFile::Option& opt) {
                                       return opt.key == "packed";
                                     }),
                      field.options.end());
  if (desc.is_packed()) {
    field.options.push_back(
        ProtoFile::Option{"features.repeated_field_encoding", "PACKED"});
  }

  return field;
}

ProtoFile::Enum::Value EnumValueFromDescriptor(
    const google::protobuf::EnumValueDescriptor& desc) {
  auto value = InitFromDescriptor<ProtoFile::Enum::Value>(desc);
  value.name = desc.name();
  value.number = desc.number();
  value.options = OptionsFromMessage(*desc.file()->pool(), desc.options());
  return value;
}

ProtoFile::Enum EnumFromDescriptor(
    const google::protobuf::EnumDescriptor& desc) {
  auto en = InitFromDescriptor<ProtoFile::Enum>(desc);
  en.name = desc.name();
  for (int i = 0; i < desc.value_count(); ++i) {
    en.values.emplace_back(EnumValueFromDescriptor(*desc.value(i)));
  }
  return en;
}

ProtoFile::Oneof OneOfFromDescriptor(
    const google::protobuf::Descriptor& parent,
    const google::protobuf::OneofDescriptor& desc,
    const std::vector<const google::protobuf::FileDescriptor*>&
        extension_files) {
  auto oneof = InitFromDescriptor<ProtoFile::Oneof>(desc);
  oneof.name = desc.name();
  for (int i = 0; i < desc.field_count(); ++i) {
    oneof.fields.emplace_back(
        FieldFromDescriptor(parent, *desc.field(i), extension_files));
  }
  return oneof;
}

ProtoFile::Message MessageFromDescriptor(
    const google::protobuf::Descriptor& desc,
    const std::vector<const google::protobuf::FileDescriptor*>&
        extension_files = {}) {
  auto message = InitFromDescriptor<ProtoFile::Message>(desc);
  message.name = desc.name();

  for (int i = 0; i < desc.reserved_range_count(); ++i) {
    const auto* range = desc.reserved_range(i);
    for (int num = range->start; num < range->end; ++num) {
      message.reserved_numbers.insert(num);
    }
  }

  for (int i = 0; i < desc.enum_type_count(); ++i) {
    message.enums.emplace_back(EnumFromDescriptor(*desc.enum_type(i)));
  }
  for (int i = 0; i < desc.nested_type_count(); ++i) {
    if (desc.nested_type(i)->options().map_entry())
      continue;
    message.nested_messages.emplace_back(
        MessageFromDescriptor(*desc.nested_type(i), extension_files));
  }
  for (int i = 0; i < desc.oneof_decl_count(); ++i) {
    message.oneofs.emplace_back(
        OneOfFromDescriptor(desc, *desc.oneof_decl(i), extension_files));
  }
  for (int i = 0; i < desc.field_count(); ++i) {
    auto* field = desc.field(i);
    if (field->containing_oneof())
      continue;
    message.fields.emplace_back(
        FieldFromDescriptor(desc, *field, extension_files));
  }

  // Inlined extension fields that extend this message from the extension files.
  for (const auto* ext_file : extension_files) {
    const auto* ext_pool = ext_file->pool();
    const auto* ext_containing_type =
        ext_pool->FindMessageTypeByName(desc.full_name());
    if (ext_containing_type) {
      std::vector<const google::protobuf::FieldDescriptor*> pool_extensions;
      ext_pool->FindAllExtensions(ext_containing_type, &pool_extensions);
      for (const auto* ext_field : pool_extensions) {
        if (ext_field->file() == ext_file) {
          message.fields.emplace_back(
              FieldFromDescriptor(desc, *ext_field, extension_files));
        }
      }
    }
  }

  return message;
}

template <typename T>
const T* FindByName(const std::vector<T>& items, const std::string& name) {
  for (const auto& item : items) {
    if (item.name == name)
      return &item;
  }
  return nullptr;
}

bool IsMessageEmpty(const ProtoFile::Message& msg) {
  return msg.fields.empty() && msg.enums.empty() &&
         msg.nested_messages.empty() && msg.oneofs.empty();
}

}  // namespace

ProtoFile ProtoFileFromDescriptor(
    std::string preamble,
    const google::protobuf::FileDescriptor& desc,
    const std::vector<const google::protobuf::FileDescriptor*>&
        extension_files) {
  ProtoFile file;
  file.preamble = std::move(preamble);
  file.is_proto2 =
      (file.preamble.find("syntax = \"proto2\"") != std::string::npos);
  for (int i = 0; i < desc.enum_type_count(); ++i) {
    file.enums.push_back(EnumFromDescriptor(*desc.enum_type(i)));
  }
  std::unordered_set<const google::protobuf::FileDescriptor*> relocated_files;
  auto relocate_helpers =
      [&](const google::protobuf::FileDescriptor* ext_file_desc,
          auto& self) -> void {
    if (!ext_file_desc || ext_file_desc == &desc ||
        ext_file_desc->name() == desc.name() ||
        !relocated_files.insert(ext_file_desc).second) {
      return;
    }
    for (int d = 0; d < ext_file_desc->dependency_count(); ++d) {
      const auto* dep = ext_file_desc->dependency(d);
      if (dep != &desc && dep->name() != desc.name() &&
          std::find(extension_files.begin(), extension_files.end(), dep) !=
              extension_files.end()) {
        self(dep, self);
      }
    }
    for (int i = 0; i < ext_file_desc->enum_type_count(); ++i) {
      auto en = EnumFromDescriptor(*ext_file_desc->enum_type(i));
      if (!FindByName(file.enums, en.name)) {
        file.enums.push_back(std::move(en));
      }
    }
    for (int i = 0; i < ext_file_desc->message_type_count(); ++i) {
      auto msg = MessageFromDescriptor(*ext_file_desc->message_type(i),
                                       extension_files);
      if (!IsMessageEmpty(msg) && !FindByName(file.messages, msg.name)) {
        file.messages.push_back(std::move(msg));
      }
    }
  };

  for (int i = 0; i < desc.message_type_count(); ++i) {
    const auto* base_msg_desc = desc.message_type(i);

    // Relocate helper messages and enums from extension files that extend
    // this base message, placing them immediately before the target message.
    for (const auto* ext_file_desc : extension_files) {
      const auto* ext_pool = ext_file_desc->pool();
      const auto* ext_containing_type =
          ext_pool->FindMessageTypeByName(base_msg_desc->full_name());
      if (ext_containing_type) {
        std::vector<const google::protobuf::FieldDescriptor*> pool_extensions;
        ext_pool->FindAllExtensions(ext_containing_type, &pool_extensions);
        for (const auto* ext_field : pool_extensions) {
          if (ext_field->file() == ext_file_desc) {
            relocate_helpers(ext_file_desc, relocate_helpers);
            break;
          }
        }
      }
    }

    file.messages.push_back(
        MessageFromDescriptor(*base_msg_desc, extension_files));
  }

  return file;
}

}  // namespace proto_merger
}  // namespace perfetto
