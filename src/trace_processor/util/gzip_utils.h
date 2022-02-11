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

#ifndef SRC_TRACE_PROCESSOR_UTIL_GZIP_UTILS_H_
#define SRC_TRACE_PROCESSOR_UTIL_GZIP_UTILS_H_

#include <memory>

struct z_stream_s;

namespace perfetto {
namespace trace_processor {
namespace util {

// Returns whether gzip related functioanlity is supported with the current
// build flags.
bool IsGzipSupported();

// Usage: To decompress in a streaming way, feed the sequence of mem-blocks,
// one by one, by calling 'SetInput'. For each time 'SetInput' is called,
// client should call 'Decompress' again and again to extrat the partially
// available output, until there in no more output to extract.
class GzipDecompressor {
 public:
  enum class ResultCode {
    // 'kOk' means nothing bad happened so far, but continue doing what you
    // were doing.
    kOk,
    // While calling 'Decompress' repeatedly, if we get 'kEof', it means
    // we have extracted all the partially available data and we are also
    // done, i.e. there is no need to feed more input.
    kEof,
    // Some error. Possibly invalid compressed stream or corrupted data.
    kError,
    // While calling 'Decompress' repeatedly, if we get 'kNeedsMoreInput',
    // it means we have extracted all the partially available data, but we are
    // not done yet. We need to call the 'SetInput' to feed the next input
    // mem-block and go through the Decompress loop again.
    kNeedsMoreInput,
  };
  struct Result {
    // The return code of the decompression.
    ResultCode ret;

    // The amount of bytes written to output.
    // Valid in all cases except |ResultCode::kError|.
    size_t bytes_written;
  };

  GzipDecompressor();
  ~GzipDecompressor();
  GzipDecompressor(const GzipDecompressor&) = delete;
  GzipDecompressor& operator=(const GzipDecompressor&) = delete;

  // Feed the next mem-block.
  void SetInput(const uint8_t* data, size_t size);

  // Extract the newly available partial output. On each 'SetInput', this method
  // should be called repeatedly until there is no more data to output
  // i.e. (either 'kEof' or 'kNeedsMoreInput').
  Result Decompress(uint8_t* out, size_t out_size);

  // Sets the state of the decompressor to reuse with other gzip streams.
  // This is almost like constructing a new 'GzipDecompressor' object
  // but without paying the cost of internal memory allocation.
  void Reset();

 private:
  std::unique_ptr<z_stream_s> z_stream_;
};

}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_UTIL_GZIP_UTILS_H_
