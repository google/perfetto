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
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/expression.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

class Compiler;
class Lowering;
class PlanCodec;

// A scalar expression over the columns of a row, as a stage computes it:
// column references, number literals, NULL and arithmetic.
//
// It has the shape of the executor's program: nodes in post-order, each
// reading only nodes before it, the last being the result. A kColumn node's
// `a` is a ColumnId rather than a batch position, and only a kConstant node's
// `type` means anything: the rest are typed when the plan is lowered.
//
// Only expressions whose meaning is the same as SQLite's are accepted.
// Anything else is refused when the plan is built, not answered
// differently: arithmetic on strings is an error, and an integer overflow
// fails the query where SQLite would answer with a double.
struct Expr {
  std::vector<core::exec::ExprStep> nodes;
};

// Compiles the expression at AST node `expr` against the row in scope.
base::Status CompileExpr(Compiler*, uint32_t expr, Expr* out);

// The type of `expr`, checked as far as the column types known when the plan
// is built allow. Unknown if it reads a column of unknown type. A failure
// names what was expected, for Compiler::Expected.
base::StatusOr<PlanType> CheckExprType(const Compiler&, const Expr&);

// Marks the columns `expr` reads.
void MarkExprColumns(const Expr&, std::vector<bool>* needed);

// Builds the executor's program for `expr`, with columns at their lowered
// positions, and sets `type` to the type it computes. Fails lowering if
// `expr` cannot be typed.
core::exec::ExprProgram LowerExpr(Lowering*,
                                  const Expr&,
                                  core::StorageType* type);

// Writes or reads `expr`.
void SerializeExpr(PlanCodec&, Expr* expr);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_EXPRESSION_H_
