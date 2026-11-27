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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPESQL_EXPRESSIONS_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPESQL_EXPRESSIONS_H_

#include <cstdint>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"
#include "src/trace_processor/perfetto_sql/pipeline/expression.h"
#include "src/trace_processor/perfetto_sql/pipeline/literal.h"

namespace perfetto::trace_processor::pipesql {

class Translator;

// Reads an integer literal (optionally negated) at AST node `expr`.
base::StatusOr<int64_t> TranslateInt64Literal(Translator*, uint32_t expr);

// Reads a number (optionally negated) or string literal at AST node `expr`.
base::StatusOr<pipeline::Literal> TranslateLiteral(Translator*, uint32_t expr);

// Returns true for IS NULL, false for IS NOT NULL; rejects other IS forms.
base::StatusOr<bool> TranslateIsNull(Translator*,
                                     uint32_t expr,
                                     const SyntaqliteIsExpr& node);

// Translates scalar expression AST node `expr` into `out` in the builder's
// arena.
base::Status TranslateExpr(Translator*, uint32_t expr, pipeline::Expr* out);

}  // namespace perfetto::trace_processor::pipesql

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPESQL_EXPRESSIONS_H_
