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

namespace {

// The names below must not collide with other .cc files in the amalgamated
// builds.

// Zstd level 3 balances speed and compression ratio for trace data.
// See https://github.com/google/perfetto/discussions/6397.
constexpr int kZstdFrameCompressionLevel = 3;

// Budget for zstd's compression and decompression state at level 3.
// GetMemoryUsageBytes() adds the history window and our own buffers separately,
// so this constant only covers zstd's remaining allocations.
//
// The estimates below allow for blocks up to 128 KiB (ZSTD_BLOCKSIZE_MAX).
// - 768 KiB for match tables, with 2^17 and 2^16 entries of 4 bytes each.
// - 762 KiB for block buffers, literals, sequences and other compression state.
// - 94 KiB for the decompression context.
// These total about 1.6 MiB, so we round up to 1.75 MiB to leave some margin.
//
// The table sizes use hashLog 17 and chainLog 16 from zstd v1.5.7.
// See ZSTD_defaultCParameters in
// https://github.com/facebook/zstd/blob/v1.5.7/lib/compress/clevels.h.
// See "Memory management" in https://facebook.github.io/zstd/zstd_manual.html.
constexpr size_t kZstdFrameFixedMemoryBytes = 1792 * 1024;

// The window is the recent input zstd can reference to encode repeated bytes.
// ZSTD_c_windowLog is log2 of that size in bytes, so 10 means 1 KiB and 21
// means 2 MiB.
// We use zstd's 1 KiB minimum and cap the window at 2 MiB to bound memory use.
// The cap matches the level 3 default when the input size is unknown.
constexpr int kMinZstdFrameWindowLog = 10;
constexpr int kMaxZstdFrameWindowLog = 21;

// Returns log2 of the window size in bytes, using |input_size_hint| rounded up
// to a power of two.
// We clamp the result to the bounds above to limit context memory.
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
      baseline_frame_buf_capacity_(ZSTD_compressBound(input_size_hint)) {
  cctx_ = ZSTD_createCCtx();
  PERFETTO_CHECK(cctx_);
  size_t rc = ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel,
                                     kZstdFrameCompressionLevel);
  PERFETTO_CHECK(!ZSTD_isError(rc));
  rc = ZSTD_CCtx_setParameter(cctx_, ZSTD_c_windowLog, window_log_);
  PERFETTO_CHECK(!ZSTD_isError(rc));

  // TODO(sashwinbalaji): Allocate dctx_ on first use once we support writing
  // compressed frames directly to a file without decompression.
  dctx_ = ZSTD_createDCtx();
  PERFETTO_CHECK(dctx_);

  frame_buf_.reset(new uint8_t[baseline_frame_buf_capacity_]);
  current_frame_buf_capacity_ = baseline_frame_buf_capacity_;
}

ZstdFrameCodec::~ZstdFrameCodec() {
  // Both accept nullptr.
  ZSTD_freeCCtx(cctx_);
  ZSTD_freeDCtx(dctx_);
}

void ZstdFrameCodec::BeginFrame() {
  PERFETTO_DCHECK(!frame_open_);
  ZSTD_CCtx_reset(cctx_, ZSTD_reset_session_only);

  // Shrink excess capacity back to the baseline to reduce retained memory, as
  // we expect most frames to fit within twice the baseline capacity.
  // We can do this here because the caller has finished using the previous
  // frame's bytes.
  if (PERFETTO_UNLIKELY(current_frame_buf_capacity_ >
                        2 * baseline_frame_buf_capacity_)) {
    frame_buf_.reset(new uint8_t[baseline_frame_buf_capacity_]);
    current_frame_buf_capacity_ = baseline_frame_buf_capacity_;
  }
  frame_size_ = 0;
  gathered_input_size_ = 0;
  frame_open_ = true;
}

void ZstdFrameCodec::AddToFrame(const void* data, size_t size) {
  PERFETTO_DCHECK(frame_open_);
  if (PERFETTO_UNLIKELY(size == 0))
    return;
  PERFETTO_DCHECK(data);

  // If the input exceeds the remaining space, flush the gathered bytes first so
  // zstd sees them before the new input.
  if (gathered_input_size_ > 0 &&
      size > sizeof(gathered_input_) - gathered_input_size_) {
    Compress(gathered_input_, gathered_input_size_, /*finish=*/false);
    gathered_input_size_ = 0;
  }

  if (PERFETTO_LIKELY(size <= sizeof(gathered_input_))) {
    // The input now fits, so append it for the next flush or EndFrame().
    memcpy(gathered_input_ + gathered_input_size_, data, size);
    gathered_input_size_ += size;
  } else {
    // Compress inputs larger than the whole buffer directly to avoid a copy.
    Compress(data, size, /*finish=*/false);
  }
}

protozero::ConstBytes ZstdFrameCodec::EndFrame() {
  PERFETTO_DCHECK(frame_open_);
  frame_open_ = false;

  Compress(gathered_input_, gathered_input_size_, /*finish=*/true);
  gathered_input_size_ = 0;
  return protozero::ConstBytes{frame_buf_.get(), frame_size_};
}

bool ZstdFrameCodec::Decompress(protozero::ConstBytes frame,
                                uint8_t* dst,
                                size_t dst_size) {
  // zstd rejects truncated input and output larger than |dst_size|.
  // Check for shorter output too, so the caller cannot read unwritten bytes.
  const size_t rc =
      ZSTD_decompressDCtx(dctx_, dst, dst_size, frame.data, frame.size);
  return !ZSTD_isError(rc) && rc == dst_size;
}

size_t ZstdFrameCodec::GetMemoryUsageBytes() const {
  const size_t zstd_overhead =
      (size_t(1) << window_log_) + kZstdFrameFixedMemoryBytes;

  // Budget for twice the baseline capacity because we retain up to that size
  // for reuse, but count the current allocation if it is larger.
  const size_t frame_buf_budget =
      std::max(current_frame_buf_capacity_, 2 * baseline_frame_buf_capacity_);

  return zstd_overhead + frame_buf_budget + sizeof(gathered_input_);
}

void ZstdFrameCodec::Compress(const void* data, size_t size, bool finish) {
  ZSTD_inBuffer in{data, size, 0};
  const ZSTD_EndDirective directive = finish ? ZSTD_e_end : ZSTD_e_continue;

  for (;;) {
    ZSTD_outBuffer out{/*dst=*/frame_buf_.get(),
                       /*size=*/current_frame_buf_capacity_,
                       /*pos=*/frame_size_};
    const size_t remaining = ZSTD_compressStream2(cctx_, &out, &in, directive);
    PERFETTO_CHECK(!ZSTD_isError(remaining));
    frame_size_ = out.pos;

    if (finish) {
      // Finishing must also flush buffered output, so keep going until zstd
      // reports that the frame is complete.
      if (remaining == 0)
        return;
    } else if (in.pos == in.size) {
      // zstd can buffer output until EndFrame(), so we can return once it has
      // consumed all input.
      return;
    }

    // Reaching here means |frame_buf_| is full and must grow before we retry.
    PERFETTO_CHECK(out.pos == out.size);

    const size_t new_capacity = 2 * current_frame_buf_capacity_;
    std::unique_ptr<uint8_t[]> new_buf(new uint8_t[new_capacity]);
    memcpy(new_buf.get(), frame_buf_.get(), frame_size_);

    frame_buf_ = std::move(new_buf);
    current_frame_buf_capacity_ = new_capacity;
  }
}

}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
