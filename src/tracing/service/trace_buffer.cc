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

#include "src/tracing/service/trace_buffer.h"

#include "perfetto/base/build_config.h"
#include "perfetto/base/status.h"
#include "src/tracing/service/trace_buffer_v1.h"
#include "src/tracing/service/trace_buffer_v2.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
#include "src/tracing/service/trace_buffer_v3.h"
#endif

namespace perfetto {

// static
base::StatusOr<std::unique_ptr<TraceBuffer>> TraceBuffer::Create(
    BufType type,
    size_t size_in_bytes,
    OverwritePolicy policy) {
  std::unique_ptr<TraceBuffer> buffer;
  switch (type) {
    case kV1:
      buffer = TraceBufferV1::Create(size_in_bytes, policy);
      break;
    case kV2:
      buffer = TraceBufferV2::Create(size_in_bytes, policy);
      break;
    case kV3:
#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
      buffer = TraceBufferV3::Create(size_in_bytes, policy);
      break;
#else
      return base::ErrStatus(
          "TraceBufferV3 is not available in this build (zstd disabled)");
#endif
  }
  if (!buffer) {
    return base::ErrStatus("Not enough memory for a trace buffer of %zu bytes",
                           size_in_bytes);
  }
  return std::move(buffer);
}

TraceBuffer::~TraceBuffer() = default;

void TraceBuffer::OnWriteCheckpoint() {}

}  // namespace perfetto
