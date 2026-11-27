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

#include "src/tracing/service/zstd_compressor.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)

#include <string.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <zstd.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/tracing/core/slice.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/tracing/service/packet_compressor_common.h"

namespace perfetto {

namespace {

using packet_compressor::GetPreamble;
using packet_compressor::kCompressSliceSize;
using packet_compressor::Preamble;
using packet_compressor::PreambleToSlice;

// Each ZSTD_compressStream2() call has a fixed cost, so we gather inputs up to
// this size and pass them to zstd together. 4 KiB is a starting point that we
// can tune with ReadBuffers() profiles if needed.
constexpr size_t kZstdGatheredInputSize = 4 * 1024;

// Writes the zstd frame for one Compress() call on ZstdPacketCompressor's
// context, so no frame state carries over to the next batch.
class ZstdPacketFrameWriter {
 public:
  // `src_size` must be the exact number of bytes PushPacket() will feed in,
  // preambles included, or zstd fails the frame.
  ZstdPacketFrameWriter(ZSTD_CCtx* cctx, size_t src_size);

  ZstdPacketFrameWriter(const ZstdPacketFrameWriter&) = delete;
  ZstdPacketFrameWriter& operator=(const ZstdPacketFrameWriter&) = delete;
  ZstdPacketFrameWriter(ZstdPacketFrameWriter&&) = delete;
  ZstdPacketFrameWriter& operator=(ZstdPacketFrameWriter&&) = delete;

  // Can be called multiple times, before Finish() is called.
  void PushPacket(const TracePacket& packet);

  // Ends the frame and returns it as one packet. Can be called at most once.
  TracePacket Finish();

 private:
  void PushData(const void* data, size_t size);
  void CompressStream(const void* data,
                      size_t size,
                      ZSTD_EndDirective directive);
  void NewOutputSlice();
  void PushCurSlice();

  ZSTD_CCtx* const cctx_;
  // Bytes to allocate per output slice, capped at the worst-case frame size
  // for this batch (ZSTD_compressBound()).
  const size_t slice_capacity_;

  std::array<uint8_t, kZstdGatheredInputSize> gathered_input_;
  size_t gathered_input_size_ = 0;

  // Points into `cur_slice_`. Zero-initialized so that the first compression
  // call observes a full output buffer and allocates the initial slice.
  ZSTD_outBuffer out_{/*dst=*/nullptr, /*size=*/0, /*pos=*/0};
  size_t total_new_slices_size_ = 0;
  std::vector<Slice> new_slices_;
  std::unique_ptr<uint8_t[]> cur_slice_;
};

ZstdPacketFrameWriter::ZstdPacketFrameWriter(ZSTD_CCtx* cctx, size_t src_size)
    : cctx_(cctx),
      slice_capacity_(
          std::min(kCompressSliceSize, ZSTD_compressBound(src_size))) {
  // Pledge the batch size so zstd sizes its tables and window for it. Without
  // a size, zstd assumes a large input even for a small batch.
  size_t rc = ZSTD_CCtx_setPledgedSrcSize(cctx_, src_size);
  PERFETTO_CHECK(!ZSTD_isError(rc));
}

void ZstdPacketFrameWriter::PushPacket(const TracePacket& packet) {
  // Prefix each packet with its proto preamble so the compressed stream itself
  // parses as a valid Trace proto, and its packets can be tokenized back out.
  Preamble preamble =
      GetPreamble<protos::pbzero::Trace::kPacketFieldNumber>(packet.size());
  PushData(preamble.buf.data(), preamble.size);
  for (const Slice& slice : packet.slices()) {
    PushData(slice.start, slice.size);
  }
}

TracePacket ZstdPacketFrameWriter::Finish() {
  // ZSTD_e_end compresses the rest of `gathered_input_`, flushes buffered data
  // and writes the frame epilogue.
  CompressStream(gathered_input_.data(), gathered_input_size_, ZSTD_e_end);
  PushCurSlice();

  TracePacket packet;
  packet.AddSlice(PreambleToSlice(
      GetPreamble<
          protos::pbzero::TracePacket::kZstdCompressedPacketsFieldNumber>(
          total_new_slices_size_)));
  for (auto& slice : new_slices_) {
    packet.AddSlice(std::move(slice));
  }
  return packet;
}

void ZstdPacketFrameWriter::PushData(const void* data, size_t size) {
  if (PERFETTO_UNLIKELY(size == 0)) {
    return;
  }
  PERFETTO_DCHECK(data);

  // If `data` doesn't fit after the gathered bytes, flush them first so zstd
  // sees the bytes in order.
  if (gathered_input_size_ > 0 &&
      size > gathered_input_.size() - gathered_input_size_) {
    CompressStream(gathered_input_.data(), gathered_input_size_,
                   ZSTD_e_continue);
    gathered_input_size_ = 0;
  }

  if (size <= gathered_input_.size()) {
    memcpy(gathered_input_.data() + gathered_input_size_, data, size);
    gathered_input_size_ += size;
  } else {
    // Compress inputs larger than the gather buffer directly to avoid a copy.
    CompressStream(data, size, ZSTD_e_continue);
  }
}

void ZstdPacketFrameWriter::CompressStream(const void* data,
                                           size_t size,
                                           ZSTD_EndDirective directive) {
  // zstd may not take all input or finish the frame in one call, for example
  // when the output slice fills. So keep calling it, with a fresh slice
  // whenever the current one is full.
  ZSTD_inBuffer in = {/*src=*/data, /*size=*/size, /*pos=*/0};
  for (;;) {
    if (out_.pos == out_.size) {
      NewOutputSlice();
    }
    size_t remaining = ZSTD_compressStream2(cctx_, &out_, &in, directive);
    PERFETTO_CHECK(!ZSTD_isError(remaining));
    if (directive == ZSTD_e_end ? remaining == 0 : in.pos == in.size) {
      return;
    }
  }
}

void ZstdPacketFrameWriter::NewOutputSlice() {
  PushCurSlice();
  // zstd overwrites the slice, so skip the zero-fill of make_unique().
  cur_slice_.reset(new uint8_t[slice_capacity_]);
  out_ = {/*dst=*/cur_slice_.get(), /*size=*/slice_capacity_, /*pos=*/0};
}

void ZstdPacketFrameWriter::PushCurSlice() {
  if (!cur_slice_) {
    return;
  }
  total_new_slices_size_ += out_.pos;
  new_slices_.push_back(Slice::TakeOwnership(std::move(cur_slice_), out_.pos));
}

}  // namespace

ZstdPacketCompressor::ZstdPacketCompressor(int level)
    : cctx_(ZSTD_createCCtx()) {
  PERFETTO_CHECK(cctx_);
  // zstd maps 0 to its default level, clamps levels above its max, and treats
  // negatives as fast modes, so any int is safe to pass through here.
  size_t rc =
      ZSTD_CCtx_setParameter(cctx_.get(), ZSTD_c_compressionLevel, level);
  PERFETTO_CHECK(!ZSTD_isError(rc));
}

void ZstdPacketCompressor::Compress(std::vector<TracePacket>* packets) {
  if (packets->empty()) {
    return;
  }

  // zstd fails the frame if the bytes we feed in differ from the pledged size.
  // So count exactly what PushPacket() feeds in, preambles included.
  size_t src_size = 0;
  for (const TracePacket& packet : *packets) {
    Preamble preamble =
        GetPreamble<protos::pbzero::Trace::kPacketFieldNumber>(packet.size());
    src_size += preamble.size + packet.size();
  }

  ZstdPacketFrameWriter writer(cctx_.get(), src_size);
  for (const TracePacket& packet : *packets) {
    writer.PushPacket(packet);
  }
  TracePacket packet = writer.Finish();

  packets->clear();
  packets->push_back(std::move(packet));
}

// static
int ZstdPacketCompressor::FreeCCtx(ZSTD_CCtx* cctx) {
  // ScopedResource's close function must return int but ZSTD_freeCCtx()
  // returns size_t, so wrap it. ZSTD_freeCCtx() only fails for a
  // static-allocated context. Ours comes from ZSTD_createCCtx() on the heap,
  // so returning 0 is safe.
  ZSTD_freeCCtx(cctx);
  return 0;
}

}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
