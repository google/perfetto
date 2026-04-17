/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PARSER_INTRINSIC_MACRO_EXPANSION_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PARSER_INTRINSIC_MACRO_EXPANSION_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"

namespace perfetto::trace_processor::perfetto_sql {

struct ExpandResult {
  enum : uint8_t {
    // Name matched an intrinsic and `body` holds the expansion text.
    kExpanded,
    // Name is not an intrinsic; caller should try the user macro registry.
    kNotIntrinsic,
    // Name matched an intrinsic but expansion failed (e.g. wrong arg count
    // or malformed token list); caller should surface a failure to the
    // syntaqlite parser.
    kExpansionFailed,
  };
  uint8_t status;
  // Populated only when `status == kExpanded`.
  std::string body;
};

// Tries to expand a PerfettoSQL-builtin intrinsic macro. These are a small
// fixed set of preprocessor-compat shims (`__intrinsic_stringify`,
// `__intrinsic_token_apply{,_prefix,_and,_and_prefix}`) which operate on raw
// token text rather than via the syntaqlite $param mechanism.
ExpandResult TryExpandIntrinsicMacro(std::string_view name,
                                     const SyntaqliteToken* args,
                                     uint32_t arg_count);

}  // namespace perfetto::trace_processor::perfetto_sql

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PARSER_INTRINSIC_MACRO_EXPANSION_H_
