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
#include "src/trace_processor/core/exec/param_value.h"

namespace perfetto::trace_processor {
class StringPool;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::pipeline {

class Compiler;
class PlanCodec;

// A value written in a query: a number, a string, or a bound param, which
// stands for one of the values a run binds.
struct Literal {
  enum class Type : uint8_t { kInt64, kDouble, kString, kBound };

  static Literal Bound(uint32_t index) {
    Literal literal;
    literal.type = Type::kBound;
    literal.bound = index;
    return literal;
  }

  bool is_double() const { return type == Type::kDouble; }
  bool is_string() const { return type == Type::kString; }
  bool is_bound() const { return type == Type::kBound; }

  Type type = Type::kInt64;
  int64_t int_value = 0;
  double double_value = 0;
  std::string text;
  uint32_t bound = 0;
};

// Reads the number, optionally negated, or string at `expr`.
base::StatusOr<Literal> CompileLiteral(Compiler*, uint32_t expr);

// Casts the `count` literals `op` compares with for a column of storage `type`,
// as the dataframe's filters cast them.
core::exec::ParamValue CastLiterals(const core::Op& op,
                                    const Literal* literals,
                                    uint32_t count,
                                    core::StorageType type,
                                    const StringPool& pool);

// Writes or reads `literal`.
void SerializeLiteral(PlanCodec&, Literal* literal);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LITERAL_H_
