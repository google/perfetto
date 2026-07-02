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

#ifndef SRC_TRACE_PROCESSOR_UTIL_DECOMPRESS_H_
#define SRC_TRACE_PROCESSOR_UTIL_DECOMPRESS_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/util/stream_decompressor.h"

// Codec-agnostic entry point for decompression. Call sites sniff the codec with
// DetectCompression() (stream_decompressor.h), check IsCompressionSupported(),
// then either DecompressFully() for one-shot use or CreateDecompressor() for
// streaming. None of them name a concrete codec.

namespace perfetto::trace_processor::util {

// Whether this build was compiled with the library needed to decompress `type`.
// kNone is trivially "supported"; unsupported codecs make CreateDecompressor()
// return nullptr and DecompressFully() return an empty vector.
bool IsCompressionSupported(CompressionType type);

// Creates a streaming decompressor for `type`, or nullptr if `type` is kNone or
// the build wasn't compiled with support for it (see IsCompressionSupported).
std::unique_ptr<StreamDecompressor> CreateDecompressor(CompressionType type);

// Decompress an entire in-memory block at once. Returns the decompressed bytes,
// or an empty vector if `type` is unsupported or the input is corrupt. For
// large or streamed inputs use CreateDecompressor() instead.
std::vector<uint8_t> DecompressFully(CompressionType type,
                                     const uint8_t* data,
                                     size_t len);

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_DECOMPRESS_H_
