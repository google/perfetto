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

#ifndef SRC_TRACING_SERVICE_TRACE_BUFFER_FACTORY_H_
#define SRC_TRACING_SERVICE_TRACE_BUFFER_FACTORY_H_

#include <stddef.h>

#include <memory>

#include "perfetto/ext/base/status_or.h"
#include "src/tracing/service/trace_buffer.h"

namespace perfetto {

namespace protos {
namespace gen {
class TraceConfig_BufferConfig;
class TraceConfig_BufferConfig_BufferTypeConfig;
}  // namespace gen
}  // namespace protos

// Creates trace buffers and owns the state they share.
class TraceBufferFactory {
 public:
  TraceBufferFactory();
  ~TraceBufferFactory();

  TraceBufferFactory(const TraceBufferFactory&) = delete;
  TraceBufferFactory& operator=(const TraceBufferFactory&) = delete;
  TraceBufferFactory(TraceBufferFactory&&) = delete;
  TraceBufferFactory& operator=(TraceBufferFactory&&) = delete;

  // Creates the buffer that |config| describes.
  // Returns an error if |config| is invalid or the allocation fails.
  base::StatusOr<std::unique_ptr<TraceBuffer>> Create(
      const protos::gen::TraceConfig_BufferConfig& config);

 private:
  // Checks the types in |types| from the highest field number down and creates
  // the first one this build supports, or V1 if none is supported.
  // Returns nullptr if allocation fails.
  std::unique_ptr<TraceBuffer> CreateNewestListedType(
      const protos::gen::TraceConfig_BufferConfig_BufferTypeConfig& types,
      size_t size_in_bytes,
      TraceBuffer::OverwritePolicy policy);
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_FACTORY_H_
