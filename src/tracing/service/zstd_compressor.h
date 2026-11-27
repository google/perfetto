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

#ifndef SRC_TRACING_SERVICE_ZSTD_COMPRESSOR_H_
#define SRC_TRACING_SERVICE_ZSTD_COMPRESSOR_H_

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#include <vector>

#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/tracing/core/trace_packet.h"

struct ZSTD_CCtx_s;

namespace perfetto {

// Compresses `TracePacket`s with zstd's streaming API. Each Compress() call
// writes one zstd frame into slices of up to
// packet_compressor::kCompressSliceSize bytes.
// The zstd context is reused across calls, so later batches reuse its tables
// and window instead of allocating their own, unless they need bigger ones.
// zstd API reference: https://facebook.github.io/zstd/zstd_manual.html
class ZstdPacketCompressor {
 public:
  explicit ZstdPacketCompressor(int level);

  ZstdPacketCompressor(const ZstdPacketCompressor&) = delete;
  ZstdPacketCompressor& operator=(const ZstdPacketCompressor&) = delete;
  ZstdPacketCompressor(ZstdPacketCompressor&&) = delete;
  ZstdPacketCompressor& operator=(ZstdPacketCompressor&&) = delete;

  // Replaces `packets` with a single packet holding their zstd frame.
  // No-op if `packets` is empty.
  void Compress(std::vector<TracePacket>* packets);

 private:
  static int FreeCCtx(ZSTD_CCtx_s*);
  using ScopedCCtx = base::ScopedResource<ZSTD_CCtx_s*,
                                          FreeCCtx,
                                          /*InvalidValue=*/nullptr,
                                          /*CheckClose=*/false>;

  ScopedCCtx cctx_;
};

}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#endif  // SRC_TRACING_SERVICE_ZSTD_COMPRESSOR_H_
