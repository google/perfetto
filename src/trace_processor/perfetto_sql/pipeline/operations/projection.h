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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_PROJECTION_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_PROJECTION_H_

#include "src/trace_processor/perfetto_sql/pipeline/compiler.h"
#include "src/trace_processor/perfetto_sql/pipeline/operation_registry.h"

namespace perfetto::trace_processor::pipeline {

class Projection {
 public:
  static const OperationRegistration kSelect;
  static const OperationRegistration kExtend;
  static const OperationRegistration kDrop;
  static const OperationRegistration kRename;
  static const OperationRegistration kSet;
  static const OperationRegistration kAs;

 private:
  static base::Status BuildSelectPlan(Compiler*, uint32_t stage);
  static base::Status BuildExtendPlan(Compiler*, uint32_t stage);
  static base::Status BuildDropPlan(Compiler*, uint32_t stage);
  static base::Status BuildRenamePlan(Compiler*, uint32_t stage);
  static base::Status BuildSetPlan(Compiler*, uint32_t stage);
  static base::Status BuildAsPlan(Compiler*, uint32_t stage);

  // The columns a SELECT or EXTEND list produces, resolved against the row
  // before the stage.
  static base::StatusOr<std::vector<Compiler::RowColumn>>
  ResolveItems(Compiler*, uint32_t list_id, bool allow_unqualified_star);

  static base::StatusOr<std::vector<Compiler::RowColumn>>
  ExpandStar(Compiler*, uint32_t star_id, bool allow_unqualified_star);
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATIONS_PROJECTION_H_
