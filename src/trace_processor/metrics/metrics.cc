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

#include <regex>
#include <unordered_map>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/tp_metatrace.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/status_macros.h"

#include "protos/perfetto/common/descriptor.pbzero.h"
#include "protos/perfetto/trace_processor/metrics_impl.pbzero.h"

namespace perfetto {
namespace trace_processor {
namespace metrics {

namespace {

base::Status ValidateSingleNonEmptyMessage(const uint8_t* ptr,
                                           size_t size,
                                           uint32_t schema_type,
                                           const std::string& message_type,
                                           protozero::ConstBytes* out) {
  PERFETTO_DCHECK(size > 0);

  if (size > protozero::proto_utils::kMaxMessageLength) {
    return base::ErrStatus(
        "Message has size %zu which is larger than the maximum allowed message "
        "size %zu",
        size, protozero::proto_utils::kMaxMessageLength);
  }

  protos::pbzero::ProtoBuilderResult::Decoder decoder(ptr, size);
  if (decoder.is_repeated()) {
    return base::ErrStatus("Cannot handle nested repeated messages");
  }

  const auto& single_field = decoder.single();
  protos::pbzero::SingleBuilderResult::Decoder single(single_field.data,
                                                      single_field.size);

  if (single.type() != schema_type) {
    return base::ErrStatus("Message field has wrong wire type %d",
                           single.type());
  }

  base::StringView actual_type(single.type_name());
  if (actual_type != base::StringView(message_type)) {
    return base::ErrStatus("Field has wrong type (expected %s, was %s)",
                           message_type.c_str(),
                           actual_type.ToStdString().c_str());
  }

  if (!single.has_protobuf()) {
    return base::ErrStatus("Message has no proto bytes");
  }

  // We disallow 0 size fields here as they should have been reported as null
  // one layer down.
  *out = single.protobuf();
  if (out->size == 0) {
    return base::ErrStatus("Field has zero size");
  }
  return base::OkStatus();
}

}  // namespace

ProtoBuilder::ProtoBuilder(const DescriptorPool* pool,
                           const ProtoDescriptor* descriptor)
    : pool_(pool), descriptor_(descriptor) {}

base::Status ProtoBuilder::AppendSqlValue(const std::string& field_name,
                                          const SqlValue& value) {
  switch (value.type) {
    case SqlValue::kLong:
      return AppendLong(field_name, value.long_value);
    case SqlValue::kDouble:
      return AppendDouble(field_name, value.double_value);
    case SqlValue::kString:
      return AppendString(field_name, value.string_value);
    case SqlValue::kBytes:
      return AppendBytes(field_name,
                         static_cast<const uint8_t*>(value.bytes_value),
                         value.bytes_count);
    case SqlValue::kNull:
      // If the value is null, it's treated as the field being absent so we
      // don't append anything.
      return base::OkStatus();
  }
  PERFETTO_FATAL("For GCC");
}

base::Status ProtoBuilder::AppendLong(const std::string& field_name,
                                      int64_t value,
                                      bool is_inside_repeated) {
  auto field = descriptor_->FindFieldByName(field_name);
  if (!field) {
    return base::ErrStatus("Field with name %s not found in proto type %s",
                           field_name.c_str(),
                           descriptor_->full_name().c_str());
  }

  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  if (field->is_repeated() && !is_inside_repeated) {
    return base::ErrStatus(
        "Unexpected long value for repeated field %s in proto type %s",
        field_name.c_str(), descriptor_->full_name().c_str());
  }

  switch (field->type()) {
    case FieldDescriptorProto::TYPE_INT32:
    case FieldDescriptorProto::TYPE_INT64:
    case FieldDescriptorProto::TYPE_UINT32:
    case FieldDescriptorProto::TYPE_BOOL:
      message_->AppendVarInt(field->number(), value);
      break;
    case FieldDescriptorProto::TYPE_ENUM: {
      auto opt_enum_descriptor_idx =
          pool_->FindDescriptorIdx(field->resolved_type_name());
      if (!opt_enum_descriptor_idx) {
        return base::ErrStatus(
            "Unable to find enum type %s to fill field %s (in proto message "
            "%s)",
            field->resolved_type_name().c_str(), field->name().c_str(),
            descriptor_->full_name().c_str());
      }
      const auto& enum_desc = pool_->descriptors()[*opt_enum_descriptor_idx];
      auto opt_enum_str = enum_desc.FindEnumString(static_cast<int32_t>(value));
      if (!opt_enum_str) {
        return base::ErrStatus("Invalid enum value %" PRId64
                               " "
                               "in enum type %s; encountered while filling "
                               "field %s (in proto message %s)",
                               value, field->resolved_type_name().c_str(),
                               field->name().c_str(),
                               descriptor_->full_name().c_str());
      }
      message_->AppendVarInt(field->number(), value);
      break;
    }
    case FieldDescriptorProto::TYPE_SINT32:
    case FieldDescriptorProto::TYPE_SINT64:
      message_->AppendSignedVarInt(field->number(), value);
      break;
    case FieldDescriptorProto::TYPE_FIXED32:
    case FieldDescriptorProto::TYPE_SFIXED32:
    case FieldDescriptorProto::TYPE_FIXED64:
    case FieldDescriptorProto::TYPE_SFIXED64:
      message_->AppendFixed(field->number(), value);
      break;
    case FieldDescriptorProto::TYPE_UINT64:
      return base::ErrStatus(
          "Field %s (in proto message %s) is using a uint64 type. uint64 in "
          "metric messages is not supported by trace processor; use an int64 "
          "field instead.",
          field->name().c_str(), descriptor_->full_name().c_str());
    default: {
      return base::ErrStatus(
          "Tried to write value of type long into field %s (in proto type %s) "
          "which has type %d",
          field->name().c_str(), descriptor_->full_name().c_str(),
          field->type());
    }
  }
  return base::OkStatus();
}

base::Status ProtoBuilder::AppendDouble(const std::string& field_name,
                                        double value,
                                        bool is_inside_repeated) {
  auto field = descriptor_->FindFieldByName(field_name);
  if (!field) {
    return base::ErrStatus("Field with name %s not found in proto type %s",
                           field_name.c_str(),
                           descriptor_->full_name().c_str());
  }

  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  if (field->is_repeated() && !is_inside_repeated) {
    return base::ErrStatus(
        "Unexpected double value for repeated field %s in proto type %s",
        field_name.c_str(), descriptor_->full_name().c_str());
  }

  switch (field->type()) {
    case FieldDescriptorProto::TYPE_FLOAT:
    case FieldDescriptorProto::TYPE_DOUBLE: {
      if (field->type() == FieldDescriptorProto::TYPE_FLOAT) {
        message_->AppendFixed(field->number(), static_cast<float>(value));
      } else {
        message_->AppendFixed(field->number(), value);
      }
      break;
    }
    default: {
      return base::ErrStatus(
          "Tried to write value of type double into field %s (in proto type "
          "%s) which has type %d",
          field->name().c_str(), descriptor_->full_name().c_str(),
          field->type());
    }
  }
  return base::OkStatus();
}

base::Status ProtoBuilder::AppendString(const std::string& field_name,
                                        base::StringView data,
                                        bool is_inside_repeated) {
  const FieldDescriptor* field = descriptor_->FindFieldByName(field_name);
  if (!field) {
    return base::ErrStatus("Field with name %s not found in proto type %s",
                           field_name.c_str(),
                           descriptor_->full_name().c_str());
  }

  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  if (field->is_repeated() && !is_inside_repeated) {
    return base::ErrStatus(
        "Unexpected string value for repeated field %s in proto type %s",
        field_name.c_str(), descriptor_->full_name().c_str());
  }

  switch (field->type()) {
    case FieldDescriptorProto::TYPE_STRING: {
      message_->AppendBytes(field->number(), data.data(), data.size());
      break;
    }
    case FieldDescriptorProto::TYPE_ENUM: {
      auto opt_enum_descriptor_idx =
          pool_->FindDescriptorIdx(field->resolved_type_name());
      if (!opt_enum_descriptor_idx) {
        return base::ErrStatus(
            "Unable to find enum type %s to fill field %s (in proto message "
            "%s)",
            field->resolved_type_name().c_str(), field->name().c_str(),
            descriptor_->full_name().c_str());
      }
      const auto& enum_desc = pool_->descriptors()[*opt_enum_descriptor_idx];
      std::string enum_str = data.ToStdString();
      auto opt_enum_value = enum_desc.FindEnumValue(enum_str);
      if (!opt_enum_value) {
        return base::ErrStatus(
            "Invalid enum string %s "
            "in enum type %s; encountered while filling "
            "field %s (in proto message %s)",
            enum_str.c_str(), field->resolved_type_name().c_str(),
            field->name().c_str(), descriptor_->full_name().c_str());
      }
      message_->AppendVarInt(field->number(), *opt_enum_value);
      break;
    }
    default: {
      return base::ErrStatus(
          "Tried to write value of type string into field %s (in proto type "
          "%s) which has type %d",
          field->name().c_str(), descriptor_->full_name().c_str(),
          field->type());
    }
  }
  return base::OkStatus();
}

base::Status ProtoBuilder::AppendBytes(const std::string& field_name,
                                       const uint8_t* ptr,
                                       size_t size,
                                       bool is_inside_repeated) {
  const FieldDescriptor* field = descriptor_->FindFieldByName(field_name);
  if (!field) {
    return base::ErrStatus("Field with name %s not found in proto type %s",
                           field_name.c_str(),
                           descriptor_->full_name().c_str());
  }

  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  if (field->is_repeated() && !is_inside_repeated)
    return AppendRepeated(*field, ptr, size);

  if (field->type() == FieldDescriptorProto::TYPE_MESSAGE)
    return AppendSingleMessage(*field, ptr, size);

  if (size == 0) {
    return base::ErrStatus(
        "Tried to write zero-sized value into field %s (in proto type "
        "%s). Nulls are only supported for message protos; all other types "
        "should ensure that nulls are not passed to proto builder functions by "
        "using the SQLite IFNULL/COALESCE functions.",
        field->name().c_str(), descriptor_->full_name().c_str());
  }

  return base::ErrStatus(
      "Tried to write value of type bytes into field %s (in proto type %s) "
      "which has type %d",
      field->name().c_str(), descriptor_->full_name().c_str(), field->type());
}

base::Status ProtoBuilder::AppendSingleMessage(const FieldDescriptor& field,
                                               const uint8_t* ptr,
                                               size_t size) {
  // If we have an zero sized bytes, we still want to propogate that the field
  // message was set but empty.
  if (size == 0) {
    // ptr can be null and passing nullptr to AppendBytes feels dangerous so
    // just pass an empty string (which will have a valid pointer always) and
    // zero as the size.
    message_->AppendBytes(field.number(), "", 0);
    return base::OkStatus();
  }

  protozero::ConstBytes bytes;
  base::Status validation = ValidateSingleNonEmptyMessage(
      ptr, size, field.type(), field.resolved_type_name(), &bytes);
  if (!validation.ok()) {
    return util::ErrStatus("[Field %s in message %s]: %s", field.name().c_str(),
                           descriptor_->full_name().c_str(),
                           validation.c_message());
  }
  message_->AppendBytes(field.number(), bytes.data, bytes.size);
  return base::OkStatus();
}

base::Status ProtoBuilder::AppendRepeated(const FieldDescriptor& field,
                                          const uint8_t* ptr,
                                          size_t size) {
  if (size > protozero::proto_utils::kMaxMessageLength) {
    return base::ErrStatus(
        "Message passed to field %s in proto message %s has size %zu which is "
        "larger than the maximum allowed message size %zu",
        field.name().c_str(), descriptor_->full_name().c_str(), size,
        protozero::proto_utils::kMaxMessageLength);
  }

  protos::pbzero::ProtoBuilderResult::Decoder decoder(ptr, size);
  if (!decoder.is_repeated()) {
    return base::ErrStatus(
        "Unexpected message value for repeated field %s in proto type %s",
        field.name().c_str(), descriptor_->full_name().c_str());
  }

  const auto& rep = decoder.repeated();
  protos::pbzero::RepeatedBuilderResult::Decoder repeated(rep.data, rep.size);

  for (auto it = repeated.value(); it; ++it) {
    protos::pbzero::RepeatedBuilderResult::Value::Decoder value(*it);
    base::Status status;
    if (value.has_int_value()) {
      status = AppendLong(field.name(), value.int_value(), true);
    } else if (value.has_double_value()) {
      status = AppendDouble(field.name(), value.double_value(), true);
    } else if (value.has_string_value()) {
      status = AppendString(field.name(),
                            base::StringView(value.string_value()), true);
    } else if (value.has_bytes_value()) {
      const auto& bytes = value.bytes_value();
      status = AppendBytes(field.name(), bytes.data, bytes.size, true);
    } else {
      status = base::ErrStatus("Unknown type in repeated field");
    }

    if (!status.ok())
      return status;
  }
  return base::OkStatus();
}

std::vector<uint8_t> ProtoBuilder::SerializeToProtoBuilderResult() {
  std::vector<uint8_t> serialized = SerializeRaw();
  if (serialized.empty())
    return serialized;

  const auto& type_name = descriptor_->full_name();

  protozero::HeapBuffered<protos::pbzero::ProtoBuilderResult> result;
  result->set_is_repeated(false);

  auto* single = result->set_single();
  single->set_type(protos::pbzero::FieldDescriptorProto::Type::TYPE_MESSAGE);
  single->set_type_name(type_name.c_str(), type_name.size());
  single->set_protobuf(serialized.data(), serialized.size());
  return result.SerializeAsArray();
}

std::vector<uint8_t> ProtoBuilder::SerializeRaw() {
  return message_.SerializeAsArray();
}

RepeatedFieldBuilder::RepeatedFieldBuilder() {
  repeated_ = message_->set_repeated();
}

base::Status RepeatedFieldBuilder::AddSqlValue(SqlValue value) {
  switch (value.type) {
    case SqlValue::kLong:
      AddLong(value.long_value);
      break;
    case SqlValue::kDouble:
      AddDouble(value.double_value);
      break;
    case SqlValue::kString:
      AddString(value.string_value);
      break;
    case SqlValue::kBytes:
      AddBytes(static_cast<const uint8_t*>(value.bytes_value),
               value.bytes_count);
      break;
    case SqlValue::kNull:
      AddBytes(nullptr, 0);
      break;
  }
  return base::OkStatus();
}

void RepeatedFieldBuilder::AddLong(int64_t value) {
  has_data_ = true;
  repeated_->add_value()->set_int_value(value);
}

void RepeatedFieldBuilder::AddDouble(double value) {
  has_data_ = true;
  repeated_->add_value()->set_double_value(value);
}

void RepeatedFieldBuilder::AddString(base::StringView value) {
  has_data_ = true;
  repeated_->add_value()->set_string_value(value.data(), value.size());
}

void RepeatedFieldBuilder::AddBytes(const uint8_t* data, size_t size) {
  has_data_ = true;
  repeated_->add_value()->set_bytes_value(data, size);
}

std::vector<uint8_t> RepeatedFieldBuilder::SerializeToProtoBuilderResult() {
  repeated_ = nullptr;
  if (!has_data_)
    return std::vector<uint8_t>();

  message_->set_is_repeated(true);
  return message_.SerializeAsArray();
}

int TemplateReplace(
    const std::string& raw_text,
    const std::unordered_map<std::string, std::string>& substitutions,
    std::string* out) {
  std::regex re(R"(\{\{\s*(\w*)\s*\}\})", std::regex_constants::ECMAScript);

  auto it = std::sregex_iterator(raw_text.begin(), raw_text.end(), re);
  auto regex_end = std::sregex_iterator();
  auto start = raw_text.begin();
  for (; it != regex_end; ++it) {
    out->insert(out->end(), start, raw_text.begin() + it->position(0));

    auto value_it = substitutions.find(it->str(1));
    if (value_it == substitutions.end())
      return 1;

    const auto& value = value_it->second;
    std::copy(value.begin(), value.end(), std::back_inserter(*out));
    start = raw_text.begin() + it->position(0) + it->length(0);
  }
  out->insert(out->end(), start, raw_text.end());
  return 0;
}

base::Status NullIfEmpty::Run(void*,
                              size_t argc,
                              sqlite3_value** argv,
                              SqlValue& out,
                              Destructors&) {
  // SQLite should enforce this for us.
  PERFETTO_CHECK(argc == 1);

  if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
    return base::ErrStatus(
        "NULL_IF_EMPTY: should only be called with bytes argument");
  }

  if (sqlite3_value_bytes(argv[0]) == 0)
    return base::OkStatus();

  out = sqlite_utils::SqliteValueToSqlValue(argv[0]);
  return base::OkStatus();
}

void RepeatedFieldStep(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  if (argc != 1) {
    sqlite3_result_error(ctx, "RepeatedField: only expected one arg", -1);
    return;
  }

  // We use a double indirection here so we can use new and delete without
  // needing to do dangerous dances with placement new and checking
  // initalization.
  auto** builder_ptr_ptr = static_cast<RepeatedFieldBuilder**>(
      sqlite3_aggregate_context(ctx, sizeof(RepeatedFieldBuilder*)));

  // The memory returned from sqlite3_aggregate_context is zeroed on its first
  // invocation so *builder_ptr_ptr will be nullptr on the first invocation of
  // RepeatedFieldStep.
  bool needs_init = *builder_ptr_ptr == nullptr;
  if (needs_init) {
    *builder_ptr_ptr = new RepeatedFieldBuilder();
  }

  auto value = sqlite_utils::SqliteValueToSqlValue(argv[0]);
  RepeatedFieldBuilder* builder = *builder_ptr_ptr;
  auto status = builder->AddSqlValue(value);
  if (!status.ok()) {
    sqlite3_result_error(ctx, status.c_message(), -1);
  }
}

void RepeatedFieldFinal(sqlite3_context* ctx) {
  // Note: we choose the size intentionally to be zero because we don't want to
  // allocate if the Step has never been called.
  auto** builder_ptr_ptr =
      static_cast<RepeatedFieldBuilder**>(sqlite3_aggregate_context(ctx, 0));

  // If Step has never been called, |builder_ptr_ptr| will be null.
  if (builder_ptr_ptr == nullptr) {
    sqlite3_result_null(ctx);
    return;
  }

  // Capture the context pointer so that it will be freed at the end of this
  // function.
  std::unique_ptr<RepeatedFieldBuilder> builder(*builder_ptr_ptr);
  std::vector<uint8_t> raw = builder->SerializeToProtoBuilderResult();
  if (raw.empty()) {
    sqlite3_result_null(ctx);
    return;
  }

  std::unique_ptr<uint8_t[], base::FreeDeleter> data(
      static_cast<uint8_t*>(malloc(raw.size())));
  memcpy(data.get(), raw.data(), raw.size());
  sqlite3_result_blob(ctx, data.release(), static_cast<int>(raw.size()), free);
}

// SQLite function implementation used to build a proto directly in SQL. The
// proto to be built is given by the descriptor which is given as a context
// parameter to this function and chosen when this function is first registed
// with SQLite. The args of this function are key value pairs specifying the
// name of the field and its value. Nested messages are expected to be passed
// as byte blobs (as they were built recursively using this function).
// The return value is the built proto or an error about why the proto could
// not be built.
base::Status BuildProto::Run(BuildProto::Context* ctx,
                             size_t argc,
                             sqlite3_value** argv,
                             SqlValue& out,
                             Destructors& destructors) {
  const ProtoDescriptor& desc = ctx->pool->descriptors()[ctx->descriptor_idx];
  if (argc % 2 != 0) {
    return base::ErrStatus("Invalid number of args to %s BuildProto (got %zu)",
                           desc.full_name().c_str(), argc);
  }

  ProtoBuilder builder(ctx->pool, &desc);
  for (size_t i = 0; i < argc; i += 2) {
    if (sqlite3_value_type(argv[i]) != SQLITE_TEXT) {
      return base::ErrStatus("BuildProto: Invalid args");
    }

    auto* key = reinterpret_cast<const char*>(sqlite3_value_text(argv[i]));
    auto value = sqlite_utils::SqliteValueToSqlValue(argv[i + 1]);
    RETURN_IF_ERROR(builder.AppendSqlValue(key, value));
  }

  // Even if the message is empty, we don't return null here as we want the
  // existence of the message to be respected.
  std::vector<uint8_t> raw = builder.SerializeToProtoBuilderResult();
  if (raw.empty()) {
    // Passing nullptr to SQLite feels dangerous so just pass an empty string
    // and zero as the size so we don't deref nullptr accidentially somewhere.
    destructors.bytes_destructor = sqlite_utils::kSqliteStatic;
    out = SqlValue::Bytes("", 0);
    return base::OkStatus();
  }

  std::unique_ptr<uint8_t[], base::FreeDeleter> data(
      static_cast<uint8_t*>(malloc(raw.size())));
  memcpy(data.get(), raw.data(), raw.size());

  destructors.bytes_destructor = free;
  out = SqlValue::Bytes(data.release(), raw.size());
  return base::OkStatus();
}

base::Status RunMetric::Run(RunMetric::Context* ctx,
                            size_t argc,
                            sqlite3_value** argv,
                            SqlValue&,
                            Destructors&) {
  if (argc == 0 || sqlite3_value_type(argv[0]) != SQLITE_TEXT)
    return base::ErrStatus("RUN_METRIC: Invalid arguments");

  const char* path = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
  auto metric_it = std::find_if(
      ctx->metrics->begin(), ctx->metrics->end(),
      [path](const SqlMetricFile& metric) { return metric.path == path; });
  if (metric_it == ctx->metrics->end()) {
    return base::ErrStatus("RUN_METRIC: Unknown filename provided %s", path);
  }

  std::unordered_map<std::string, std::string> substitutions;
  for (size_t i = 1; i < argc; i += 2) {
    if (sqlite3_value_type(argv[i]) != SQLITE_TEXT)
      return base::ErrStatus("RUN_METRIC: all keys must be strings");

    base::Optional<std::string> key_str = sqlite_utils::SqlValueToString(
        sqlite_utils::SqliteValueToSqlValue(argv[i]));
    base::Optional<std::string> value_str = sqlite_utils::SqlValueToString(
        sqlite_utils::SqliteValueToSqlValue(argv[i + 1]));

    if (!value_str) {
      return base::ErrStatus(
          "RUN_METRIC: all values must be convertible to strings");
    }
    substitutions[*key_str] = *value_str;
  }

  std::string subbed_sql;
  int ret = TemplateReplace(metric_it->sql, substitutions, &subbed_sql);
  if (ret) {
    return base::ErrStatus(
        "RUN_METRIC: Error when performing substitutions: %s",
        metric_it->sql.c_str());
  }

  auto it = ctx->tp->ExecuteQuery(subbed_sql);
  it.Next();

  base::Status status = it.Status();
  if (!status.ok()) {
    return base::ErrStatus("RUN_METRIC: Error when running file %s: %s", path,
                           status.c_message());
  }
  return base::OkStatus();
}

base::Status UnwrapMetricProto::Run(Context*,
                                    size_t argc,
                                    sqlite3_value** argv,
                                    SqlValue& out,
                                    Destructors& destructors) {
  if (argc != 2) {
    return base::ErrStatus(
        "UNWRAP_METRIC_PROTO: Expected exactly proto and message type as "
        "arguments");
  }

  SqlValue proto = sqlite_utils::SqliteValueToSqlValue(argv[0]);
  SqlValue message_type = sqlite_utils::SqliteValueToSqlValue(argv[1]);

  if (proto.type != SqlValue::Type::kBytes)
    return base::ErrStatus("UNWRAP_METRIC_PROTO: proto is not a blob");

  if (message_type.type != SqlValue::Type::kString)
    return base::ErrStatus("UNWRAP_METRIC_PROTO: message type is not string");

  const uint8_t* ptr = static_cast<const uint8_t*>(proto.AsBytes());
  size_t size = proto.bytes_count;
  if (size == 0) {
    destructors.bytes_destructor = sqlite_utils::kSqliteStatic;
    out = SqlValue::Bytes("", 0);
    return base::OkStatus();
  }

  static constexpr uint32_t kMessageType =
      static_cast<uint32_t>(protozero::proto_utils::ProtoSchemaType::kMessage);
  protozero::ConstBytes bytes;
  base::Status validation = ValidateSingleNonEmptyMessage(
      ptr, size, kMessageType, message_type.AsString(), &bytes);
  if (!validation.ok())
    return base::ErrStatus("UNWRAP_METRICS_PROTO: %s", validation.c_message());

  std::unique_ptr<uint8_t[], base::FreeDeleter> data(
      static_cast<uint8_t*>(malloc(bytes.size)));
  memcpy(data.get(), bytes.data, bytes.size);

  destructors.bytes_destructor = free;
  out = SqlValue::Bytes(data.release(), bytes.size);

  return base::OkStatus();
}

base::Status ComputeMetrics(TraceProcessor* tp,
                            const std::vector<std::string> metrics_to_compute,
                            const std::vector<SqlMetricFile>& sql_metrics,
                            const DescriptorPool& pool,
                            const ProtoDescriptor& root_descriptor,
                            std::vector<uint8_t>* metrics_proto) {
  ProtoBuilder metric_builder(&pool, &root_descriptor);
  for (const auto& name : metrics_to_compute) {
    auto metric_it =
        std::find_if(sql_metrics.begin(), sql_metrics.end(),
                     [&name](const SqlMetricFile& metric) {
                       return metric.proto_field_name.has_value() &&
                              name == metric.proto_field_name.value();
                     });
    if (metric_it == sql_metrics.end())
      return base::ErrStatus("Unknown metric %s", name.c_str());

    const auto& sql_metric = *metric_it;
    auto prep_it = tp->ExecuteQuery(sql_metric.sql);
    prep_it.Next();
    RETURN_IF_ERROR(prep_it.Status());

    auto output_query =
        "SELECT * FROM " + sql_metric.output_table_name.value() + ";";
    PERFETTO_TP_TRACE(
        metatrace::Category::QUERY, "COMPUTE_METRIC_QUERY",
        [&](metatrace::Record* r) { r->AddArg("SQL", output_query); });

    auto it = tp->ExecuteQuery(output_query.c_str());
    auto has_next = it.Next();
    RETURN_IF_ERROR(it.Status());

    // Allow the query to return no rows. This has the same semantic as an
    // empty proto being returned.
    const auto& field_name = sql_metric.proto_field_name.value();
    if (!has_next) {
      metric_builder.AppendBytes(field_name, nullptr, 0);
      continue;
    }

    if (it.ColumnCount() != 1) {
      return base::ErrStatus("Output table %s should have exactly one column",
                             sql_metric.output_table_name.value().c_str());
    }

    SqlValue col = it.Get(0);
    if (col.type != SqlValue::kBytes) {
      return base::ErrStatus("Output table %s column has invalid type",
                             sql_metric.output_table_name.value().c_str());
    }
    RETURN_IF_ERROR(metric_builder.AppendSqlValue(field_name, col));

    has_next = it.Next();
    if (has_next) {
      return base::ErrStatus("Output table %s should have at most one row",
                             sql_metric.output_table_name.value().c_str());
    }

    RETURN_IF_ERROR(it.Status());
  }
  *metrics_proto = metric_builder.SerializeRaw();
  return base::OkStatus();
}

}  // namespace metrics
}  // namespace trace_processor
}  // namespace perfetto
