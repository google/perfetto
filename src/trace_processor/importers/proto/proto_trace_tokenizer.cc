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

#include "perfetto/ext/base/utils.h"

namespace perfetto {
namespace trace_processor {

util::Status ProtoTraceTokenizer::Decompress(TraceBlobView input,
                                             TraceBlobView* output) {
  PERFETTO_DCHECK(gzip::IsGzipSupported());

  uint8_t out[4096];

  std::vector<uint8_t> data;
  data.reserve(input.length());

  // Ensure that the decompressor is able to cope with a new stream of data.
  decompressor_.Reset();
  decompressor_.SetInput(input.data(), input.length());

  using ResultCode = GzipDecompressor::ResultCode;
  for (auto ret = ResultCode::kOk; ret != ResultCode::kEof;) {
    auto res = decompressor_.Decompress(out, base::ArraySize(out));
    ret = res.ret;
    if (ret == ResultCode::kError || ret == ResultCode::kNoProgress ||
        ret == ResultCode::kNeedsMoreInput) {
      return util::ErrStatus("Failed to decompress (error code: %d)",
                             static_cast<int>(ret));
    }

    data.insert(data.end(), out, out + res.bytes_written);
  }

  std::unique_ptr<uint8_t[]> out_data(new uint8_t[data.size()]);
  memcpy(out_data.get(), data.data(), data.size());
  *output = TraceBlobView(std::move(out_data), 0, data.size());
  return util::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
