/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/functions/replace_numbers_function.h"

#include <stdlib.h>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/basic_types.h"
#include "protos/perfetto/trace_processor/stack.pbzero.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_engine.h"
#include "src/trace_processor/perfetto_sql/intrinsics/functions/sql_function.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {
namespace {

// __intrinsic_strip_hex(name STRING, min_repeated_digits LONG)
//
//   Replaces hexadecimal sequences (with at least one digit) in a string with
//   "<num>" based on specified criteria.
struct StripHexFunction : public SqlFunction {
  static constexpr char kFunctionName[] = "__intrinsic_strip_hex";
  static constexpr std::array<std::string_view, 2> kSpecialPrefixes = {"0x",
                                                                       "0X"};

  using Context = void;

  static base::Status Run(void* cxt,
                          size_t argc,
                          sqlite3_value** argv,
                          SqlValue& out,
                          Destructors& destructors) {
    base::Status status = RunImpl(cxt, argc, argv, out, destructors);
    if (!status.ok()) {
      return base::ErrStatus("%s: %s", kFunctionName, status.message().c_str());
    }
    return status;
  }

  static const std::string_view MatchesSpecialPrefix(
      base::StringView input_view,
      size_t pos) {
    for (const auto& special_prefix : kSpecialPrefixes) {
      if (input_view.substr(pos, special_prefix.size()) ==
          special_prefix.data()) {
        return special_prefix;
      }
    }
    return "";
  }

  static std::string StripHex(std::string input, int64_t min_repeated_digits) {
    base::StringView input_view = base::StringView(input);
    std::string result;
    result.reserve(input.length());
    for (size_t i = 0; i < input.length();) {
      const std::string_view special_prefix =
          MatchesSpecialPrefix(input_view, i);
      if (!special_prefix.empty()) {
        // Case 1: Special prefixes for hex sequence found
        result += input.substr(i, special_prefix.size());
        i += special_prefix.size();
      } else if (!isalnum(input[i])) {
        // Case 2: Non alpha numeric prefix for hex sequence found
        result += input[i++];
      } else if (i == 0 && isxdigit(input[i])) {
        // Case 3: Start of input is hex digit, continue to check hex sequence
      } else {
        // Case 4: No potential prefix for hex digits found
        result += input[i++];
        continue;
      }

      size_t hex_start = i;
      bool digit_found = false;
      for (; i < input.length() && isxdigit(input[i]); i++) {
        if (isdigit(input[i])) {
          digit_found = true;
        }
      }
      result += digit_found && (i - hex_start >=
                                static_cast<size_t>(min_repeated_digits))
                    ? "<num>"
                    : input.substr(hex_start, i - hex_start);
    }
    return result;
  }

  static base::Status RunImpl(void*,
                              size_t argc,
                              sqlite3_value** argv,
                              SqlValue& out,
                              Destructors& destructors) {
    if (argc != 2) {
      return base::ErrStatus(
          "%s; Invalid number of arguments: expected 2, actual %zu",
          kFunctionName, argc);
    }
    std::optional<std::string> first_arg = sqlite::utils::SqlValueToString(
        sqlite::utils::SqliteValueToSqlValue(argv[0]));
    if (!first_arg.has_value()) {
      return base::ErrStatus("Invalid name argument for %s expected string",
                             kFunctionName);
    }
    const std::string& input = first_arg.value();

    SqlValue second_arg = sqlite::utils::SqliteValueToSqlValue(argv[1]);
    if (second_arg.type != SqlValue::Type::kLong) {
      return base::ErrStatus(
          "Invalid min_repeated_digits argument for %s expected integer",
          kFunctionName);
    }

    const int64_t min_repeated_digits = second_arg.AsLong();
    if (min_repeated_digits < 0) {
      return base::ErrStatus(
          "Invalid min_repeated_digits argument for %s expected positive "
          "integer",
          kFunctionName);
    }

    std::string result = StripHex(input, min_repeated_digits);
    char* result_cstr = static_cast<char*>(malloc(result.length() + 1));
    memcpy(result_cstr, result.c_str(), result.length() + 1);
    out = SqlValue::String(result_cstr);
    destructors.string_destructor = free;
    return base::OkStatus();
  }
};

}  // namespace

base::Status RegisterStripHexFunction(PerfettoSqlEngine* engine,
                                      TraceProcessorContext* context) {
  return engine->RegisterStaticFunction<StripHexFunction>(
      StripHexFunction::kFunctionName, 2, context->storage.get());
}

std::string SqlStripHex(std::string input, int64_t min_repeated_digits) {
  return StripHexFunction::StripHex(input, min_repeated_digits);
}

}  // namespace trace_processor
}  // namespace perfetto
