/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/util/protozero_to_json.h"

#include <optional>
#include <unordered_set>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto {
namespace trace_processor {
namespace protozero_to_json {

namespace {

using protos::pbzero::FieldDescriptorProto;
using protozero::PackedRepeatedFieldIterator;
using protozero::proto_utils::ProtoWireType;

class JsonBuilder {
 public:
  JsonBuilder(int flags) : flags_(flags) {}

  void OpenObject() {
    if (is_array_scope() && !is_empty_scope()) {
      Append(",");
      MaybeAppendNewline();
      MaybeAppendIndent();
    }
    Append("{");
    stack_.push_back(Scope{ScopeContext::kObject});
  }

  void CloseObject() {
    // If we're closing the root object add errors if requested:
    if (is_root_scope() && is_inline_errors() && !errors_.empty()) {
      Key("__error");
      StringValue(base::StringView(base::Join(errors_, "\n")));
    }

    bool needs_newline = !is_empty_scope();
    stack_.pop_back();
    if (needs_newline) {
      MaybeAppendNewline();
      MaybeAppendIndent();
    }

    MarkScopeAsNonEmpty();
    Append("}");
  }

  void OpenArray() {
    Append("[");
    stack_.push_back(Scope{ScopeContext::kArray});
  }

  void CloseArray() {
    bool needs_newline = !is_empty_scope();
    stack_.pop_back();
    if (needs_newline) {
      MaybeAppendNewline();
      MaybeAppendIndent();
    }
    Append("]");
    if (is_array_scope() && !is_empty_scope()) {
      Append(",");
    }
  }

  void Key(const std::string& key) {
    if (is_object_scope() && !is_empty_scope()) {
      Append(",");
    }
    MaybeAppendNewline();
    MaybeAppendIndent();
    Append(EscapeString(base::StringView(key)));
    Append(":");
    MaybeAppendSpace();
    MarkScopeAsNonEmpty();
  }

  template <typename T>
  void NumberValue(T v) {
    AppendValue(std::to_string(v));
  }

  void BoolValue(bool v) { AppendValue(v ? "true" : "false"); }

  void FloatValue(float v) { NumberValue(v); }

  void DoubleValue(double v) { NumberValue(v); }

  void StringValue(base::StringView v) { AppendValue(EscapeString(v)); }

  void AddError(const std::string& s) { errors_.push_back(s); }

  std::string ToString() { return base::Join(parts_, ""); }

  bool is_pretty() { return flags_ & Flags::kPretty; }

  bool is_inline_errors() { return flags_ & Flags::kInlineErrors; }

 private:
  enum class ScopeContext {
    kObject,
    kArray,
  };

  struct Scope {
    ScopeContext ctx;
    bool is_empty = true;
  };

  int flags_;
  std::vector<std::string> parts_;
  std::vector<Scope> stack_;
  std::vector<std::string> errors_;

  bool is_object_scope() {
    return stack_.size() > 0 && stack_.back().ctx == ScopeContext::kObject;
  }

  bool is_array_scope() {
    return stack_.size() > 0 && stack_.back().ctx == ScopeContext::kArray;
  }

  bool is_empty_scope() { return stack_.size() > 0 && stack_.back().is_empty; }

  bool is_root_scope() { return stack_.size() == 1; }

  void MarkScopeAsNonEmpty() {
    if (stack_.size() > 0) {
      stack_.back().is_empty = false;
    }
  }

  void MaybeAppendSpace() {
    if (is_pretty()) {
      Append(" ");
    }
  }

  void MaybeAppendIndent() {
    if (is_pretty()) {
      Append(std::string(stack_.size() * 2, ' '));
    }
  }

  void MaybeAppendNewline() {
    if (is_pretty()) {
      Append("\n");
    }
  }

  void AppendValue(const std::string& s) {
    if (is_array_scope() && !is_empty_scope()) {
      Append(",");
    }
    if (is_array_scope()) {
      MaybeAppendNewline();
      MaybeAppendIndent();
    }
    Append(s);
    MarkScopeAsNonEmpty();
  }

  void Append(const std::string& s) { parts_.push_back(s); }

  std::string EscapeString(base::StringView raw) {
    std::string result;
    result.reserve(raw.size() + 2);
    result += "\"";
    for (size_t i = 0; i < raw.size(); ++i) {
      char c = *(raw.begin() + i);
      switch (c) {
        case '"':
        case '\\':
          result += '\\';
          result += c;
          break;
        case '\n':
          result += R"(\n)";
          break;
        case '\b':
          result += R"(\b)";
          break;
        case '\f':
          result += R"(\b)";
          break;
        case '\r':
          result += R"(\r)";
          break;
        case '\t':
          result += R"(\t)";
          break;
        default:
          // ASCII characters between 0x20 (space) and 0x7e (tilde) are
          // inserted directly. All others are escaped.
          if (c >= 0x20 && c <= 0x7e) {
            result += c;
          } else {
            unsigned char uc = static_cast<unsigned char>(c);
            uint32_t codepoint = 0;

            // Compute the number of bytes:
            size_t extra = 1 + (uc >= 0xc0u) + (uc >= 0xe0u) + (uc >= 0xf0u);

            // We want to consume |extra| bytes but also need to not
            // read out of bounds:
            size_t stop = std::min(raw.size(), i + extra);

            // Manually insert the bits from first byte:
            codepoint |= uc & (0xff >> (extra + 1));

            // Insert remaining bits:
            for (size_t j = i + 1; j < stop; ++j) {
              uc = static_cast<unsigned char>(*(raw.begin() + j));
              codepoint = (codepoint << 6) | (uc & 0x3f);
            }

            // Update i to show the consumed chars:
            i = stop - 1;

            static const char hex_chars[] = "0123456789abcdef";
            // JSON does not have proper utf-8 escapes. Instead you
            // have to use utf-16 codes. For the low codepoints
            // \uXXXX and for the high codepoints a surrogate pair:
            // \uXXXX\uYYYY
            if (codepoint <= 0xffff) {
              result += R"(\u)";
              result += hex_chars[(codepoint >> 12) & 0xf];
              result += hex_chars[(codepoint >> 8) & 0xf];
              result += hex_chars[(codepoint >> 4) & 0xf];
              result += hex_chars[(codepoint >> 0) & 0xf];
            } else {
              uint32_t high = ((codepoint - 0x10000) >> 10) + 0xD800;
              uint32_t low = (codepoint & 0x4fff) + 0xDC00;
              result += R"(\u)";
              result += hex_chars[(high >> 12) & 0xf];
              result += hex_chars[(high >> 8) & 0xf];
              result += hex_chars[(high >> 4) & 0xf];
              result += hex_chars[(high >> 0) & 0xf];
              result += R"(\u)";
              result += hex_chars[(low >> 12) & 0xf];
              result += hex_chars[(low >> 8) & 0xf];
              result += hex_chars[(low >> 4) & 0xf];
              result += hex_chars[(low >> 0) & 0xf];
            }
          }
          break;
      }
    }
    result += "\"";
    return result;
  }
};

bool IsTypeMatch(ProtoWireType wire, uint32_t type) {
  switch (wire) {
    case ProtoWireType::kVarInt:
      switch (type) {
        case FieldDescriptorProto::TYPE_INT32:
        case FieldDescriptorProto::TYPE_SINT32:
        case FieldDescriptorProto::TYPE_UINT32:
        case FieldDescriptorProto::TYPE_INT64:
        case FieldDescriptorProto::TYPE_SINT64:
        case FieldDescriptorProto::TYPE_UINT64:
        case FieldDescriptorProto::TYPE_BOOL:
        case FieldDescriptorProto::TYPE_ENUM:
          return true;
        default:
          return false;
      }
    case ProtoWireType::kLengthDelimited:
      switch (type) {
        case FieldDescriptorProto::TYPE_BYTES:
        case FieldDescriptorProto::TYPE_MESSAGE:
        case FieldDescriptorProto::TYPE_STRING:
          // The normal case.
          return true;
        case FieldDescriptorProto::TYPE_INT32:
        case FieldDescriptorProto::TYPE_SINT32:
        case FieldDescriptorProto::TYPE_UINT32:
        case FieldDescriptorProto::TYPE_INT64:
        case FieldDescriptorProto::TYPE_SINT64:
        case FieldDescriptorProto::TYPE_UINT64:
        case FieldDescriptorProto::TYPE_BOOL:
        case FieldDescriptorProto::TYPE_ENUM:
        case FieldDescriptorProto::TYPE_FIXED32:
        case FieldDescriptorProto::TYPE_SFIXED32:
        case FieldDescriptorProto::TYPE_FLOAT:
        case FieldDescriptorProto::TYPE_FIXED64:
        case FieldDescriptorProto::TYPE_SFIXED64:
        case FieldDescriptorProto::TYPE_DOUBLE:
          // Packed repeated fields.
          return true;
        default:
          return false;
      }
    case ProtoWireType::kFixed32:
      switch (type) {
        case FieldDescriptorProto::TYPE_FIXED32:
        case FieldDescriptorProto::TYPE_SFIXED32:
        case FieldDescriptorProto::TYPE_FLOAT:
          return true;
        default:
          return false;
      }
    case ProtoWireType::kFixed64:
      switch (type) {
        case FieldDescriptorProto::TYPE_FIXED64:
        case FieldDescriptorProto::TYPE_SFIXED64:
        case FieldDescriptorProto::TYPE_DOUBLE:
          return true;
        default:
          return false;
      }
  }
  PERFETTO_FATAL("For GCC");
}

bool IsNumericFieldType(uint32_t type) {
  switch (type) {
    case FieldDescriptorProto::TYPE_BYTES:
    case FieldDescriptorProto::TYPE_MESSAGE:
    case FieldDescriptorProto::TYPE_STRING:
      return false;
    case FieldDescriptorProto::TYPE_INT32:
    case FieldDescriptorProto::TYPE_SINT32:
    case FieldDescriptorProto::TYPE_UINT32:
    case FieldDescriptorProto::TYPE_INT64:
    case FieldDescriptorProto::TYPE_SINT64:
    case FieldDescriptorProto::TYPE_UINT64:
    case FieldDescriptorProto::TYPE_BOOL:
    case FieldDescriptorProto::TYPE_ENUM:
    case FieldDescriptorProto::TYPE_FIXED32:
    case FieldDescriptorProto::TYPE_SFIXED32:
    case FieldDescriptorProto::TYPE_FLOAT:
    case FieldDescriptorProto::TYPE_FIXED64:
    case FieldDescriptorProto::TYPE_SFIXED64:
    case FieldDescriptorProto::TYPE_DOUBLE:
    default:
      return true;
  }
}

void MessageField(const DescriptorPool& pool,
                  const std::string& type,
                  protozero::ConstBytes protobytes,
                  JsonBuilder* out);
void EnumField(const DescriptorPool& pool,
               const FieldDescriptor& fd,
               int32_t value,
               JsonBuilder* out);

template <ProtoWireType W, typename T>
void PackedField(const DescriptorPool& pool,
                 const FieldDescriptor& fd,
                 const protozero::Field& field,
                 JsonBuilder* out) {
  out->OpenArray();
  bool e = false;
  for (PackedRepeatedFieldIterator<W, T> it(field.data(), field.size(), &e); it;
       it++) {
    T value = *it;
    if (fd.type() == FieldDescriptorProto::TYPE_ENUM) {
      EnumField(pool, fd, static_cast<int32_t>(value), out);
    } else {
      out->NumberValue<T>(value);
    }
  }
  out->CloseArray();
  if (e) {
    out->AddError(
        std::string("Decoding failure for field '" + fd.name() + "'"));
  }
}

template <ProtoWireType W>
void PackedBoolField(const DescriptorPool&,
                     const FieldDescriptor& fd,
                     const protozero::Field& field,
                     JsonBuilder* out) {
  out->OpenArray();
  bool e = false;
  for (PackedRepeatedFieldIterator<W, int32_t> it(field.data(), field.size(),
                                                  &e);
       it; it++) {
    bool value = *it;
    out->BoolValue(value);
  }
  out->CloseArray();
  if (e) {
    out->AddError(
        std::string("Decoding failure for field '" + fd.name() + "'"));
  }
}

void LengthField(const DescriptorPool& pool,
                 const FieldDescriptor* fd,
                 const protozero::Field& field,
                 JsonBuilder* out) {
  uint32_t type = fd ? fd->type() : 0;
  switch (type) {
    case FieldDescriptorProto::TYPE_BYTES:
      out->StringValue(field.as_string());
      return;
    case FieldDescriptorProto::TYPE_STRING:
      out->StringValue(field.as_string());
      return;
    case FieldDescriptorProto::TYPE_MESSAGE:
      MessageField(pool, fd->resolved_type_name(), field.as_bytes(), out);
      return;
    case FieldDescriptorProto::TYPE_DOUBLE:
      PackedField<ProtoWireType::kFixed64, double>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_FLOAT:
      PackedField<ProtoWireType::kFixed32, float>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_FIXED32:
      PackedField<ProtoWireType::kFixed32, uint32_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_SFIXED32:
      PackedField<ProtoWireType::kFixed32, int32_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_INT32:
      PackedField<ProtoWireType::kVarInt, int32_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_SINT32:
      PackedField<ProtoWireType::kVarInt, int32_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_UINT32:
      PackedField<ProtoWireType::kVarInt, uint32_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_FIXED64:
      PackedField<ProtoWireType::kFixed64, uint64_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_SFIXED64:
      PackedField<ProtoWireType::kFixed64, int64_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_INT64:
      PackedField<ProtoWireType::kVarInt, int64_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_SINT64:
      PackedField<ProtoWireType::kVarInt, int64_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_UINT64:
      PackedField<ProtoWireType::kVarInt, uint64_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_ENUM:
      PackedField<ProtoWireType::kVarInt, int32_t>(pool, *fd, field, out);
      return;
    case FieldDescriptorProto::TYPE_BOOL:
      PackedBoolField<ProtoWireType::kVarInt>(pool, *fd, field, out);
      return;
    case 0:
    default:
      // In the absence of specific information display bytes.
      out->StringValue(field.as_string());
      return;
  }
}

void EnumField(const DescriptorPool& pool,
               const FieldDescriptor& fd,
               int32_t value,
               JsonBuilder* out) {
  auto opt_enum_descriptor_idx =
      pool.FindDescriptorIdx(fd.resolved_type_name());
  if (!opt_enum_descriptor_idx) {
    out->NumberValue(value);
    return;
  }
  auto opt_enum_string =
      pool.descriptors()[*opt_enum_descriptor_idx].FindEnumString(value);
  // If the enum value is unknown, treat it like a completely unknown field.
  if (!opt_enum_string) {
    out->NumberValue(value);
    return;
  }

  out->StringValue(base::StringView(*opt_enum_string));
}

void VarIntField(const DescriptorPool& pool,
                 const FieldDescriptor* fd,
                 const protozero::Field& field,
                 JsonBuilder* out) {
  uint32_t type = fd ? fd->type() : 0;
  switch (type) {
    case FieldDescriptorProto::TYPE_INT32:
      out->NumberValue(field.as_int32());
      return;
    case FieldDescriptorProto::TYPE_SINT32:
      out->NumberValue(field.as_sint32());
      return;
    case FieldDescriptorProto::TYPE_UINT32:
      out->NumberValue(field.as_uint32());
      return;
    case FieldDescriptorProto::TYPE_INT64:
      out->NumberValue(field.as_int64());
      return;
    case FieldDescriptorProto::TYPE_SINT64:
      out->NumberValue(field.as_sint64());
      return;
    case FieldDescriptorProto::TYPE_UINT64:
      out->NumberValue(field.as_uint64());
      return;
    case FieldDescriptorProto::TYPE_BOOL:
      out->BoolValue(field.as_bool());
      return;
    case FieldDescriptorProto::TYPE_ENUM:
      EnumField(pool, *fd, field.as_int32(), out);
      return;
    case 0:
    default:
      out->NumberValue(field.as_int64());
      return;
  }
}

void Fixed32Field(const FieldDescriptor* fd,
                  const protozero::Field& field,
                  JsonBuilder* out) {
  uint32_t type = fd ? fd->type() : 0;
  switch (type) {
    case FieldDescriptorProto::TYPE_SFIXED32:
      out->NumberValue(field.as_int32());
      break;
    case FieldDescriptorProto::TYPE_FIXED32:
      out->NumberValue(field.as_uint32());
      break;
    case FieldDescriptorProto::TYPE_FLOAT:
      out->FloatValue(field.as_float());
      break;
    case 0:
    default:
      out->NumberValue(field.as_uint32());
      break;
  }
}

void Fixed64Field(const FieldDescriptor* fd,
                  const protozero::Field& field,
                  JsonBuilder* out) {
  uint64_t type = fd ? fd->type() : 0;
  switch (type) {
    case FieldDescriptorProto::TYPE_SFIXED64:
      out->NumberValue(field.as_int64());
      break;
    case FieldDescriptorProto::TYPE_FIXED64:
      out->NumberValue(field.as_uint64());
      break;
    case FieldDescriptorProto::TYPE_DOUBLE:
      out->DoubleValue(field.as_double());
      break;
    case 0:
    default:
      out->NumberValue(field.as_uint64());
      break;
  }
}

void RepeatedVarInt(const DescriptorPool& pool,
                    protozero::ConstBytes protobytes,
                    const FieldDescriptor* fd,
                    uint32_t id,
                    JsonBuilder* out) {
  out->OpenArray();
  protozero::ProtoDecoder decoder(protobytes.data, protobytes.size);
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == id) {
      VarIntField(pool, fd, field, out);
    }
  }
  out->CloseArray();
}

void RepeatedLengthField(const DescriptorPool& pool,
                         protozero::ConstBytes protobytes,
                         const FieldDescriptor* fd,
                         uint32_t id,
                         JsonBuilder* out) {
  out->OpenArray();
  protozero::ProtoDecoder decoder(protobytes.data, protobytes.size);
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == id) {
      LengthField(pool, fd, field, out);
    }
  }
  out->CloseArray();
}

void RepeatedFixed64(protozero::ConstBytes protobytes,
                     const FieldDescriptor* fd,
                     uint32_t id,
                     JsonBuilder* out) {
  out->OpenArray();
  protozero::ProtoDecoder decoder(protobytes.data, protobytes.size);
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == id) {
      Fixed64Field(fd, field, out);
    }
  }
  out->CloseArray();
}

void RepeatedFixed32(protozero::ConstBytes protobytes,
                     const FieldDescriptor* fd,
                     uint32_t id,
                     JsonBuilder* out) {
  out->OpenArray();
  protozero::ProtoDecoder decoder(protobytes.data, protobytes.size);
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == id) {
      Fixed32Field(fd, field, out);
    }
  }
  out->CloseArray();
}

void MessageField(const DescriptorPool& pool,
                  const std::string& type,
                  protozero::ConstBytes protobytes,
                  JsonBuilder* out) {
  out->OpenObject();

  std::optional<uint32_t> opt_proto_desc_idx = pool.FindDescriptorIdx(type);
  const ProtoDescriptor* opt_proto_descriptor =
      opt_proto_desc_idx ? &pool.descriptors()[*opt_proto_desc_idx] : nullptr;

  protozero::ProtoDecoder decoder(protobytes.data, protobytes.size);
  std::unordered_set<uint32_t> fields_seen;

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    auto* opt_field_descriptor =
        opt_proto_descriptor ? opt_proto_descriptor->FindFieldByTag(field.id())
                             : nullptr;
    bool is_repeated = false;
    if (opt_field_descriptor &&
        IsTypeMatch(field.type(), opt_field_descriptor->type())) {
      is_repeated = opt_field_descriptor->is_repeated();
      // The first time we see a repeated field we consume them all:
      if (fields_seen.count(field.id())) {
        continue;
      }
      out->Key(opt_field_descriptor->name());
    } else {
      out->Key(std::to_string(field.id()));
    }
    if (is_repeated) {
      fields_seen.insert(field.id());

      switch (field.type()) {
        case ProtoWireType::kVarInt:
          RepeatedVarInt(pool, protobytes, opt_field_descriptor, field.id(),
                         out);
          break;
        case ProtoWireType::kLengthDelimited:
          if (opt_field_descriptor &&
              IsNumericFieldType(opt_field_descriptor->type())) {
            // wire_type = length + field_type in
            // {u,s,}int{32,64}, float, double etc means this is the
            // packed case:
            LengthField(pool, opt_field_descriptor, field, out);
          } else {
            RepeatedLengthField(pool, protobytes, opt_field_descriptor,
                                field.id(), out);
          }
          break;
        case ProtoWireType::kFixed32:
          RepeatedFixed32(protobytes, opt_field_descriptor, field.id(), out);
          break;
        case ProtoWireType::kFixed64:
          RepeatedFixed64(protobytes, opt_field_descriptor, field.id(), out);
          break;
      }
    } else {
      switch (field.type()) {
        case ProtoWireType::kVarInt:
          VarIntField(pool, opt_field_descriptor, field, out);
          break;
        case ProtoWireType::kLengthDelimited:
          LengthField(pool, opt_field_descriptor, field, out);
          break;
        case ProtoWireType::kFixed32:
          Fixed32Field(opt_field_descriptor, field, out);
          break;
        case ProtoWireType::kFixed64:
          Fixed64Field(opt_field_descriptor, field, out);
          break;
      }
    }
  }

  if (decoder.bytes_left() != 0) {
    out->AddError(std::to_string(decoder.bytes_left()) + " extra bytes");
  }

  out->CloseObject();
}

}  // namespace

std::string ProtozeroToJson(const DescriptorPool& pool,
                            const std::string& type,
                            protozero::ConstBytes protobytes,
                            int flags) {
  JsonBuilder builder(flags);
  MessageField(pool, type, protobytes, &builder);
  return builder.ToString();
}

std::string ProtozeroToJson(const DescriptorPool& pool,
                            const std::string& type,
                            const std::vector<uint8_t>& protobytes,
                            int flags) {
  return ProtozeroToJson(
      pool, type, protozero::ConstBytes{protobytes.data(), protobytes.size()},
      flags);
}

}  // namespace protozero_to_json
}  // namespace trace_processor
}  // namespace perfetto
