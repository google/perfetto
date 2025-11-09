// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/trace_processor/perfetto_sql/intrinsics/functions/args.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/ui/args.pbzero.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/args_utils.h"
#include "src/trace_processor/util/json_writer.h"

namespace perfetto::trace_processor {
namespace {

void WriteVariadic(const Variadic& v,
                   const TraceStorage* storage,
                   json::JsonValueWriter&& writer) {
  switch (v.type) {
    case Variadic::Type::kNull:
      std::move(writer).WriteNull();
      break;
    case Variadic::Type::kBool:
      std::move(writer).WriteBool(v.bool_value);
      break;
    case Variadic::Type::kInt:
      std::move(writer).WriteInt(v.int_value);
      break;
    case Variadic::Type::kUint:
      std::move(writer).WriteUint(v.uint_value);
      break;
    case Variadic::Type::kReal:
      std::move(writer).WriteDouble(v.real_value);
      break;
    case Variadic::Type::kString: {
      if (v.string_value.is_null()) {
        std::move(writer).WriteNull();
        break;
      }
      NullTermStringView str = storage->GetString(v.string_value);
      std::move(writer).WriteString(str.c_str());
      break;
    }
    case Variadic::Type::kPointer: {
      std::move(writer).WriteString(base::Uint64ToHexString(v.pointer_value));
      break;
    }
    case Variadic::Type::kJson: {
      // For JSON values, we need to parse and reconstruct them properly
      // For now, just treat as string
      if (v.json_value.is_null()) {
        std::move(writer).WriteNull();
        break;
      }
      NullTermStringView str = storage->GetString(v.json_value);
      std::move(writer).WriteString(str.c_str());
      break;
    }
  }
}

void WriteArgNode(const ArgNode& node,
                  const TraceStorage* storage,
                  json::JsonValueWriter&& writer);

void WriteArgNode(const ArgNode& node,
                  const TraceStorage* storage,
                  json::JsonArrayWriter& writer);

void WriteArgNode(const ArgNode& node,
                  const TraceStorage* storage,
                  json::JsonDictWriter& writer,
                  std::string_view key);

void WriteArgNode(const ArgNode& node,
                  const TraceStorage* storage,
                  json::JsonValueWriter&& writer) {
  switch (node.GetType()) {
    case ArgNode::Type::kPrimitive:
      WriteVariadic(node.GetPrimitiveValue(), storage, std::move(writer));
      break;
    case ArgNode::Type::kArray:
      std::move(writer).WriteArray(
          [&node, storage](json::JsonArrayWriter& arr) {
            for (const auto& child : node.GetArray()) {
              WriteArgNode(child, storage, arr);
            }
          });
      break;
    case ArgNode::Type::kDict:
      std::move(writer).WriteDict([&node, storage](json::JsonDictWriter& dict) {
        for (const auto& [k, v] : node.GetDict()) {
          WriteArgNode(v, storage, dict, k);
        }
      });
      break;
  }
}

void WriteArgNode(const ArgNode& node,
                  const TraceStorage* storage,
                  json::JsonArrayWriter& writer) {
  writer.Append([&node, storage](json::JsonValueWriter&& value_writer) {
    WriteArgNode(node, storage, std::move(value_writer));
  });
}

void WriteArgNode(const ArgNode& node,
                  const TraceStorage* storage,
                  json::JsonDictWriter& writer,
                  std::string_view key) {
  writer.Add(key, [&node, storage](json::JsonValueWriter&& value_writer) {
    WriteArgNode(node, storage, std::move(value_writer));
  });
}

inline bool ExtractArgsRowFromSqlArgs(sqlite3_context* ctx,
                                      sqlite3_value* arg_set_value,
                                      sqlite3_value* key_value,
                                      TraceStorage* storage,
                                      uint32_t& result) {
  sqlite::Type arg_set_value_type = sqlite::value::Type(arg_set_value);
  sqlite::Type key_value_type = sqlite::value::Type(key_value);

  // If the arg set id is null, just return null as the result.
  if (arg_set_value_type == sqlite::Type::kNull) {
    return false;
  }

  if (arg_set_value_type != sqlite::Type::kInteger) {
    sqlite::result::Error(ctx,
                          "EXTRACT_ARG: 1st argument should be arg set id");
    return false;
  }

  if (key_value_type != sqlite::Type::kText) {
    sqlite::result::Error(ctx, "EXTRACT_ARG: 2nd argument should be key");
    return false;
  }

  uint32_t arg_set_id =
      static_cast<uint32_t>(sqlite::value::Int64(arg_set_value));
  const char* key =
      reinterpret_cast<const char*>(sqlite::value::Text(key_value));

  result = storage->ExtractArgRowFast(arg_set_id, key);
  if (result == std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  return true;
}

}  // namespace

// static
void ExtractArg::Step(sqlite3_context* ctx, int, sqlite3_value** argv) {
  TraceStorage* storage = GetUserData(ctx);

  uint32_t row;
  if (!ExtractArgsRowFromSqlArgs(ctx, argv[0], argv[1], storage, row)) {
    return;
  }

  auto rr = storage->arg_table()[row];
  switch (*storage->GetVariadicTypeForId(rr.value_type())) {
    case Variadic::Type::kBool:
    case Variadic::Type::kInt:
    case Variadic::Type::kUint:
    case Variadic::Type::kPointer:
      return sqlite::result::Long(ctx, *rr.int_value());
    case Variadic::Type::kJson:
    case Variadic::Type::kString: {
      auto opt_string_id = rr.string_value();
      if (!opt_string_id.has_value() || opt_string_id->is_null()) {
        sqlite::result::Null(ctx);
        return;
      }
      NullTermStringView value = storage->GetString(*opt_string_id);
      return sqlite::result::StaticString(ctx, value.c_str());
    }
    case Variadic::Type::kReal:
      return sqlite::result::Double(ctx, *rr.real_value());
    case Variadic::Type::kNull:
      return;
  }
}

// static
void SerialiseArg::Step(sqlite3_context* ctx, int, sqlite3_value** argv) {
  TraceStorage* storage = GetUserData(ctx);

  uint32_t row;
  if (!ExtractArgsRowFromSqlArgs(ctx, argv[0], argv[1], storage, row)) {
    return;
  }
  const auto rr = storage->arg_table()[row];
  protozero::HeapBuffered<protos::pbzero::ArgValue> arg_proto;
  const auto variadic_type = *storage->GetVariadicTypeForId(rr.value_type());
  switch (variadic_type) {
    case Variadic::Type::kNull:
      arg_proto->set_type(protos::pbzero::ArgValue::TYPE_NULL);
      break;
    case Variadic::Type::kBool:
      arg_proto->set_type(protos::pbzero::ArgValue::TYPE_BOOL);
      arg_proto->set_int_value(*rr.int_value());
      break;
    case Variadic::Type::kInt:
    case Variadic::Type::kUint:
      arg_proto->set_type(protos::pbzero::ArgValue::TYPE_INT);
      arg_proto->set_int_value(*rr.int_value());
      break;

    case Variadic::Type::kPointer:
      arg_proto->set_type(protos::pbzero::ArgValue::TYPE_POINTER);
      arg_proto->set_int_value(*rr.int_value());
      break;
    case Variadic::Type::kReal:
      arg_proto->set_type(protos::pbzero::ArgValue::TYPE_REAL);
      arg_proto->set_real_value(*rr.real_value());
      break;
    case Variadic::Type::kString:
    case Variadic::Type::kJson: {
      arg_proto->set_type(protos::pbzero::ArgValue::TYPE_STRING);
      auto opt_string_id = rr.string_value();
      if (opt_string_id.has_value() && !opt_string_id->is_null()) {
        NullTermStringView value = storage->GetString(*opt_string_id);
        arg_proto->set_string_value(value.data(), value.size());
      }
      break;
    }
  }
  const auto data = arg_proto.SerializeAsArray();
  return sqlite::result::TransientBytes(ctx, data.data(), static_cast<int>(data.size()));
}

// static
void ArgSetToJson::Step(sqlite3_context* ctx, int, sqlite3_value** argv) {
  sqlite::Type arg_set_value = sqlite::value::Type(argv[0]);
  if (arg_set_value == sqlite::Type::kNull) {
    return;
  }
  if (arg_set_value != sqlite::Type::kInteger) {
    return sqlite::result::Error(
        ctx, "PRINT_ARGS: 1st argument should be arg set id");
  }
  uint32_t arg_set_id = static_cast<uint32_t>(sqlite::value::Int64(argv[0]));

  auto* user_data = GetUserData(ctx);
  auto* storage = user_data->storage;
  const auto& arg_table = storage->arg_table();

  // Reuse cursor - just update the filter value
  user_data->arg_cursor.SetFilterValueUnchecked(0, arg_set_id);

  // Reuse arg_set - clear but retain capacity
  user_data->arg_set.Clear();
  for (user_data->arg_cursor.Execute(); !user_data->arg_cursor.Eof();
       user_data->arg_cursor.Next()) {
    const auto row_number = user_data->arg_cursor.ToRowNumber();
    const auto row = row_number.ToRowReference(arg_table);

    const auto result = user_data->arg_set.AppendArg(
        storage->GetString(row.key()),
        storage->GetArgValue(row_number.row_number()));
    if (!result.ok()) {
      return sqlite::result::Error(ctx, result.c_message());
    }
  }

  // Reuse json_writer - clear but retain capacity
  user_data->json_writer.Clear();
  json::JsonValueWriter(user_data->json_writer)
      .WriteDict([&](json::JsonDictWriter& writer) {
        for (const auto& [key, value] : user_data->arg_set.root().GetDict()) {
          WriteArgNode(value, storage, writer, key);
        }
      });
  auto json_sv = user_data->json_writer.GetStringView();
  return sqlite::result::TransientString(ctx, json_sv.data(),
                                         static_cast<int>(json_sv.size()));
}

}  // namespace perfetto::trace_processor
