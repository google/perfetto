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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_EXPRESSION_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_EXPRESSION_H_

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/exec/expression.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor {
class StringPool;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::pipeline {

class Compiler;
class Lowering;
class PlanReader;
class PlanWriter;

// A scalar expression over the columns of a row, as a stage computes it:
// column references, literals, arithmetic, comparisons, AND, OR, NOT and
// IS [NOT] NULL.
//
// Only expressions whose meaning is the same as SQLite's are accepted.
// Anything else is refused when the plan is built, not answered
// differently: comparing a number with a string, ordering strings, and
// arithmetic on strings are errors, and an integer overflow fails the query
// where SQLite would answer with a double.
struct Expr {
  using Op = core::exec::ExprStep::Op;
  using Type = core::exec::ExprType;

  // kColumn, kConstant and kNull are leaves; the rest have `args`.
  Op op = Op::kColumn;
  ColumnId column = 0;
  // For kConstant, which of the values it is.
  Type literal_type = Type::kInt64;
  int64_t int_value = 0;
  double double_value = 0;
  std::string string_value;
  std::vector<Expr> args;
};

// Compiles the expression at AST node `expr` against the row in scope.
base::StatusOr<Expr> CompileExpr(Compiler*, uint32_t expr);

// The type of `expr`, checked as far as the column types known when the plan
// is built allow. Unknown if it reads a column of unknown type. A failure
// names what was expected, for Compiler::Expected.
base::StatusOr<PlanType> CheckExprType(Compiler*, const Expr&);

// Marks the columns `expr` reads.
void MarkExprColumns(const Expr&, std::vector<bool>* needed);

// Builds the executor's program for `expr`, with columns at their lowered
// positions and string literals interned in `pool`. Records the type it
// computes in `type`. Fails lowering if `expr` cannot be typed.
core::exec::ExprProgram LowerExpr(Lowering*,
                                  const Expr&,
                                  StringPool* pool,
                                  core::StorageType* type);

void WriteExpr(PlanWriter*, const Expr&, const Available&);
Expr ReadExpr(PlanReader*, const Available&);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_EXPRESSION_H_
