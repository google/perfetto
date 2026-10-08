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
#include <optional>

#include "perfetto/base/status.h"
#include "src/trace_processor/perfetto_sql/pipeline/plan_types.h"

namespace perfetto::trace_processor::pipeline {

class Compiler;
class PlanReader;

// Entry points which need no operation instance: building a plan from an AST
// and reconstructing it from bytes. Each registration is a static singleton;
// its address also identifies a payload's concrete type without RTTI.
class OperationRegistration {
 public:
  using BuildPlanFn = base::Status (*)(Compiler*, uint32_t);
  using DecodePlanFn = void (*)(PlanReader*, Available*);

  class Encoding {
   public:
    constexpr Encoding(uint8_t tag, bool is_source, DecodePlanFn decode_plan)
        : tag_(tag), is_source_(is_source), decode_plan_(decode_plan) {}

    void DecodePlan(PlanReader* reader, Available* available) const {
      decode_plan_(reader, available);
    }

    uint8_t tag() const { return tag_; }
    bool is_source() const { return is_source_; }

   private:
    // Explicit wire identifier, independent of registration order.
    uint8_t tag_;
    // Starts a serialized pipeline. A source can have children: intersection
    // embeds its operand scans rather than encoding them as preceding stages.
    bool is_source_;
    // Reads the payload after its tag, appends nodes to the reader's plan and
    // updates available columns. The reader latches malformed-input failures.
    DecodePlanFn decode_plan_;
  };

  constexpr OperationRegistration(uint32_t syntax,
                                  BuildPlanFn build_plan,
                                  std::optional<Encoding> encoding)
      : syntax_(syntax), build_plan_(build_plan), encoding_(encoding) {}

  base::Status BuildPlan(Compiler* compiler, uint32_t node) const {
    return build_plan_(compiler, node);
  }

  uint32_t syntax() const { return syntax_; }
  const std::optional<Encoding>& encoding() const { return encoding_; }

 private:
  // Syntaqlite node tag identifying the source or stage syntax.
  uint32_t syntax_;

  // Resolves an AST node, updating compiler scope and the logical plan.
  // The integer is an AST node ID, not a logical plan node ID.
  BuildPlanFn build_plan_;

  // Projections only change compiler scope and have no encoded plan payload.
  std::optional<Encoding> encoding_;
};

const OperationRegistration* FindOperationByTag(uint8_t tag);
const OperationRegistration* FindOperationBySyntax(uint32_t syntax);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATION_REGISTRY_H_
