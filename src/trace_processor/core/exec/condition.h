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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_CONDITION_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_CONDITION_H_

#include <cstdint>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/filter_value.h"

namespace perfetto::trace_processor::core::exec {

// The batch rows kept so far: `count` of them from `rows`, increasing. Each
// condition narrows them in place.
struct ConditionRows {
  uint32_t* rows;
  uint32_t count;
};

// Keeps the rows which are null if `nulls`, or which are not otherwise.
void KeepNulls(const ColumnView& column, bool nulls, ConditionRows& r);

// Keeps the rows `op` is true of, comparing `column` with `cast`, which was
// cast for the column's storage type and `op` (see GetFilterValueCaster).
// `op` is =, !=, <, <=, >, >=, IN or, for a string column, GLOB. `pool` holds
// the column's strings.
void ApplyCondition(const Op& op,
                    const FilterValue& cast,
                    const ColumnView& column,
                    const StringPool& pool,
                    ConditionRows& r);

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_CONDITION_H_
