/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/trace_parser.h"

#include "perfetto/base/utils.h"

namespace perfetto {
namespace trace_processor {

TraceParser::TraceParser(BlobReader* reader,
                         TraceStorage* trace,
                         uint32_t chunk_size_b)
    : reader_(reader), trace_(trace), chunk_size_b_(chunk_size_b) {}

void TraceParser::LoadNextChunk() {
  if (!buffer_)
    buffer_.reset(new uint8_t[chunk_size_b_]);

  uint32_t read = reader_->Read(offset_, chunk_size_b_, buffer_.get());
  if (read == 0)
    return;

  // TODO(lalitm): actually parse the data read here.
  base::ignore_result(trace_);

  offset_ += read;
}

}  // namespace trace_processor
}  // namespace perfetto
