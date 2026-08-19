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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_COLUMN_REF_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_COLUMN_REF_H_

#include "src/trace_processor/core/exec/transient_column.h"

namespace perfetto::trace_processor::core::dataframe {

// A batch column referencing a dataframe column's storage. Nothing is copied,
// so the dataframe must outlive it.
exec::TransientColumn BorrowColumn(const struct Column& column);

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_COLUMN_REF_H_
