/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_ARROW_IPC_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_ARROW_IPC_H_

#include <cstddef>
#include <functional>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor::core::dataframe {

using ArrowIpcWriteSink = std::function<void(const uint8_t*, size_t)>;

// Streaming write: serializes the Dataframe as an Arrow IPC file,
// calling sink with chunks as they are produced.
base::Status SerializeToArrowIpc(const Dataframe& df,
                                 StringPool* pool,
                                 ArrowIpcWriteSink sink);

// Deserialize an Arrow IPC file into an existing (empty) Dataframe.
// Schema must match exactly (same column names, types, nullability,
// excluding _auto_id).
base::Status DeserializeFromArrowIpc(
    Dataframe& df,
    StringPool* pool,
    const util::TraceBlobViewReader& reader);

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_ARROW_IPC_H_
