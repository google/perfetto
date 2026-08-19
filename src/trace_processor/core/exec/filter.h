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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_H_

#include <cstdint>
#include <memory>

#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {

// Keeps rows whose value in `column` compares true against the value at
// `value_index` of the fetcher the query is armed with. The operator narrows
// that value to the column's own type itself, since it is the only thing that
// knows what that type is. String columns support equality and inequality.
//
// `scratch` holds the rows the filter selects and must outlive the operator and
// stay untouched by anything else while a chunk is in flight. Chained filters
// must be given different buffers, since a filter reads the rows its
// predecessor selected while writing its own.
//
// `contiguous_input` says whether the batch still holds the source's rows
// unchanged. That is fixed for a whole execution, so the two cases are separate
// operators rather than a test on every chunk.
std::unique_ptr<Operator> MakeFilter(uint32_t column,
                                     StorageType type,
                                     Op op,
                                     uint32_t value_index,
                                     Span<uint32_t> scratch,
                                     bool contiguous_input);

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_H_
