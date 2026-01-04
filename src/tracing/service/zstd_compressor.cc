/*
 * Copyright (C) 2025 The Android Open Source Project
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

#if !PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
#error "Zstd must be enabled to compile this file."
#endif

#include <lib/zstd.h>

#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {

struct Preamble {
  uint32_t size;
  std::array<uint8_t, 16> buf;
};

template <uint32_t id>
Preamble GetPreamble(size_t sz) {
  Preamble preamble;
  uint8_t* ptr = preamble.buf.data();
  constexpr uint32_t tag = protozero::proto_utils::MakeTagLengthDelimited(id);
  ptr = protozero::proto_utils::WriteVarInt(tag, ptr);
  ptr = protozero::proto_utils::WriteVarInt(sz, ptr);
  preamble.size =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr) -
                            reinterpret_cast<uintptr_t>(preamble.buf.data()));
  PERFETTO_DCHECK(preamble.size < preamble.buf.size());
  return preamble;
}

Slice PreambleToSlice(const Preamble& preamble) {
  Slice slice = Slice::Allocate(preamble.size);
  memcpy(slice.own_data(), preamble.buf.data(), preamble.size);
  return slice;
}

// A compressor for `TracePacket`s that uses zstd. The class is exposed for
// testing.
class ZstdPacketCompressor {
 public:
  ZstdPacketCompressor();
  ~ZstdPacketCompressor();

  // Can be called multiple times, before Finish() is called.
  void PushPacket(const TracePacket& packet);

  // Returned the compressed data. Can be called at most once. After this call,
  // the object is unusable (PushPacket should not be called) and must be
  // destroyed.
  TracePacket Finish();

 private:
  void PushData(const void* data, size_t size);
  void NewOutputSlice();
  void PushCurSlice();

  ZSTD_CStream* stream_;
  size_t total_new_slices_size_ = 0;
  std::vector<Slice> new_slices_;
  std::unique_ptr<uint8_t[]> cur_slice_;
  size_t cur_slice_offset_ = 0;
};

ZstdPacketCompressor::ZstdPacketCompressor() {
  stream_ = ZSTD_createCStream();
  PERFETTO_CHECK(stream_);
  // Use compression level 3 (default for zstd, good balance of speed/ratio)
  size_t ret = ZSTD_initCStream(stream_, 3);
  PERFETTO_CHECK(!ZSTD_isError(ret));
}

ZstdPacketCompressor::~ZstdPacketCompressor() {
  ZSTD_freeCStream(stream_);
}

void ZstdPacketCompressor::PushPacket(const TracePacket& packet) {
  // We need to be able to tokenize packets in the compressed stream, so we
  // prefix a proto preamble to each packet. The compressed stream looks like a
  // valid Trace proto.
  Preamble preamble =
      GetPreamble<protos::pbzero::Trace::kPacketFieldNumber>(packet.size());
  PushData(preamble.buf.data(), preamble.size);
  for (const Slice& slice : packet.slices()) {
    PushData(slice.start, slice.size);
  }
}

void ZstdPacketCompressor::PushData(const void* data, size_t size) {
  ZSTD_inBuffer input = {data, size, 0};

  while (input.pos < input.size) {
    if (!cur_slice_) {
      NewOutputSlice();
    }

    ZSTD_outBuffer output = {cur_slice_.get(), kZstdCompressSliceSize,
                             cur_slice_offset_};

    size_t ret = ZSTD_compressStream(stream_, &output, &input);
    PERFETTO_CHECK(!ZSTD_isError(ret));

    cur_slice_offset_ = output.pos;

    if (cur_slice_offset_ == kZstdCompressSliceSize) {
      PushCurSlice();
    }
  }
}

TracePacket ZstdPacketCompressor::Finish() {
  for (;;) {
    if (!cur_slice_) {
      NewOutputSlice();
    }

    ZSTD_outBuffer output = {cur_slice_.get(), kZstdCompressSliceSize,
                             cur_slice_offset_};

    size_t ret = ZSTD_endStream(stream_, &output);
    PERFETTO_CHECK(!ZSTD_isError(ret));

    cur_slice_offset_ = output.pos;

    if (ret == 0) {
      break;  // Frame fully flushed
    }

    if (cur_slice_offset_ == kZstdCompressSliceSize) {
      PushCurSlice();
    }
  }

  PushCurSlice();

  TracePacket packet;
  packet.AddSlice(PreambleToSlice(
      GetPreamble<protos::pbzero::TracePacket::kCompressedPacketsFieldNumber>(
          total_new_slices_size_)));
  for (auto& slice : new_slices_) {
    packet.AddSlice(std::move(slice));
  }
  return packet;
}

void ZstdPacketCompressor::NewOutputSlice() {
  PushCurSlice();
  cur_slice_ = std::make_unique<uint8_t[]>(kZstdCompressSliceSize);
  cur_slice_offset_ = 0;
}

void ZstdPacketCompressor::PushCurSlice() {
  if (cur_slice_) {
    total_new_slices_size_ += cur_slice_offset_;
    new_slices_.push_back(
        Slice::TakeOwnership(std::move(cur_slice_), cur_slice_offset_));
  }
}

}  // namespace

void ZstdCompressFn(std::vector<TracePacket>* packets) {
  if (packets->empty()) {
    return;
  }

  ZstdPacketCompressor stream;

  for (const TracePacket& packet : *packets) {
    stream.PushPacket(packet);
  }

  TracePacket packet = stream.Finish();

  packets->clear();
  packets->push_back(std::move(packet));
}

}  // namespace perfetto
