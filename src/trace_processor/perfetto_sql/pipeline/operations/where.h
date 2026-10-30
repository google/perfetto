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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_WHERE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_WHERE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/perfetto_sql/pipeline/logical_plan.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

class Lowering;
class PlanNode;

// A literal a WHERE condition compares with: a number or a string.
struct WhereLiteral {
  enum class Type : uint8_t { kInt64, kDouble, kString };

  Type type = Type::kInt64;
  // Explicit, so a plan's literals can be copied as bytes.
  uint8_t pad_[7] = {};
  // The integer, or for a string its index in the WHERE's strings.
  int64_t int_value = 0;
  double double_value = 0;
};
static_assert(sizeof(WhereLiteral) == 24);
template <>
inline constexpr bool kCopiedAsBytes<WhereLiteral> = true;

// A test of one column: `column OP literal` for OP one of =, !=, <, <=, >, >=
// or GLOB, `column IN (literal, ...)`, or `column IS [NOT] NULL`. A WHERE
// condition is the AND of these. Literals are compared as SQL compares them
// with the column's type, whatever that is.
struct WherePredicate {
  using Op = core::Op;

  ColumnId column = 0;
  Op op{core::Eq{}};
  // The literals compared with: none for IS [NOT] NULL, the list for IN and
  // one otherwise.
  uint32_t first_literal = 0;
  uint32_t literal_count = 0;
};
static_assert(sizeof(WherePredicate) == 16);
template <>
inline constexpr bool kCopiedAsBytes<WherePredicate> = true;

// `|> WHERE condition`. The condition must be a conjunction (AND) of
// predicates (see WherePredicate); anything else is unsupported.
class Where : public PlanOperation {
 public:
  static const OperationRegistration kRegistration;

 private:
  // Test-only formatting; keep payload details out of the public interface.
  friend class LogicalPlanFormatter;

  // PlanOperation implementation. Dispatch goes through the base interface.
  std::unique_ptr<PlanOperation> Clone() const override;
  std::optional<uint32_t> Prune(std::vector<bool>* needed) override;
  void Lower(Lowering*, const PlanNode&) const override;
  void Serialize(PlanCodec&) override;

  static base::Status BuildPlan(Compiler*, uint32_t);

  const OperationRegistration& registration() const override;

  std::vector<WherePredicate> predicates_;
  std::vector<WhereLiteral> literals_;
  std::vector<std::string> strings_;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_WHERE_H_
