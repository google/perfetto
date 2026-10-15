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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_TYPES_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>

#include "src/trace_processor/core/common/schema.h"

namespace perfetto::trace_processor::pipeline {

using core::ColumnSchema;
using core::Schema;

// Stable within a plan. Lowering assigns physical batch positions separately.
using ColumnId = uint32_t;

// A name in a scope or result. Multiple names can refer to the same value.
struct NamedColumn {
  std::string name;
  ColumnId id;
};

// Stable within a plan.
using PlanNodeId = uint32_t;

// Which way a tree is walked: from the leaves up, or from the roots down.
enum class TreeDirection : uint8_t { kUp, kDown };

// A sort key: the first key in a list decides, each later one breaks ties.
struct SortKey {
  ColumnId column = 0;
  bool descending = false;
};

// The columns available to a stage, in the order positions number them.
using Available = std::vector<ColumnId>;

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_TYPES_H_
