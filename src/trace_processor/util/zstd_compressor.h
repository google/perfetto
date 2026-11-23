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

#ifndef SRC_TRACE_PROCESSOR_UTIL_ZSTD_COMPRESSOR_H_
#define SRC_TRACE_PROCESSOR_UTIL_ZSTD_COMPRESSOR_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/ext/base/utils.h"

struct ZSTD_CCtx_s;

namespace perfetto::trace_processor::util {

// One-shot zstd compression, the counterpart of ZstdDecompressor. Check
// IsZstdSupported() (decompressor.h) before relying on this.
class ZstdCompressor {
 public:
  // Compresses |data| into a single zstd frame, returning a malloc()-allocated
  // buffer (caller frees; the frame length is written to |*out_size|) or
  // nullptr on error or when zstd is not compiled in.
  static std::unique_ptr<uint8_t, base::FreeDeleter>
  CompressFully(const uint8_t* data, size_t len, size_t* out_size, int level);
};

// Streams input into zstd frames one at a time, reusing one context across
// frames. Check IsZstdSupported() (decompressor.h) before relying on this.
class ZstdStreamCompressor {
 public:
  explicit ZstdStreamCompressor(int level);
  ~ZstdStreamCompressor();

  // Starts a frame which will hold exactly |size| bytes of input.
  bool BeginFrame(uint64_t size);
  bool Append(const uint8_t* data, size_t size, std::vector<uint8_t>* out);
  bool EndFrame(std::vector<uint8_t>* out);

 private:
  struct Deleter {
    void operator()(ZSTD_CCtx_s*) const;
  };

  bool Compress(const uint8_t* data,
                size_t size,
                bool end,
                std::vector<uint8_t>* out);

  std::unique_ptr<ZSTD_CCtx_s, Deleter> cctx_;
};

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_ZSTD_COMPRESSOR_H_
