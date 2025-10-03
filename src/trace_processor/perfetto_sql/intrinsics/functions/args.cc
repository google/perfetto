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

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
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
    case Variadic::Type::kString:
      std::move(writer).WriteString(storage->GetString(v.string_value).c_str());
      break;
    case Variadic::Type::kPointer: {
      std::move(writer).WriteString(base::Uint64ToHexString(v.pointer_value));
      break;
    }
    case Variadic::Type::kJson:
      // For JSON values, we need to parse and reconstruct them properly
      // For now, just treat as string
      std::move(writer).WriteString(storage->GetString(v.json_value).c_str());
      break;
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

}  // namespace

// static
void ExtractArg::Step(sqlite3_context* ctx, int, sqlite3_value** argv) {
  sqlite::Type arg_set_value = sqlite::value::Type(argv[0]);
  sqlite::Type key_value = sqlite::value::Type(argv[1]);

  // If the arg set id is null, just return null as the result.
  if (arg_set_value == sqlite::Type::kNull) {
    return;
  }

  if (arg_set_value != sqlite::Type::kInteger) {
    return sqlite::result::Error(
        ctx, "EXTRACT_ARG: 1st argument should be arg set id");
  }

  if (key_value != sqlite::Type::kText) {
    return sqlite::result::Error(ctx,
                                 "EXTRACT_ARG: 2nd argument should be key");
  }

  uint32_t arg_set_id = static_cast<uint32_t>(sqlite::value::Int64(argv[0]));
  const char* key = reinterpret_cast<const char*>(sqlite::value::Text(argv[1]));

  auto* storage = GetUserData(ctx);
  uint32_t row = storage->ExtractArgRowFast(arg_set_id, key);
  if (row == std::numeric_limits<uint32_t>::max()) {
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
    case Variadic::Type::kString:
      return sqlite::result::StaticString(
          ctx, storage->GetString(rr.string_value()).c_str());
    case Variadic::Type::kReal:
      return sqlite::result::Double(ctx, *rr.real_value());
    case Variadic::Type::kNull:
      return;
  }
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

  auto* storage = GetUserData(ctx);
  const auto& arg_table = storage->arg_table();
  auto cursor = arg_table.CreateCursor({dataframe::FilterSpec{
      tables::ArgTable::ColumnIndex::arg_set_id,
      0,
      dataframe::Eq{},
      std::nullopt,
  }});
  cursor.SetFilterValueUnchecked(0, arg_set_id);
  ArgSet arg_set;
  for (cursor.Execute(); !cursor.Eof(); cursor.Next()) {
    const auto row_number = cursor.ToRowNumber();
    const auto row = row_number.ToRowReference(arg_table);

    const auto result =
        arg_set.AppendArg(storage->GetString(row.key()),
                          storage->GetArgValue(row_number.row_number()));
    if (!result.ok()) {
      return sqlite::result::Error(ctx, result.c_message());
    }
  }
  std::string result = json::Write([&](json::JsonValueWriter&& json_writer) {
    std::move(json_writer).WriteDict([&](json::JsonDictWriter& writer) {
      for (const auto& [key, value] : arg_set.root().GetDict()) {
        WriteArgNode(value, storage, writer, key);
      }
    });
  });
  return sqlite::result::TransientString(ctx, result.c_str());
}

}  // namespace perfetto::trace_processor
