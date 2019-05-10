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

#include "perfetto/base/string_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_processor/metrics/descriptors.h"
#include "src/trace_processor/metrics/metrics.descriptor.h"
#include "src/trace_processor/metrics/sql_metrics.h"

#include "perfetto/common/descriptor.pbzero.h"
#include "perfetto/metrics/android/mem_metric.pbzero.h"
#include "perfetto/metrics/metrics.pbzero.h"

namespace perfetto {
namespace trace_processor {
namespace metrics {

namespace {

// TODO(lalitm): delete this and use sqlite_utils when that is cleaned up of
// trace processor dependencies.
const char* ExtractSqliteValue(sqlite3_value* value) {
  auto type = sqlite3_value_type(value);
  PERFETTO_DCHECK(type == SQLITE_TEXT);
  return reinterpret_cast<const char*>(sqlite3_value_text(value));
}

}  // namespace

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

// SQLite function implementation used to build a proto directly in SQL. The
// proto to be built is given by the descriptor which is given as a context
// parameter to this function and chosen when this function is first registed
// with SQLite. The args of this function are key value pairs specifying the
// name of the field and its value. Nested messages are expected to be passed
// as byte blobs (as they were built recursively using this function).
// The return value is the built proto or an error about why the proto could
// not be built.
void BuildProto(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  const auto* desc = static_cast<ProtoDescriptor*>(sqlite3_user_data(ctx));
  if (argc % 2 != 0) {
    sqlite3_result_error(ctx, "Invalid call to BuildProto", -1);
    return;
  }

  protozero::ScatteredHeapBuffer delegate;
  protozero::ScatteredStreamWriter writer(&delegate);
  delegate.set_writer(&writer);

  protozero::Message message;
  message.Reset(&writer);

  for (int i = 0; i < argc; i += 2) {
    if (sqlite3_value_type(argv[i]) != SQLITE_TEXT) {
      sqlite3_result_error(ctx, "BuildProto: Invalid args", -1);
      return;
    }

    auto* key_str = reinterpret_cast<const char*>(sqlite3_value_text(argv[i]));
    auto opt_field_idx = desc->FindFieldIdx(key_str);
    const auto& field = desc->fields()[opt_field_idx.value()];

    using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
    auto* value = argv[i + 1];
    switch (field.type()) {
      case FieldDescriptorProto::TYPE_INT32:
      case FieldDescriptorProto::TYPE_INT64:
      case FieldDescriptorProto::TYPE_UINT32:
      case FieldDescriptorProto::TYPE_BOOL:
        if (sqlite3_value_type(value) != SQLITE_INTEGER) {
          sqlite3_result_error(ctx, "BuildProto: field has wrong type", -1);
          return;
        }
        message.AppendVarInt(field.number(), sqlite3_value_int64(value));
        break;
      case FieldDescriptorProto::TYPE_SINT32:
      case FieldDescriptorProto::TYPE_SINT64:
        if (sqlite3_value_type(value) != SQLITE_INTEGER) {
          sqlite3_result_error(ctx, "BuildProto: field has wrong type", -1);
          return;
        }
        message.AppendSignedVarInt(field.number(), sqlite3_value_int64(value));
        break;
      case FieldDescriptorProto::TYPE_FIXED32:
      case FieldDescriptorProto::TYPE_SFIXED32:
      case FieldDescriptorProto::TYPE_FIXED64:
      case FieldDescriptorProto::TYPE_SFIXED64:
        if (sqlite3_value_type(value) != SQLITE_INTEGER) {
          sqlite3_result_error(ctx, "BuildProto: field has wrong type", -1);
          return;
        }
        message.AppendFixed(field.number(), sqlite3_value_int64(value));
        break;
      case FieldDescriptorProto::TYPE_FLOAT:
      case FieldDescriptorProto::TYPE_DOUBLE: {
        if (sqlite3_value_type(value) != SQLITE_FLOAT) {
          sqlite3_result_error(ctx, "BuildProto: field has wrong type", -1);
          return;
        }
        double real_val = sqlite3_value_double(value);
        if (field.type() == FieldDescriptorProto::TYPE_FLOAT) {
          message.AppendFixed(field.number(), static_cast<float>(real_val));
        } else {
          message.AppendFixed(field.number(), real_val);
        }
        break;
      }
      case FieldDescriptorProto::TYPE_STRING: {
        if (sqlite3_value_type(value) != SQLITE_TEXT) {
          sqlite3_result_error(ctx, "BuildProto: field has wrong type", -1);
          return;
        }
        auto* text = reinterpret_cast<const char*>(sqlite3_value_text(value));
        message.AppendString(field.number(), text);
        break;
      }
      case FieldDescriptorProto::TYPE_MESSAGE: {
        // TODO(lalitm): verify the type of the nested message.
        if (sqlite3_value_type(value) != SQLITE_BLOB) {
          sqlite3_result_error(ctx, "BuildProto: field has wrong type", -1);
          return;
        }
        auto* blob = sqlite3_value_blob(value);
        auto bytes = static_cast<size_t>(sqlite3_value_bytes(value));
        message.AppendBytes(field.number(), blob, bytes);
        break;
      }
      case FieldDescriptorProto::TYPE_UINT64:
        sqlite3_result_error(ctx, "BuildProto: uint64_t unsupported", -1);
        return;
      case FieldDescriptorProto::TYPE_GROUP:
        sqlite3_result_error(ctx, "BuildProto: groups unsupported", -1);
        return;
      case FieldDescriptorProto::TYPE_ENUM:
        // TODO(lalit): add support for enums.
        sqlite3_result_error(ctx, "BuildProto: enums unsupported", -1);
        return;
    }
  }
  message.Finalize();

  auto slices = delegate.StitchSlices();
  std::unique_ptr<uint8_t[]> data(static_cast<uint8_t*>(malloc(slices.size())));
  memcpy(data.get(), slices.data(), slices.size());
  sqlite3_result_blob(ctx, data.release(), static_cast<int>(slices.size()),
                      free);
}

void RunMetric(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  auto* tp = static_cast<TraceProcessor*>(sqlite3_user_data(ctx));
  if (argc == 0 || sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
    sqlite3_result_error(ctx, "RUN_METRIC: Invalid arguments", -1);
    return;
  }

  const char* filename =
      reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
  const char* sql = sql_metrics::GetBundledMetric(filename);
  if (!sql) {
    sqlite3_result_error(ctx, "RUN_METRIC: Unknown filename provided", -1);
    return;
  }

  std::unordered_map<std::string, std::string> substitutions;
  for (int i = 1; i < argc; i += 2) {
    if (sqlite3_value_type(argv[i]) != SQLITE_TEXT) {
      sqlite3_result_error(ctx, "RUN_METRIC: Invalid args", -1);
      return;
    }

    auto* key_str = ExtractSqliteValue(argv[i]);
    auto* value_str = ExtractSqliteValue(argv[i + 1]);
    substitutions[key_str] = value_str;
  }

  for (const auto& query : base::SplitString(sql, ";\n")) {
    std::string buffer;
    int ret = TemplateReplace(query, substitutions, &buffer);
    if (ret) {
      sqlite3_result_error(
          ctx, "RUN_METRIC: Error when performing substitution", -1);
      return;
    }

    PERFETTO_DLOG("RUN_METRIC: Executing query: %s", buffer.c_str());
    auto it = tp->ExecuteQuery(buffer);
    util::Status status = it.Status();
    if (!status.ok()) {
      char* error =
          sqlite3_mprintf("RUN_METRIC: Error when running file %s: %s",
                          filename, status.c_message());
      sqlite3_result_error(ctx, error, -1);
      sqlite3_free(error);
      return;
    } else if (it.Next()) {
      sqlite3_result_error(
          ctx, "RUN_METRIC: functions should not produce any output", -1);
      return;
    }
  }
}

util::Status ComputeMetrics(TraceProcessor* tp,
                            const std::vector<std::string>& metric_names,
                            std::vector<uint8_t>* metrics_proto) {
  // TODO(lalitm): stop hardcoding android.mem metric and read the proto
  // descriptor for this logic instead.
  if (metric_names.size() != 1 || metric_names[0] != "android.mem")
    return util::ErrStatus("Only android.mem metric is currently supported");

  auto queries = base::SplitString(sql_metrics::kAndroidMem, ";\n");
  for (const auto& query : queries) {
    PERFETTO_DLOG("Executing query: %s", query.c_str());
    auto prep_it = tp->ExecuteQuery(query);
    prep_it.Next();

    util::Status status = prep_it.Status();
    if (!status.ok())
      return status;
  }

  auto it = tp->ExecuteQuery("SELECT * from AndroidMemOutput;");
  auto has_next = it.Next();
  util::Status status = it.Status();
  if (!status.ok()) {
    return status;
  } else if (!has_next) {
    return util::ErrStatus("Output table should have at least one row");
  } else if (it.ColumnCount() != 1) {
    return util::ErrStatus("Output table should have exactly one column");
  } else if (it.Get(0).type != SqlValue::kBytes) {
    return util::ErrStatus("Output table column should have type bytes");
  }

  const uint8_t* ptr = static_cast<const uint8_t*>(it.Get(0).bytes_value);
  *metrics_proto = std::vector<uint8_t>(ptr, ptr + it.Get(0).bytes_count);

  has_next = it.Next();
  if (has_next)
    return util::ErrStatus("Output table should only have one row");
  return it.Status();
}

}  // namespace metrics
}  // namespace trace_processor
}  // namespace perfetto
