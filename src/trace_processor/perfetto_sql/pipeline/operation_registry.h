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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATION_REGISTRY_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATION_REGISTRY_H_

#include <cstdint>

#include "perfetto/base/status.h"

namespace perfetto::trace_processor::pipeline {

class Compiler;

// Each operation owns its grammar entry point; the registry lists them once.
// These callbacks need no operation instance: projections only change compiler
// scope, and other operations construct their plan payload from the AST.
class OperationRegistration {
 public:
  using BuildPlanFn = base::Status (*)(Compiler*, uint32_t);

  constexpr OperationRegistration(uint32_t syntax, BuildPlanFn build_plan)
      : syntax_(syntax), build_plan_(build_plan) {}

  base::Status BuildPlan(Compiler* compiler, uint32_t node) const {
    return build_plan_(compiler, node);
  }

  uint32_t syntax() const { return syntax_; }

 private:
  // Syntaqlite node tag identifying the source or stage syntax.
  uint32_t syntax_;

  // Resolves an AST node, updating the compiler scope and logical plan.
  // The integer is an AST node ID, not a logical plan node ID.
  BuildPlanFn build_plan_;
};

const OperationRegistration* FindOperationBySyntax(uint32_t syntax);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATION_REGISTRY_H_
