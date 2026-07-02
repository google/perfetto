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

#ifndef SRC_TRACE_PROCESSOR_UTIL_STREAM_DECOMPRESSOR_H_
#define SRC_TRACE_PROCESSOR_UTIL_STREAM_DECOMPRESSOR_H_

#include <cstddef>
#include <cstdint>

namespace perfetto::trace_processor::util {

// The compression codecs trace_processor can detect and decompress. To add one,
// give it a magic in DetectCompression() and a case in the decompress.h
// factory.
enum class CompressionType {
  // Not compressed, or a header we don't recognize.
  kNone,
  // gzip-framed deflate (e.g. a .gz file). Magic bytes 0x1f 0x8b.
  kGzip,
  // Headerless deflate, as stored in .zip entries. Has no magic of its own, so
  // DetectCompression() never returns it; callers that know they are looking at
  // a zip entry pass it explicitly.
  kRawDeflate,
  // A zstd frame. Magic bytes 0x28 0xb5 0x2f 0xfd.
  kZstd,
};

// Leading bytes that identify a self-describing codec from a stream header.
inline constexpr uint8_t kGzipMagic[] = {0x1f, 0x8b};
inline constexpr uint8_t kZstdMagic[] = {0x28, 0xb5, 0x2f, 0xfd};

// Sniffs the leading bytes of `data` and returns the codec, or kNone if the
// header matches no known codec (or `size` is too small to tell). Never returns
// kRawDeflate (raw deflate is not self-describing).
CompressionType DetectCompression(const uint8_t* data, size_t size);

// Picks the codec for an in-trace `compressed_packets` blob. These carry no
// codec tag: a zstd blob starts with the zstd magic, a deflate blob is
// zlib-wrapped and has none. Returns kZstd on a zstd magic, else kGzip (whose
// auto-detect handles zlib- and gzip-wrapped deflate).
CompressionType DetectPacketCompression(const uint8_t* data, size_t size);

// Codec-agnostic streaming decompressor. Concrete codecs (gzip, zstd, ...)
// subclass this; obtain one via CreateDecompressor() in decompress.h so call
// sites never branch on the codec.
//
// Streaming usage, two ways:
// 1. [Common] Feed each input block to FeedAndExtract(); output is delivered to
//    the callback, possibly several times per call.
// 2. [Low-level] Call Feed() once, then ExtractOutput() repeatedly until it
//    returns kEof or kNeedsMoreInput. See ResultCode.
class StreamDecompressor {
 public:
  enum class ResultCode {
    // Made progress; keep calling ExtractOutput to drain more output.
    kOk,
    // The stream is fully decompressed; no more input is needed.
    kEof,
    // Corrupt/invalid input.
    kError,
    // All available input was consumed but the stream is not complete; feed the
    // next mem-block and continue.
    kNeedsMoreInput,
  };
  struct Result {
    ResultCode ret;
    // Bytes written to output. Valid in all cases except |ResultCode::kError|.
    size_t bytes_written;
  };

  StreamDecompressor() = default;
  virtual ~StreamDecompressor();

  // Hands out / holds internal pointers; never copy or move.
  StreamDecompressor(const StreamDecompressor&) = delete;
  StreamDecompressor& operator=(const StreamDecompressor&) = delete;
  StreamDecompressor(StreamDecompressor&&) = delete;
  StreamDecompressor& operator=(StreamDecompressor&&) = delete;

  // Feed the next input mem-block.
  virtual void Feed(const uint8_t* data, size_t size) = 0;

  // Extract the newly available partial output. After each Feed(), call this
  // repeatedly until it returns kEof or kNeedsMoreInput.
  virtual Result ExtractOutput(uint8_t* out, size_t out_capacity) = 0;

  // Reset to decode the next stream/frame, reusing internal buffers. Any fed
  // but unconsumed input is preserved (so multi-stream inputs can continue).
  virtual void Reset() = 0;

  // The amount of input bytes left unprocessed.
  virtual size_t AvailIn() const = 0;

  // Feed the next mem-block and push all resulting output to `output_consumer`,
  // which may be invoked any number of times. The returned code is guaranteed
  // not to be kOk.
  template <typename Callback>
  ResultCode FeedAndExtract(const uint8_t* data,
                            size_t size,
                            const Callback& output_consumer) {
    Feed(data, size);
    uint8_t buffer[4096];
    Result result;
    do {
      result = ExtractOutput(buffer, sizeof(buffer));
      if (result.ret != ResultCode::kError && result.bytes_written > 0) {
        output_consumer(buffer, result.bytes_written);
      }
    } while (result.ret == ResultCode::kOk);
    return result.ret;
  }
};

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_STREAM_DECOMPRESSOR_H_
