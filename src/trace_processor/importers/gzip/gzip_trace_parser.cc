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

#include "src/trace_processor/importers/gzip/gzip_trace_parser.h"

#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/util/gzip_utils.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto {
namespace trace_processor {

namespace {

using ResultCode = util::GzipDecompressor::ResultCode;

}  // namespace

GzipTraceParser::GzipTraceParser(TraceProcessorContext* context)
    : context_(context) {}

GzipTraceParser::GzipTraceParser(std::unique_ptr<ChunkedTraceReader> reader)
    : context_(nullptr), inner_(std::move(reader)) {}

GzipTraceParser::~GzipTraceParser() = default;

util::Status GzipTraceParser::Parse(TraceBlobView blob) {
  return ParseUnowned(blob.data(), blob.size());
}

util::Status GzipTraceParser::ParseUnowned(const uint8_t* data, size_t size) {
  const uint8_t* start = data;
  size_t len = size;

  if (!inner_) {
    PERFETTO_CHECK(context_);
    inner_.reset(new ForwardingTraceParser(context_));
  }

  if (!first_chunk_parsed_) {
    // .ctrace files begin with: "TRACE:\n" or "done. TRACE:\n" strip this if
    // present.
    base::StringView beginning(reinterpret_cast<const char*>(start), size);

    static const char* kSystraceFileHeader = "TRACE:\n";
    size_t offset = Find(kSystraceFileHeader, beginning);
    if (offset != std::string::npos) {
      start += strlen(kSystraceFileHeader) + offset;
      len -= strlen(kSystraceFileHeader) + offset;
    }
    first_chunk_parsed_ = true;
  }

  // Our default uncompressed buffer size is 32MB as it allows for good
  // throughput.
  constexpr size_t kUncompressedBufferSize = 32 * 1024 * 1024;

  needs_more_input_ = false;
  decompressor_.Feed(start, len);

  for (auto ret = ResultCode::kOk; ret != ResultCode::kEof;) {
    if (!buffer_) {
      buffer_.reset(new uint8_t[kUncompressedBufferSize]);
      bytes_written_ = 0;
    }

    auto result =
        decompressor_.ExtractOutput(buffer_.get() + bytes_written_,
                                    kUncompressedBufferSize - bytes_written_);
    ret = result.ret;
    if (ret == ResultCode::kError)
      return util::ErrStatus("Failed to decompress trace chunk");

    if (ret == ResultCode::kNeedsMoreInput) {
      PERFETTO_DCHECK(result.bytes_written == 0);
      needs_more_input_ = true;
      return util::OkStatus();
    }
    bytes_written_ += result.bytes_written;

    if (bytes_written_ == kUncompressedBufferSize || ret == ResultCode::kEof) {
      TraceBlob blob =
          TraceBlob::TakeOwnership(std::move(buffer_), bytes_written_);
      RETURN_IF_ERROR(inner_->Parse(TraceBlobView(std::move(blob))));
    }
  }
  return util::OkStatus();
}

void GzipTraceParser::NotifyEndOfFile() {
  // TODO(lalitm): this should really be an error returned to the caller but
  // due to historical implementation, NotifyEndOfFile does not return a
  // util::Status.
  PERFETTO_DCHECK(!needs_more_input_);
  PERFETTO_DCHECK(!buffer_);

  if (inner_)
    inner_->NotifyEndOfFile();
}

}  // namespace trace_processor
}  // namespace perfetto
