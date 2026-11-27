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

namespace perfetto::trace_processor::core {
class Arena;
}  // namespace perfetto::trace_processor::core

namespace perfetto::trace_processor::pipeline {

class PlanOperation;
struct BoundPlan;
struct LoweringContext;

// Static descriptor for a concrete PlanOperation: identifies its type without
// RTTI (by singleton address) and reconstructs it from its wire tag.
class OperationRegistration {
 public:
  using MakeFn = PlanOperation* (*)(core::Arena*);

  constexpr OperationRegistration(uint8_t tag, MakeFn make)
      : tag_(tag), make_(make) {}

  // An empty operation of this kind in `arena`, for a reader to fill in.
  PlanOperation* Make(core::Arena* arena) const;
  uint8_t tag() const { return tag_; }

 private:
  uint8_t tag_;
  MakeFn make_;
};

const OperationRegistration* FindOperationByTag(uint8_t tag);

// Runs rewrite and input-planning passes over `bound`, populating
// `bound->planned` before lowering.
base::Status RunBoundPasses(BoundPlan* bound, const LoweringContext& context);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_OPERATION_REGISTRY_H_
