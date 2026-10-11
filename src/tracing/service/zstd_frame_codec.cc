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

#include "src/tracing/service/zstd_frame_codec.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#include <string.h>

#include <algorithm>
#include <memory>
#include <utility>

#include <zstd.h>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace internal {

namespace {

// The names below must not collide with other .cc files in the amalgamated
// builds.

// RFC 0050 starts with level 3. TraceBufferV3 compresses on the service
// thread, so measure bundling time before raising the level. The context
// memory budget below also depends on this choice.
constexpr int kZstdFrameCompressionLevel = 3;

// Bounds for ZSTD_c_windowLog. The lower bound is ZSTD_WINDOWLOG_MIN. The
// upper bound limits context memory, which grows with the window.
constexpr int kMinZstdFrameWindowLog = 10;
constexpr int kMaxZstdFrameWindowLog = 21;

// Budget for the two zstd contexts, excluding the window. The sizes below are
// what ZSTD_sizeof_CCtx() and ZSTD_sizeof_DCtx() report after one level-3
// frame. They total about 1.6 MiB, so we round up to 1.75 MiB. See "Memory
// management" in https://facebook.github.io/zstd/zstd_manual.html.
//
// - Compression tables, 768 KiB. Level 3 has hashLog 17 and chainLog 16, and
//   an entry takes 4 bytes. See ZSTD_defaultCParameters in
//   https://github.com/facebook/zstd/blob/v1.5.7/lib/compress/clevels.h.
// - Compression buffers for one block of ZSTD_BLOCKSIZE_MAX, 762 KiB.
// - Decompression context, 94 KiB.
constexpr size_t kZstdFrameFixedMemoryBytes = 1792 * 1024;

// Returns log2 of the window size in bytes. Rounds |input_size_hint| up to
// a power of two and clamps the logarithm to the bounds above.
int ComputeZstdFrameWindowLog(size_t input_size_hint) {
  int window_log = kMinZstdFrameWindowLog;
  while (window_log < kMaxZstdFrameWindowLog &&
         (size_t(1) << window_log) < input_size_hint) {
    ++window_log;
  }
  return window_log;
}

}  // namespace

ZstdFrameCodec::ZstdFrameCodec(size_t input_size_hint)
    : window_log_(ComputeZstdFrameWindowLog(input_size_hint)),
      initial_frame_buf_capacity_(ZSTD_compressBound(input_size_hint)) {}

ZstdFrameCodec::~ZstdFrameCodec() = default;

void ZstdFrameCodec::BeginFrame() {
  if (!cctx_) {
    cctx_.reset(ZSTD_createCCtx());
    PERFETTO_CHECK(cctx_);
    size_t rc = ZSTD_CCtx_setParameter(cctx_.get(), ZSTD_c_compressionLevel,
                                       kZstdFrameCompressionLevel);
    PERFETTO_CHECK(!ZSTD_isError(rc));
    rc = ZSTD_CCtx_setParameter(cctx_.get(), ZSTD_c_windowLog, window_log_);
    PERFETTO_CHECK(!ZSTD_isError(rc));
  } else {
    ZSTD_CCtx_reset(cctx_.get(), ZSTD_reset_session_only);
  }

  // Release scratch grown by an unusually large frame before starting the
  // next one. Keep up to twice the initial capacity for reuse.
  if (!frame_buf_ || frame_buf_capacity_ > 2 * initial_frame_buf_capacity_) {
    frame_buf_.reset(new uint8_t[initial_frame_buf_capacity_]);
    frame_buf_capacity_ = initial_frame_buf_capacity_;
  }
  frame_size_ = 0;
  gathered_input_size_ = 0;
  frame_open_ = true;
}

void ZstdFrameCodec::AddToFrame(const void* data, size_t size) {
  PERFETTO_DCHECK(frame_open_);

  // Gather small inputs to avoid a zstd call for each one. TraceBufferV3
  // adds record headers and packet slices separately. Flush gathered bytes
  // before passing a large input directly to zstd, so their order is kept.
  if (size > sizeof(gathered_input_) - gathered_input_size_)
    FlushGatheredInput();
  if (size <= sizeof(gathered_input_)) {
    memcpy(gathered_input_ + gathered_input_size_, data, size);
    gathered_input_size_ += size;
    return;
  }
  ZSTD_inBuffer in{data, size, 0};
  Compress(&in, ZSTD_e_continue);
}

protozero::ConstBytes ZstdFrameCodec::EndFrame() {
  PERFETTO_DCHECK(frame_open_);
  frame_open_ = false;

  FlushGatheredInput();
  ZSTD_inBuffer in{nullptr, 0, 0};
  Compress(&in, ZSTD_e_end);
  return protozero::ConstBytes{frame_buf_.get(), frame_size_};
}

bool ZstdFrameCodec::Decompress(protozero::ConstBytes frame,
                                uint8_t* dst,
                                size_t dst_size) {
  if (!dctx_) {
    dctx_.reset(ZSTD_createDCtx());
    PERFETTO_CHECK(dctx_);
  }
  // zstd rejects truncated input and output larger than |dst_size|. Check
  // for shorter output too, so the caller cannot read unwritten bytes.
  const size_t rc =
      ZSTD_decompressDCtx(dctx_.get(), dst, dst_size, frame.data, frame.size);
  return !ZSTD_isError(rc) && rc == dst_size;
}

size_t ZstdFrameCodec::GetMemoryUsageBytes() const {
  // Count oversized scratch until BeginFrame() releases it. EndFrame() must
  // leave it allocated because the caller borrows the compressed bytes.
  return (size_t(1) << window_log_) +
         std::max(frame_buf_capacity_, 2 * initial_frame_buf_capacity_) +
         kZstdFrameFixedMemoryBytes;
}

void ZstdFrameCodec::Compress(ZSTD_inBuffer* in, ZSTD_EndDirective directive) {
  for (;;) {
    ZSTD_outBuffer out{frame_buf_.get(), frame_buf_capacity_, frame_size_};
    const size_t remaining =
        ZSTD_compressStream2(cctx_.get(), &out, in, directive);
    PERFETTO_CHECK(!ZSTD_isError(remaining));
    frame_size_ = out.pos;

    // ZSTD_e_continue is done when all input is consumed. ZSTD_e_end must
    // also flush the remaining compressed bytes and finish the frame.
    const bool done =
        directive == ZSTD_e_end ? remaining == 0 : in->pos == in->size;
    if (done)
      return;

    // zstd made all the progress it could and needs more output space.
    PERFETTO_CHECK(frame_size_ == frame_buf_capacity_);
    const size_t new_capacity = 2 * frame_buf_capacity_;
    std::unique_ptr<uint8_t[]> new_buf(new uint8_t[new_capacity]);
    memcpy(new_buf.get(), frame_buf_.get(), frame_size_);
    frame_buf_ = std::move(new_buf);
    frame_buf_capacity_ = new_capacity;
  }
}

void ZstdFrameCodec::FlushGatheredInput() {
  if (gathered_input_size_ == 0)
    return;
  ZSTD_inBuffer in{gathered_input_, gathered_input_size_, 0};
  Compress(&in, ZSTD_e_continue);
  gathered_input_size_ = 0;
}

}  // namespace internal
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
