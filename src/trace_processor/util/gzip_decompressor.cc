/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/util/gzip_decompressor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
#include <zconf.h>
#include <zlib.h>
#else
struct z_stream_s {};
#endif

namespace perfetto::trace_processor::util {

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)

namespace {
// zlib's avail_in/avail_out are 32-bit. Hand data to zlib in 1 GiB chunks so
// that buffers >= 4 GiB (e.g. zip64 entries) are not truncated.
constexpr size_t kMaxZlibChunk = 1u << 30;
}  // namespace

GzipDecompressor::GzipDecompressor(InputMode mode)
    : z_stream_(new z_stream_s()) {
  z_stream_->zalloc = nullptr;
  z_stream_->zfree = nullptr;
  z_stream_->opaque = nullptr;
  // zlib uses a window size of -15..-8 to indicate it's a raw inflate stream
  // (i.e. there's no zlib or gzip header).
  int wbits = (mode == InputMode::kRawDeflate) ? -15 : 32 + MAX_WBITS;
  inflateInit2(z_stream_.get(), wbits);
}

void GzipDecompressor::Reset() {
  inflateReset(z_stream_.get());
}

void GzipDecompressor::Feed(const uint8_t* data, size_t size) {
  // This const_cast is not harmfull as zlib will not modify the data in this
  // pointer. This is only necessary because of the build flags we use to be
  // compatible with other embedders.
  z_stream_->next_in = const_cast<uint8_t*>(data);
  size_t chunk = std::min(size, kMaxZlibChunk);
  z_stream_->avail_in = static_cast<uInt>(chunk);
  pending_in_ = size - chunk;
}

GzipDecompressor::Result GzipDecompressor::ExtractOutput(uint8_t* out,
                                                         size_t out_capacity) {
  if (out_capacity == 0)
    return Result{ResultCode::kNeedsMoreInput, 0};

  // zlib advances next_in as it consumes input, so the next chunk of pending
  // input starts right there.
  if (z_stream_->avail_in == 0 && pending_in_ > 0) {
    size_t chunk = std::min(pending_in_, kMaxZlibChunk);
    z_stream_->avail_in = static_cast<uInt>(chunk);
    pending_in_ -= chunk;
  }

  // Note: we intentionally do not early-return when avail_in is 0: inflate()
  // may still hold buffered output (e.g. the tail of a back-reference) that
  // did not fit into the previous output buffer.
  size_t out_chunk = std::min(out_capacity, kMaxZlibChunk);
  z_stream_->next_out = out;
  z_stream_->avail_out = static_cast<uInt>(out_chunk);

  int ret = inflate(z_stream_.get(), Z_NO_FLUSH);
  switch (ret) {
    case Z_STREAM_ERROR:
    case Z_NEED_DICT:
    case Z_DATA_ERROR:
    case Z_MEM_ERROR:
      // Ignore inflateEnd error as we will error out anyway.
      inflateEnd(z_stream_.get());
      return Result{ResultCode::kError, 0};
    case Z_STREAM_END:
      return Result{ResultCode::kEof, out_chunk - z_stream_->avail_out};
    case Z_BUF_ERROR:
      return Result{ResultCode::kNeedsMoreInput, 0};
    default:
      return Result{ResultCode::kOk, out_chunk - z_stream_->avail_out};
  }
}

size_t GzipDecompressor::AvailIn() const {
  return z_stream_->avail_in + pending_in_;
}

void GzipDecompressor::Deleter::operator()(z_stream_s* stream) const {
  inflateEnd(stream);
  delete stream;
}

#else  // !PERFETTO_ZLIB

GzipDecompressor::GzipDecompressor(InputMode) {}
void GzipDecompressor::Reset() {}
void GzipDecompressor::Feed(const uint8_t*, size_t) {}
GzipDecompressor::Result GzipDecompressor::ExtractOutput(uint8_t*, size_t) {
  return Result{ResultCode::kError, 0};
}
size_t GzipDecompressor::AvailIn() const {
  return pending_in_;
}
void GzipDecompressor::Deleter::operator()(z_stream_s*) const {}

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZLIB)

}  // namespace perfetto::trace_processor::util
