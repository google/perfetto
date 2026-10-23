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

#ifndef SRC_TRACING_SERVICE_ZSTD_FRAME_CODEC_H_
#define SRC_TRACING_SERVICE_ZSTD_FRAME_CODEC_H_

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#include <stddef.h>
#include <stdint.h>

#include <memory>

#include "perfetto/protozero/field.h"

struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;

namespace perfetto {
namespace internal {

// Compresses bytes into independent zstd frames and decompresses them into
// caller-owned memory.
// TraceBufferV3 uses one frame per bundle, so each bundle can be decoded or
// discarded without its neighbors.
//
// Owns the zstd contexts and the scratch for compressed output.
// Allocates them on first use and reuses them across frames.
//
// Call from one thread at a time.
// zstd API reference: https://facebook.github.io/zstd/zstd_manual.html
class ZstdFrameCodec {
 public:
  // |input_size_hint| is the usual uncompressed size of a frame.
  // It sizes the zstd window and the initial scratch, but does not limit the
  // frame size.
  explicit ZstdFrameCodec(size_t input_size_hint);
  ~ZstdFrameCodec();

  ZstdFrameCodec(const ZstdFrameCodec&) = delete;
  ZstdFrameCodec& operator=(const ZstdFrameCodec&) = delete;
  ZstdFrameCodec(ZstdFrameCodec&&) = delete;
  ZstdFrameCodec& operator=(ZstdFrameCodec&&) = delete;

  // Starts a frame and discards any unfinished frame.
  // Invalidates the bytes returned by the previous EndFrame().
  void BeginFrame();

  // Adds |size| bytes to the current frame.
  // Consumes or copies them before returning, so the caller can reuse |data|
  // afterwards.
  // |data| can be null when |size| is 0.
  // Call between BeginFrame() and EndFrame().
  //
  // TODO(sashwinbalaji): Measure whether inlining the path for small inputs
  // reduces bundling time.
  // TraceBufferV3 calls this for each record header and each packet slice.
  void AddToFrame(const void* data, size_t size);

  // Ends the current frame.
  // Returns bytes owned by this codec, valid until the next BeginFrame() or
  // destruction.
  // Requires a frame started by BeginFrame() and not yet ended.
  protozero::ConstBytes EndFrame();

  // Decompresses |frame| into caller-owned |dst|.
  // Returns false on a zstd error or if the decoded size differs from
  // |dst_size|.
  // On failure, |dst| can be partly written and the caller must discard its
  // contents.
  //
  // Does not require BeginFrame().
  // Leaves the bytes returned by EndFrame() intact, so the caller can still
  // store them after a Decompress() call.
  bool Decompress(protozero::ConstBytes frame, uint8_t* dst, size_t dst_size);

  // Returns a conservative memory budget for the zstd contexts, the window
  // and the scratch buffers.
  // Counts contexts that are not allocated yet, and oversized scratch until
  // the next BeginFrame() releases it.
  size_t GetMemoryUsageBytes() const;

 private:
  // Consumes |size| bytes from |data|.
  // If |finish| is true, also finishes the frame.
  // Grows |frame_buf_| as needed.
  void Compress(const void* data, size_t size, bool finish);

  const int window_log_;
  const size_t initial_frame_buf_capacity_;
  ZSTD_CCtx_s* cctx_ = nullptr;
  ZSTD_DCtx_s* dctx_ = nullptr;

  // Compressed output for the current or most recently finished frame.
  std::unique_ptr<uint8_t[]> frame_buf_;
  size_t frame_buf_capacity_ = 0;
  size_t frame_size_ = 0;

  // Small pieces of input that zstd has not seen yet.
  // See AddToFrame().
  // TODO(sashwinbalaji): Tune this size with bundling profiles.
  // Larger gathers reduce codec calls but copy more input and retain more
  // scratch memory.
  uint8_t gathered_input_[16 * 1024];
  size_t gathered_input_size_ = 0;

  // True between BeginFrame() and EndFrame().
  // Used by DCHECKs for call order.
  bool frame_open_ = false;
};

}  // namespace internal
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#endif  // SRC_TRACING_SERVICE_ZSTD_FRAME_CODEC_H_
