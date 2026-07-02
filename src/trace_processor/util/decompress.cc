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

#include "src/trace_processor/util/decompress.h"

#include <memory>

#include "src/trace_processor/util/gzip_decompressor.h"
#include "src/trace_processor/util/stream_decompressor.h"
#include "src/trace_processor/util/zstd_decompressor.h"

namespace perfetto::trace_processor::util {

bool IsCompressionSupported(CompressionType type) {
  switch (type) {
    case CompressionType::kNone:
      return true;
    case CompressionType::kGzip:
    case CompressionType::kRawDeflate:
      return IsGzipSupported();
    case CompressionType::kZstd:
      return IsZstdSupported();
  }
  return false;
}

std::unique_ptr<StreamDecompressor> CreateDecompressor(CompressionType type) {
  if (!IsCompressionSupported(type)) {
    return nullptr;
  }
  switch (type) {
    case CompressionType::kNone:
      return nullptr;
    case CompressionType::kGzip:
      return std::make_unique<GzipDecompressor>(
          GzipDecompressor::InputMode::kGzip);
    case CompressionType::kRawDeflate:
      return std::make_unique<GzipDecompressor>(
          GzipDecompressor::InputMode::kRawDeflate);
    case CompressionType::kZstd:
      return std::make_unique<ZstdDecompressor>();
  }
  return nullptr;
}

std::vector<uint8_t> DecompressFully(CompressionType type,
                                     const uint8_t* data,
                                     size_t len) {
  std::unique_ptr<StreamDecompressor> decompressor = CreateDecompressor(type);
  if (!decompressor) {
    return {};
  }
  // Returns whatever was extracted, without requiring the stream to end cleanly
  // at kEof; callers that care detect failure via the returned size or
  // downstream parsing.
  std::vector<uint8_t> output;
  decompressor->FeedAndExtract(data, len,
                               [&output](const uint8_t* ptr, size_t size) {
                                 output.insert(output.end(), ptr, ptr + size);
                               });
  return output;
}

}  // namespace perfetto::trace_processor::util
