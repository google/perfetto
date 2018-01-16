/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACING_CORE_BASIC_TYPES_H_
#define INCLUDE_PERFETTO_TRACING_CORE_BASIC_TYPES_H_

#include <stddef.h>
#include <stdint.h>

namespace perfetto {

using ProducerID = uint64_t;
using DataSourceID = uint64_t;
using DataSourceInstanceID = uint64_t;
using WriterID = uint16_t;
using BufferID = uint16_t;

// Keep this in sync with SharedMemoryABI::PageHeader::target_buffer.
static constexpr BufferID kMaxTraceBufferID = (1ul << 16) - 1;

// TODO(primiano): temporary. The buffer page size should be configurable by
// consumers.
static constexpr size_t kBufferPageSize = 4096;

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_CORE_BASIC_TYPES_H_
