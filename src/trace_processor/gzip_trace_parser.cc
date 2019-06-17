/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/gzip_trace_parser.h"

#include <zlib.h>

#include "src/trace_processor/systrace_trace_parser.h"

namespace perfetto {
namespace trace_processor {

GzipTraceParser::GzipTraceParser(TraceProcessorContext* context)
    : context_(context), z_stream_(new z_stream()) {
  z_stream_->zalloc = Z_NULL;
  z_stream_->zfree = Z_NULL;
  z_stream_->opaque = Z_NULL;
  inflateInit(z_stream_.get());
}

GzipTraceParser::~GzipTraceParser() {
  // Ensure the call to inflateEnd to prevent leaks of internal state.
  inflateEnd(z_stream_.get());
}

util::Status GzipTraceParser::Parse(std::unique_ptr<uint8_t[]> data,
                                    size_t size) {
  uint8_t* start = data.get();
  size_t len = size;

  static const char kSystraceFilerHeader[] = "TRACE:\n";
  if (!inner_) {
    inner_.reset(new SystraceTraceParser(context_));

    // Strip the header by ignoring the associated bytes.
    start += strlen(kSystraceFilerHeader);
    len -= strlen(kSystraceFilerHeader);
  }

  z_stream_->next_in = start;
  z_stream_->avail_in = static_cast<uInt>(len);

  // Our default uncompressed buffer size is 32MB as it allows for good
  // throughput.
  constexpr size_t kUncompressedBufferSize = 32 * 1024 * 1024;
  int ret = Z_OK;
  for (; ret != Z_STREAM_END && z_stream_->avail_in != 0;) {
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[kUncompressedBufferSize]);
    z_stream_->next_out = buffer.get();
    z_stream_->avail_out = static_cast<uInt>(kUncompressedBufferSize);

    ret = inflate(z_stream_.get(), Z_NO_FLUSH);
    switch (ret) {
      case Z_NEED_DICT:
      case Z_DATA_ERROR:
      case Z_MEM_ERROR:
        // Ignore inflateEnd error as we will error out anyway.
        inflateEnd(z_stream_.get());
        return util::ErrStatus("Error decompressing ctrace file");
    }

    size_t read = kUncompressedBufferSize - z_stream_->avail_out;
    util::Status status = inner_->Parse(std::move(buffer), read);
    if (!status.ok())
      return status;
  }
  if (ret == Z_STREAM_END) {
    ret = inflateEnd(z_stream_.get());
    if (ret == Z_STREAM_ERROR)
      return util::ErrStatus("Error finishing decompression");
  }
  return util::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
