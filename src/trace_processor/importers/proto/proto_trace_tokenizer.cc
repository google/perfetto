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

#include "src/trace_processor/importers/proto/proto_trace_tokenizer.h"
#include "perfetto/trace_processor/trace_blob.h"

#include <cstring>
#include <optional>

#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/util/decompressor.h"

namespace perfetto {
namespace trace_processor {

ProtoTraceTokenizer::ProtoTraceTokenizer() = default;

namespace {

constexpr size_t kInitialWindowSize = 256 * 1024;

RefPtr<TraceBlob> NewWindow(size_t capacity) {
  return RefPtr<TraceBlob>(new TraceBlob(TraceBlob::Allocate(capacity)));
}

}  // namespace

base::Status ProtoTraceTokenizer::StartDecompression(
    util::CompressionType type,
    const TraceBlobView& input) {
  if (!decompressor_) {
    decompressor_ = util::CreateDecompressor(type);
    decompressor_type_ = type;
    if (!decompressor_) {
      auto info = util::GetCompressionCodecInfo(type);
      return base::ErrStatus(
          "Cannot decompress compressed_packets: %s is not enabled in this "
          "build (rebuild with %s=true)",
          info.name, info.gn_arg);
    }
  } else if (decompressor_type_ != type) {
    // Malformed (or adversarial) input: a well-formed trace never mixes codecs.
    return base::ErrStatus(
        "A trace must not mix compressed_packets and zstd_compressed_packets "
        "(ERR:tp-corrupt)");
  } else {
    decompressor_->Reset();
  }
  if (!window_) {
    window_capacity_ = kInitialWindowSize;
    window_ = NewWindow(window_capacity_);
  }
  decompressor_->Feed(input.data(), input.length());
  return base::OkStatus();
}

void ProtoTraceTokenizer::CompactWindow(size_t consumed, size_t* fill) {
  size_t rest = *fill - consumed;
  if (PERFETTO_UNLIKELY(!window_->HasOneRef())) {
    // Something kept a view of a packet: leave this window to it.
    RefPtr<TraceBlob> fresh = NewWindow(window_capacity_);
    memcpy(fresh->mutable_data(), window_->data() + consumed, rest);
    window_ = std::move(fresh);
  } else if (consumed > 0 && rest > 0) {
    memmove(window_->mutable_data(), window_->data() + consumed, rest);
  }
  *fill = rest;
}

void ProtoTraceTokenizer::GrowWindow(size_t fill) {
  window_capacity_ *= 2;
  RefPtr<TraceBlob> bigger = NewWindow(window_capacity_);
  memcpy(bigger->mutable_data(), window_->data(), fill);
  window_ = std::move(bigger);
}

}  // namespace trace_processor
}  // namespace perfetto
