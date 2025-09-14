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

#include "perfetto/ext/base/dynamic_string_writer.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/util/args_utils.h"

namespace perfetto::trace_processor {
namespace {
// We care about the argument order, so we can't use jsoncpp here.
void WriteAsJson(const ArgNode& node,
                 const TraceStorage* storage,
                 base::DynamicStringWriter& writer) {
  switch (node.GetType()) {
    case ArgNode::Type::kPrimitive: {
      Variadic v = node.GetPrimitiveValue();
      switch (v.type) {
        case Variadic::Type::kNull:
          writer.AppendLiteral("null");
          break;
        case Variadic::Type::kBool:
          writer.AppendString(v.bool_value ? "true" : "false");
          break;
        case Variadic::Type::kInt:
          writer.AppendInt(v.int_value);
          break;
        case Variadic::Type::kUint:
          writer.AppendUnsignedInt(v.uint_value);
          break;
        case Variadic::Type::kReal:
          if (std::isnan(v.real_value)) {
            writer.AppendLiteral("\"NaN\"");
          } else if (std::isinf(v.real_value) && v.real_value > 0) {
            writer.AppendLiteral("\"Infinity\"");
          } else if (std::isinf(v.real_value) && v.real_value < 0) {
            writer.AppendLiteral("\"-Infinity\"");
          } else {
            writer.AppendDouble(v.real_value);
          }
          break;
        case Variadic::Type::kString:
          writer.AppendChar('"');
          for (const char* p = storage->GetString(v.string_value).c_str(); *p;
               p++) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (*p == '"') {
              writer.AppendLiteral("\\\"");
            } else if (*p == '\\') {
              writer.AppendLiteral("\\\\");
            } else if (*p == '\n') {
              writer.AppendLiteral("\\n");
            } else if (*p == '\r') {
              writer.AppendLiteral("\\r");
            } else if (*p == '\t') {
              writer.AppendLiteral("\\t");
            } else if (c < 0x20) {
              // Escape all control characters below 0x20 in \uXXXX format
              writer.AppendLiteral("\\u00");
              writer.AppendChar("0123456789abcdef"[c >> 4]);
              writer.AppendChar("0123456789abcdef"[c & 0xf]);
            } else {
              writer.AppendChar(*p);
            }
          }
          writer.AppendChar('"');
          break;
        case Variadic::Type::kPointer:
          writer.AppendChar('"');
          writer.AppendString(
              base::StringView(base::Uint64ToHexString(v.pointer_value)));
          writer.AppendChar('"');
          break;
        case Variadic::Type::kJson:
          writer.AppendString(storage->GetString(v.json_value).c_str());
          break;
      }
      break;
    }
    case ArgNode::Type::kArray: {
      writer.AppendChar('[');
      const auto& array = node.GetArray();
      for (size_t i = 0; i < array.size(); i++) {
        if (i > 0)
          writer.AppendChar(',');
        WriteAsJson(array[i], storage, writer);
      }
      writer.AppendChar(']');
      break;
    }
    case ArgNode::Type::kDict: {
      writer.AppendChar('{');
      const auto& dict = node.GetDict();
      for (size_t i = 0; i < dict.size(); i++) {
        if (i > 0)
          writer.AppendChar(',');
        writer.AppendChar('"');
        writer.AppendString(dict[i].first.c_str());
        writer.AppendLiteral("\":");
        WriteAsJson(dict[i].second, storage, writer);
      }
      writer.AppendChar('}');
      break;
    }
  }
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
void PrintArgs::Step(sqlite3_context* ctx, int, sqlite3_value** argv) {
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
        arg_set.AppendArg(storage->GetString(row.key()).ToStdString(),
                          storage->GetArgValue(row_number.row_number()));
    if (!result.ok()) {
      return sqlite::result::Error(ctx, result.c_message());
    }
  }
  base::DynamicStringWriter writer;
  WriteAsJson(arg_set.root(), storage, writer);
  std::string result = writer.GetStringView().ToStdString();
  return sqlite::result::TransientString(ctx, result.c_str());
}

}  // namespace perfetto::trace_processor