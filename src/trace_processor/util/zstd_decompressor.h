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

#ifndef SRC_TRACE_PROCESSOR_UTIL_ZSTD_DECOMPRESSOR_H_
#define SRC_TRACE_PROCESSOR_UTIL_ZSTD_DECOMPRESSOR_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "perfetto/base/build_config.h"
#include "src/trace_processor/util/stream_decompressor.h"

struct ZSTD_DCtx_s;

namespace perfetto::trace_processor::util {

// Returns whether zstd related functionality is supported with the current
// build flags.
constexpr bool IsZstdSupported() {
#if PERFETTO_BUILDFLAG(PERFETTO_ZSTD)
  return true;
#else
  return false;
#endif
}

// zstd StreamDecompressor. Prefer obtaining one through
// CreateDecompressor(); see stream_decompressor.h for the streaming
// usage contract.
class ZstdDecompressor : public StreamDecompressor {
 public:
  ZstdDecompressor();

  void Feed(const uint8_t* data, size_t size) override;
  Result ExtractOutput(uint8_t* out, size_t out_capacity) override;
  void Reset() override;
  size_t AvailIn() const override;

 private:
  struct Deleter {
    void operator()(ZSTD_DCtx_s*) const;
  };
  std::unique_ptr<ZSTD_DCtx_s, Deleter> dstream_;
  const uint8_t* in_data_ = nullptr;
  size_t in_size_ = 0;
  size_t in_pos_ = 0;
  bool eof_ = false;
};

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_ZSTD_DECOMPRESSOR_H_
