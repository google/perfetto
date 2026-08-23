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
#ifndef SRC_TRACE_PROCESSOR_CORE_UTIL_FILE_BACKED_COLUMN_VECTOR_H_
#define SRC_TRACE_PROCESSOR_CORE_UTIL_FILE_BACKED_COLUMN_VECTOR_H_

#include <cstdint>
#include <memory>

#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/io.h"
#include "src/trace_processor/core/util/column_vector.h"

namespace perfetto::trace_processor::core {

base::StatusOr<std::unique_ptr<ColumnVectorBacking>>
CreateFileColumnBacking(io::FileSystem*,
                        uint64_t element_size,
                        uint64_t capacity);

template <typename T>
base::StatusOr<ColumnVector<T>> CreateFileBackedColumnVector(
    io::FileSystem* file_system,
    uint64_t capacity) {
  capacity = base::AlignUp(capacity, ColumnVector<T>::kCapacityMultiple);
  ASSIGN_OR_RETURN(auto backing,
                   CreateFileColumnBacking(file_system, sizeof(T), capacity));
  return ColumnVector<T>::AdoptBacking(0, std::move(backing), true);
}

}  // namespace perfetto::trace_processor::core
#endif  // SRC_TRACE_PROCESSOR_CORE_UTIL_FILE_BACKED_COLUMN_VECTOR_H_
