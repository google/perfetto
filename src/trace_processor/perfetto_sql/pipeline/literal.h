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
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/filter_value.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor {
class StringPool;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::pipeline {

class Compiler;

// What WHERE and expressions share, so a condition means one thing in both.

// A literal a condition compares with: a number or a string.
struct ConditionLiteral {
  enum class Type : uint8_t { kInt64, kDouble, kString };

  Type type = Type::kInt64;
  // Explicit, so a plan's literals can be copied as bytes.
  uint8_t pad_[7] = {};
  // The integer, or for a string its index in the strings kept beside it.
  int64_t int_value = 0;
  double double_value = 0;
};
static_assert(sizeof(ConditionLiteral) == 24);
template <>
inline constexpr bool kCopiedAsBytes<ConditionLiteral> = true;

// Reads the number literal, optionally negated, or string literal at `expr`.
base::Status CompileConditionLiteral(Compiler*,
                                     uint32_t expr,
                                     std::vector<ConditionLiteral>* literals,
                                     std::vector<std::string>* strings);

// The comparison `op` is, if it is one.
std::optional<core::Op> ComparisonOp(SyntaqliteBinaryOp op);

// The test the IS at `expr` is, if it is IS [NOT] NULL.
base::StatusOr<core::Op> NullTestOp(Compiler*,
                                    uint32_t expr,
                                    const SyntaqliteIsExpr& node);

// What `op` should compare with to suit a column of storage `type`, if a
// literal is a string where the column holds numbers, or the other way around.
// GLOB and IS [NOT] NULL do not compare values.
std::optional<std::string> LiteralMismatch(const core::Op& op,
                                           const ConditionLiteral* literals,
                                           uint32_t count,
                                           core::StorageType type);

// Casts the literals `op` compares with for a column of storage `type`, as the
// dataframe's filters cast them. `strings` holds the text of string literals.
core::exec::FilterValue CastConditionLiterals(
    const core::Op& op,
    const ConditionLiteral* literals,
    uint32_t count,
    const std::vector<std::string>& strings,
    core::StorageType type,
    const StringPool& pool);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_LITERAL_H_
