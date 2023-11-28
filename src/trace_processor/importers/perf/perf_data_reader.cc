/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/perf/perf_data_reader.h"

#include <cstddef>
#include <optional>
#include <vector>
#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/trace_blob_view.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {
void Reader::SkipSlow(size_t bytes_to_skip) {
  size_t bytes_in_buffer = BytesInBuffer();

  // Size fits in buffer.
  if (bytes_in_buffer >= bytes_to_skip) {
    buffer_offset_ += bytes_to_skip;
    return;
  }

  // Empty the buffer and increase the |blob_offset_|.
  buffer_offset_ = 0;
  buffer_.clear();
  blob_offset_ += bytes_to_skip - bytes_in_buffer;
}

void Reader::PeekSlow(uint8_t* obj_data, size_t size) const {
  size_t bytes_in_buffer = BytesInBuffer();

  // Read from buffer.
  if (bytes_in_buffer >= size) {
    memcpy(obj_data, buffer_.data() + buffer_offset_, size);
    return;
  }

  // Read from blob and buffer.
  memcpy(obj_data, buffer_.data() + buffer_offset_, bytes_in_buffer);
  memcpy(obj_data + bytes_in_buffer, tbv_.data() + blob_offset_,
         size - bytes_in_buffer);
}

TraceBlobView Reader::PeekTraceBlobViewSlow(size_t size) const {
  auto blob = TraceBlob::Allocate(size);
  size_t bytes_in_buffer = BytesInBuffer();

  // Data is in buffer, so we need to create a new TraceBlob from it.
  if (bytes_in_buffer >= size) {
    memcpy(blob.data(), buffer_.data() + buffer_offset_, size);
    return TraceBlobView(std::move(blob));
  }

  // Data is in between blob and buffer and we need to dump data from buffer
  // and blob to a new TraceBlob.
  size_t bytes_from_blob = size - bytes_in_buffer;
  memcpy(blob.data(), buffer_.data() + buffer_offset_, bytes_in_buffer);
  memcpy(blob.data() + bytes_in_buffer, tbv_.data() + blob_offset_,
         bytes_from_blob);
  return TraceBlobView(std::move(blob));
}

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto
