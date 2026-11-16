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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LITERAL_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LITERAL_H_

#include <cstdint>
#include <string>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/filter_value.h"

namespace perfetto::trace_processor {
class StringPool;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::pipeline {

class Compiler;
class PlanCodec;

// A value written in a query: a number or a string. Unlike a filter value,
// which a run binds, a literal is part of the plan and never changes.
struct Literal {
  enum class Type : uint8_t { kInt64, kDouble, kString };

  bool is_double() const { return type == Type::kDouble; }
  bool is_string() const { return type == Type::kString; }

  Type type = Type::kInt64;
  int64_t int_value = 0;
  double double_value = 0;
  std::string text;
};

// Reads the number, optionally negated, or string at `expr`.
base::StatusOr<Literal> CompileLiteral(Compiler*, uint32_t expr);

// Casts the `count` literals `op` compares with for a column of storage `type`,
// as the dataframe's filters cast them.
core::exec::FilterValue CastLiterals(const core::Op& op,
                                     const Literal* literals,
                                     uint32_t count,
                                     core::StorageType type,
                                     const StringPool& pool);

// What comparing a value with a literal comes to, once the literal is of the
// value's type. Comparing an integer with 2.5, say, is decided without 2.5: `n
// = 2.5` is never true, and `n > 2.5` is `n > 2`.
struct ResolvedComparison {
  enum class Kind : uint8_t {
    // Compare with `literal`.
    kCompare,
    // True or false for every value which is not null.
    kAlwaysTrue,
    kAlwaysFalse,
  };
  Kind kind = Kind::kCompare;
  Literal literal;
};

// Resolves comparing a number with the number `literal` by `op`, one of =, !=,
// <,
// <=, > and >=. The value is a double if `double_value`, else an integer. Means
// what a filter on a column of that type means.
ResolvedComparison ResolveLiteralComparison(const core::Op& op,
                                            const Literal& literal,
                                            bool double_value);

// Writes or reads `literal`.
void SerializeLiteral(PlanCodec&, Literal* literal);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LITERAL_H_
