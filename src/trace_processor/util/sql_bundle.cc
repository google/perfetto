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

#include "src/trace_processor/util/sql_bundle.h"

#include <memory>
#include <utility>

#include "src/trace_processor/util/decompressor.h"

namespace perfetto::trace_processor {

SqlBundle SqlBundle::Decode(const uint8_t* data, size_t size) {
  // Keep codec IDs in sync with tools/gen_amalgamated_sql.py.
  constexpr size_t kHeaderSize = 1 + sizeof(uint32_t);
  PERFETTO_CHECK(size >= kHeaderSize);
  uint8_t codec = *data;
  uint32_t decoded_size;
  std::memcpy(&decoded_size, data + 1, sizeof(decoded_size));
  decoded_size = base::LE32ToHost(decoded_size);
  PERFETTO_CHECK(decoded_size >= sizeof(uint32_t));
  data += kHeaderSize;
  size -= kHeaderSize;
  if (codec == 0) {
    PERFETTO_CHECK(size == decoded_size);
    return SqlBundle(data, size);
  }

  util::CompressionType type;
  switch (codec) {
    case 1:
      // The zlib backend accepts both zlib and gzip framing.
      type = util::CompressionType::kGzip;
      break;
    case 2:
      type = util::CompressionType::kZstd;
      break;
    default:
      PERFETTO_FATAL("Unknown SQL bundle codec: %u", codec);
  }
  auto decoder = util::CreateDecompressor(type);
  PERFETTO_CHECK(decoder);
  auto buffer = std::make_unique<uint8_t[]>(decoded_size);
  decoder->Feed(data, size);
  size_t written = 0;
  for (;;) {
    // Allow the decoder to consume a frame trailer after filling the buffer,
    // but reject any additional decoded byte.
    uint8_t extra;
    size_t remaining = decoded_size - written;
    auto result = decoder->ExtractOutput(
        remaining ? buffer.get() + written : &extra, remaining ? remaining : 1);
    PERFETTO_CHECK(result.ret != util::Decompressor::ResultCode::kError);
    PERFETTO_CHECK(result.bytes_written <= remaining);
    written += result.bytes_written;
    if (result.ret == util::Decompressor::ResultCode::kEof) {
      PERFETTO_CHECK(written == decoded_size && decoder->AvailIn() == 0);
      break;
    }
    PERFETTO_CHECK(result.ret == util::Decompressor::ResultCode::kOk);
  }
  SqlBundle bundle(buffer.get(), decoded_size);
  bundle.owned_data_ = std::move(buffer);
  return bundle;
}

}  // namespace perfetto::trace_processor
