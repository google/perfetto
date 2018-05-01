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

#include "perfetto/base/build_config.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
using uid_t = unsigned int;
#endif

namespace perfetto {

// Unique within the scope of the tracing service.
using TracingSessionID = uint64_t;

// Unique within the scope of the tracing service.
using ProducerID = uint16_t;

// Unique within the scope of the tracing service.
using DataSourceInstanceID = uint64_t;

// Unique within the scope of a Producer.
using WriterID = uint16_t;

// Unique within the scope of the tracing service.
using FlushRequestID = uint64_t;

// We need one FD per producer and we are not going to be able to keep > 64k FDs
// open in the service.
static constexpr ProducerID kMaxProducerID = static_cast<ProducerID>(-1);

// 1024 Writers per producer seems a resonable bound. This reduces the ability
// to memory-DoS the service by having to keep track of too many writer IDs.
static constexpr WriterID kMaxWriterID = static_cast<WriterID>((1 << 10) - 1);

// Unique within the scope of a {ProducerID, WriterID} tuple.
using ChunkID = uint32_t;
static constexpr ChunkID kMaxChunkID = static_cast<ChunkID>(-1);

// Unique within the scope of the tracing service.
using BufferID = uint16_t;

// Keep this in sync with SharedMemoryABI::PageHeader::target_buffer.
static constexpr BufferID kMaxTraceBufferID = static_cast<BufferID>(-1);

// TODO(primiano): temporary. The buffer page size should be configurable by
// consumers.
static constexpr size_t kBufferPageSize = 8192;

constexpr uid_t kInvalidUid = static_cast<uid_t>(-1);

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_CORE_BASIC_TYPES_H_
