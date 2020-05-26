/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <google/protobuf/message.h>

#include "perfetto/base/logging.h"
#include "src/trace_processor/proto_to_json.h"

namespace perfetto {
namespace trace_processor {
namespace proto_to_json {

namespace {

std::string EscapeJsonString(const std::string& raw) {
  std::string ret;
  for (auto it = raw.cbegin(); it != raw.cend(); it++) {
    switch (*it) {
      case '\\':
        ret += "\\\\";
        break;
      case '"':
        ret += "\\\"";
        break;
      case '/':
        ret += "\\/";
        break;
      case '\b':
        ret += "\\b";
        break;
      case '\f':
        ret += "\\f";
        break;
      case '\n':
        ret += "\\n";
        break;
      case '\r':
        ret += "\\r";
        break;
      case '\t':
        ret += "\\t";
        break;
      default:
        ret += *it;
        break;
    }
  }
  return '"' + ret + '"';
}

std::string FieldToJson(const google::protobuf::Message& message,
                        const google::protobuf::FieldDescriptor* field_desc,
                        int idx,
                        uint32_t indent) {
  using google::protobuf::FieldDescriptor;

  const google::protobuf::Reflection* ref = message.GetReflection();
  bool is_repeated = field_desc->is_repeated();
  switch (field_desc->cpp_type()) {
    case FieldDescriptor::CppType::CPPTYPE_BOOL:
      return std::to_string(is_repeated
                                ? ref->GetRepeatedBool(message, field_desc, idx)
                                : ref->GetBool(message, field_desc));
    case FieldDescriptor::CppType::CPPTYPE_ENUM:
      return EscapeJsonString(
          is_repeated ? ref->GetRepeatedEnum(message, field_desc, idx)->name()
                      : ref->GetEnum(message, field_desc)->name());
    case FieldDescriptor::CppType::CPPTYPE_FLOAT:
      return std::to_string(
          is_repeated
              ? static_cast<double>(
                    ref->GetRepeatedFloat(message, field_desc, idx))
              : static_cast<double>(ref->GetFloat(message, field_desc)));
    case FieldDescriptor::CppType::CPPTYPE_INT32:
      return std::to_string(
          is_repeated ? ref->GetRepeatedInt32(message, field_desc, idx)
                      : ref->GetInt32(message, field_desc));
    case FieldDescriptor::CppType::CPPTYPE_INT64:
      return std::to_string(
          is_repeated ? ref->GetRepeatedInt64(message, field_desc, idx)
                      : ref->GetInt64(message, field_desc));
    case FieldDescriptor::CppType::CPPTYPE_DOUBLE:
      return std::to_string(
          is_repeated ? ref->GetRepeatedDouble(message, field_desc, idx)
                      : ref->GetDouble(message, field_desc));
    case FieldDescriptor::CppType::CPPTYPE_STRING:
      return EscapeJsonString(
          is_repeated ? ref->GetRepeatedString(message, field_desc, idx)
                      : ref->GetString(message, field_desc));
    case FieldDescriptor::CppType::CPPTYPE_UINT32:
      return std::to_string(
          is_repeated ? ref->GetRepeatedUInt32(message, field_desc, idx)
                      : ref->GetUInt32(message, field_desc));
    case FieldDescriptor::CppType::CPPTYPE_UINT64:
      return std::to_string(
          is_repeated ? ref->GetRepeatedInt64(message, field_desc, idx)
                      : ref->GetInt64(message, field_desc));
    case FieldDescriptor::CppType::CPPTYPE_MESSAGE:
      return MessageToJson(
          is_repeated ? ref->GetRepeatedMessage(message, field_desc, idx)
                      : ref->GetMessage(message, field_desc),
          indent);
  }
  PERFETTO_FATAL("For GCC");
}

std::string RepeatedFieldValuesToJson(
    const google::protobuf::Message& message,
    const google::protobuf::FieldDescriptor* field_desc,
    uint32_t indent) {
  const google::protobuf::Reflection* ref = message.GetReflection();
  std::string ret;
  for (int i = 0; i < ref->FieldSize(message, field_desc); ++i) {
    if (i != 0) {
      ret += ",";
    }
    ret += "\n" + std::string(indent, ' ') +
           FieldToJson(message, field_desc, i, indent);
  }
  return ret;
}

std::string MessageFieldsToJson(const google::protobuf::Message& message,
                                uint32_t indent) {
  const google::protobuf::Reflection* ref = message.GetReflection();
  std::vector<const google::protobuf::FieldDescriptor*> field_descs;
  ref->ListFields(message, &field_descs);

  std::string ret;
  uint32_t next_field_idx = 0;
  for (const google::protobuf::FieldDescriptor* field_desc : field_descs) {
    if (next_field_idx++ != 0) {
      ret += ",";
    }
    std::string value;
    if (field_desc->is_repeated()) {
      value = "[" + RepeatedFieldValuesToJson(message, field_desc, indent + 2) +
              "\n" + std::string(indent, ' ') + "]";
    } else {
      value = FieldToJson(message, field_desc, 0, indent);
    }
    const std::string& name = field_desc->is_extension()
                                  ? field_desc->full_name()
                                  : field_desc->name();
    ret += "\n" + std::string(indent, ' ') + "\"" + name + "\": " + value;
  }
  return ret;
}

}  // namespace

std::string MessageToJson(const google::protobuf::Message& message,
                          uint32_t indent) {
  return "{" + MessageFieldsToJson(message, indent + 2) + "\n" +
         std::string(indent, ' ') + "}";
}

}  // namespace proto_to_json
}  // namespace trace_processor
}  // namespace perfetto
