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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_EXPRESSION_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_EXPRESSION_H_

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/selection.h"

namespace perfetto::trace_processor::core::exec {

// The type of the values an expression step computes. Truth values are Int64
// 0 or 1, as in SQLite.
enum class ExprType : uint8_t { kInt64, kDouble, kString };

// One step of an ExprProgram. Step `i` computes register `i` from the batch
// or from earlier registers.
struct ExprStep {
  enum class Op : uint8_t {
    // Reads batch column `a`. Integer columns of any width read as Int64.
    kColumn,
    // A constant: `int_value`, `double_value` or `string_value` by `type`.
    kConstant,
    // A null of `type`.
    kNull,
    kNeg,
    kNot,
    // Int64 to Double.
    kToDouble,
    kAdd,
    kSub,
    kMul,
    kDiv,
    kMod,
    kEq,
    kNe,
    kLt,
    kLe,
    kGt,
    kGe,
    kAnd,
    kOr,
    kIsNull,
    kIsNotNull,
  };

  Op op = Op::kColumn;
  // The type of the value computed.
  ExprType type = ExprType::kInt64;
  // The type of the operands. Both operands of a binary step have it.
  ExprType operand_type = ExprType::kInt64;
  // The registers read, or for kColumn the batch column.
  uint32_t a = 0;
  uint32_t b = 0;
  int64_t int_value = 0;
  double double_value = 0;
  StringPool::Id string_value = StringPool::Id::Null();
};

// A typed expression, flattened so that each step reads only steps before
// it. The last step is the result.
//
// Every step is typed when the plan is built, so evaluating one dispatches on
// its op and types once per batch and then runs a loop over the batch's kept
// rows. Nulls follow SQL: any null operand gives null, except for AND, OR and
// IS [NOT] NULL. Division or remainder by zero gives null, as in SQLite. An
// integer overflow, which SQLite would answer with a double, fails the run.
struct ExprProgram {
  std::vector<ExprStep> steps;
};

// Appends one column to each batch for each program, its value at every kept
// row.
class Compute final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit Compute(std::vector<ExprProgram> programs);
  ~Compute() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool Process(RowBatch& batch, OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  std::vector<ExprProgram> programs_;
};

// Keeps the rows of each batch for which the program is true: neither null
// nor zero. Nothing is copied: the batch's selection is narrowed.
class Where final : public Transform {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit Where(ExprProgram program);
  ~Where() override;

  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  bool Process(RowBatch& batch, OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

 private:
  friend class PipelineFormatter;

  ExprProgram program_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_EXPRESSION_H_
