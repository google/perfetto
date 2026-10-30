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

#include "src/tracing/service/trace_buffer_factory.h"

#include <cinttypes>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/base/status.h"
#include "perfetto/tracing/core/trace_config.h"
#include "src/tracing/service/trace_buffer_v1.h"
#include "src/tracing/service/trace_buffer_v2.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
#include "src/tracing/service/trace_buffer_v3.h"
#include "src/tracing/service/zstd_frame_codec.h"
#endif

namespace perfetto {

TraceBufferFactory::TraceBufferFactory() = default;
TraceBufferFactory::~TraceBufferFactory() = default;

base::StatusOr<std::unique_ptr<TraceBuffer>> TraceBufferFactory::Create(
    const TraceConfig::BufferConfig& config) {
  // TraceBuffer size is limited to 32-bit.
  const uint32_t buf_size_kb = config.size_kb();
  const uint64_t buf_size_bytes = buf_size_kb * static_cast<uint64_t>(1024);
  const size_t buf_size = static_cast<size_t>(buf_size_bytes);
  if (buf_size_bytes == 0 ||
      buf_size_bytes > std::numeric_limits<uint32_t>::max() ||
      buf_size != buf_size_bytes) {
    return base::ErrStatus(
        "size_kb must be nonzero and less than 4 GiB, got %" PRIu32,
        buf_size_kb);
  }

  const TraceBuffer::OverwritePolicy policy =
      config.fill_policy() == TraceConfig::BufferConfig::DISCARD
          ? TraceBuffer::kDiscard
          : TraceBuffer::kOverwrite;

  std::unique_ptr<TraceBuffer> buffer;
  const auto mode = config.experimental_mode();
  if (config.has_buffer_type()) {
    buffer = CreateNewestListedType(config.buffer_type(), buf_size, policy);
  } else if (mode == TraceConfig::BufferConfig::MODE_UNSPECIFIED) {
    buffer = TraceBufferV1::Create(buf_size, policy);
  } else if (mode == TraceConfig::BufferConfig::TRACE_BUFFER_V2) {
    buffer = TraceBufferV2::Create(buf_size, policy);
  } else {
    // New types only go in |buffer_type|, so any other value here is an
    // invalid config rather than a type we don't know yet.
    // This includes the removed value 2 for TRACE_BUFFER_V2_SHADOW_MODE.
    return base::ErrStatus("Unknown experimental_mode %d",
                           static_cast<int>(mode));
  }

  if (!buffer) {
    return base::ErrStatus("Not enough memory for a trace buffer of %zu bytes",
                           buf_size);
  }
  return std::move(buffer);
}

std::unique_ptr<TraceBuffer> TraceBufferFactory::CreateNewestListedType(
    const TraceConfig::BufferConfig::BufferTypeConfig& types,
    size_t size_in_bytes,
    TraceBuffer::OverwritePolicy policy) {
#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
  // TraceBufferV3 compresses bundles with zstd, so only zstd builds have it.
  if (types.has_v3()) {
    std::shared_ptr<ZstdFrameCodec> codec = v3_codec_.lock();
    if (!codec) {
      // Use new rather than make_shared() to allocate the codec separately.
      // Its storage is freed after destruction while |v3_codec_| keeps only the
      // shared_ptr bookkeeping allocation.
      // https://en.cppreference.com/cpp/memory/shared_ptr/make_shared#Notes
      codec.reset(new ZstdFrameCodec(TraceBufferV3::kMaxBundleTargetSize));
      v3_codec_ = codec;
    }
    return TraceBufferV3::Create(size_in_bytes, policy, std::move(codec));
  }
#endif
  if (types.has_v2())
    return TraceBufferV2::Create(size_in_bytes, policy);
  return TraceBufferV1::Create(size_in_bytes, policy);
}

size_t TraceBufferFactory::GetMemoryUsageBytes() const {
#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
  if (std::shared_ptr<ZstdFrameCodec> codec = v3_codec_.lock())
    return codec->GetMemoryUsageBytes();
#endif
  return 0;
}

}  // namespace perfetto
