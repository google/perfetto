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

// Compresses bytes into independent zstd frames and decompresses them into
// memory supplied by the caller.
//
// Owns the zstd contexts and the scratch buffer for compressed output.
//
// See https://facebook.github.io/zstd/zstd_manual.html for the zstd API.
class ZstdFrameCodec {
 public:
  // |input_size_hint| is the usual uncompressed size of a frame.
  // Used to size the zstd window and the initial scratch.
  explicit ZstdFrameCodec(size_t input_size_hint);
  ~ZstdFrameCodec();

  ZstdFrameCodec(const ZstdFrameCodec&) = delete;
  ZstdFrameCodec& operator=(const ZstdFrameCodec&) = delete;
  ZstdFrameCodec(ZstdFrameCodec&&) = delete;
  ZstdFrameCodec& operator=(ZstdFrameCodec&&) = delete;

  // Starts a frame once the previous one has ended with EndFrame().
  // This also invalidates the bytes returned by the previous EndFrame().
  void BeginFrame();

  // Call between BeginFrame() and EndFrame() to add |size| bytes to the frame.
  // The codec copies the bytes so the caller can reuse |data| afterwards.
  void AddToFrame(const void* data, size_t size);

  // Ends the current frame and returns compressed bytes owned by this codec.
  // These bytes remain valid until the next BeginFrame() or this codec's
  // destruction.
  // Call at most once after each BeginFrame().
  protozero::ConstBytes EndFrame();

  // Decompresses |frame| into memory supplied by the caller in |dst|.
  // Returns false if zstd reports an error or the decoded size differs from
  // |dst_size|.
  // On failure, |dst| can be partly written, so the caller must discard its
  // contents.
  bool Decompress(protozero::ConstBytes frame, uint8_t* dst, size_t dst_size);

  // Returns a conservative memory budget for the zstd contexts, window and
  // scratch buffers, including memory that is not allocated yet.
  size_t GetMemoryUsageBytes() const;

 private:
  // Consumes |size| bytes from |data| and grows |frame_buf_| as needed.
  // If |finish| is true, it also finishes the frame.
  void Compress(const void* data, size_t size, bool finish);

  const int window_log_;

  // Capacity for a typical frame, derived from |input_size_hint|.
  // Used by BeginFrame() to shrink the output buffer and free excess memory
  // before compressing the next frame.
  // We keep allocations up to twice this capacity so later frames can reuse
  // them.
  const size_t baseline_frame_buf_capacity_;
  ZSTD_CCtx_s* cctx_ = nullptr;
  ZSTD_DCtx_s* dctx_ = nullptr;

  // Compressed output for the current or most recently finished frame.
  std::unique_ptr<uint8_t[]> frame_buf_;
  // Actual allocation size, which grows to fit compressed output.
  size_t current_frame_buf_capacity_ = 0;
  size_t frame_size_ = 0;

  // TraceBufferV3 passes record headers and packet slices to AddToFrame()
  // separately, so we collect small inputs here before passing them to zstd.
  // This avoids a zstd call for each small input.
  // TODO(sashwinbalaji): Tune this size with bundling profiles.
  // Larger batches reduce codec calls, but require more input copies and
  // scratch memory.
  uint8_t gathered_input_[16 * 1024];
  size_t gathered_input_size_ = 0;

  // True between BeginFrame() and EndFrame(), so DCHECKs can verify call order.
  bool frame_open_ = false;
};

}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#endif  // SRC_TRACING_SERVICE_ZSTD_FRAME_CODEC_H_
