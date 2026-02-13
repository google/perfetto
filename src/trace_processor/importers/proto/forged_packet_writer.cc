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

#include "src/trace_processor/importers/proto/forged_packet_writer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"

namespace perfetto::trace_processor {

ForgedTracePacketWriter::ForgedTracePacketWriter() : writer_(this) {}

ForgedTracePacketWriter::~ForgedTracePacketWriter() = default;

void ForgedTracePacketWriter::BeginPacket() {
  if (!slab_ || cur_offset_ >= slab_->size()) {
    slab_.reset(new TraceBlob(TraceBlob::Allocate(kSlabSize)));
    cur_offset_ = 0;
  }

  pkt_start_ = cur_offset_;

  auto& blob = *slab_;
  writer_.Reset({blob.data() + cur_offset_, blob.data() + blob.size()});
  msg_.Reset(&writer_);
}

TraceBlobView ForgedTracePacketWriter::EndPacket() {
  msg_.Finalize();

  // Common case: packet fits in the single slab. Zero copies.
  if (PERFETTO_LIKELY(overflow_slabs_.empty())) {
    size_t write_pos = slab_->size() - writer_.bytes_available();
    cur_offset_ = write_pos;
    return {slab_, pkt_start_, write_pos - pkt_start_};
  }

  // Rare: packet spans multiple slabs. Stitch into one contiguous blob.
  return StitchOverflow();
}

TraceBlobView ForgedTracePacketWriter::StitchOverflow() {
  PERFETTO_CHECK(overflow_slabs_.size() > 0);

  // Get the last overflow slab which we'll need to handle separately since
  // we'll promote it to be the new current slab after stitching.
  auto last_slab = std::move(overflow_slabs_.back());
  overflow_slabs_.pop_back();

  size_t last_slab_bytes = last_slab->size() - writer_.bytes_available();
  size_t first_slab_bytes = slab_->size() - pkt_start_;

  // Compute total size: partial first slab + full middle slabs + partial last.
  size_t total = 0;
  total += first_slab_bytes;
  for (size_t i = 0; i + 1 < overflow_slabs_.size(); ++i) {
    total += overflow_slabs_[i]->size();
  }
  total += last_slab_bytes;

  TraceBlob stitched = TraceBlob::Allocate(total);
  uint8_t* dst = stitched.data();

  // Copy over partial first slab.
  memcpy(dst, slab_->data() + pkt_start_, first_slab_bytes);
  dst += first_slab_bytes;

  // Copy over full middle slabs.
  for (const auto& o : overflow_slabs_) {
    memcpy(dst, o->data(), o->size());
    dst += o->size();
  }

  // Copy over partial last slab.
  memcpy(dst, last_slab->data(), last_slab_bytes);

  // Promote the last overflow slab as the new current slab and clear overflow.
  slab_ = std::move(last_slab);
  cur_offset_ = last_slab_bytes;
  overflow_slabs_.clear();

  return TraceBlobView(std::move(stitched), 0, total);
}

protozero::ContiguousMemoryRange ForgedTracePacketWriter::GetNewBuffer() {
  overflow_slabs_.emplace_back(new TraceBlob(TraceBlob::Allocate(kSlabSize)));
  auto& blob = *overflow_slabs_.back();
  return {blob.data(), blob.data() + blob.size()};
}

}  // namespace perfetto::trace_processor
