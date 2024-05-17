
/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/util/file_buffer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"

namespace perfetto::trace_processor::util {

void FileBuffer::PushBack(TraceBlobView data) {
  if (data.size() == 0) {
    return;
  }
  const size_t size = data.size();
  data_.emplace_back(Entry{end_offset_, std::move(data)});
  end_offset_ += size;
}

bool FileBuffer::PopFrontUntil(const size_t target_offset) {
  PERFETTO_CHECK(file_offset() <= target_offset);
  while (!data_.empty()) {
    Entry& entry = data_.front();
    if (target_offset == entry.file_offset) {
      return true;
    }
    const size_t bytes_to_pop = target_offset - entry.file_offset;
    if (entry.data.size() > bytes_to_pop) {
      entry.data =
          entry.data.slice_off(bytes_to_pop, entry.data.size() - bytes_to_pop);
      entry.file_offset += bytes_to_pop;
      return true;
    }
    data_.pop_front();
  }

  return target_offset == end_offset_;
}

std::optional<TraceBlobView> FileBuffer::SliceOff(size_t start_offset,
                                                  size_t length) const {
  if (length == 0) {
    return TraceBlobView();
  }

  if (start_offset + length > end_offset_) {
    return std::nullopt;
  }

  Iterator it = FindEntryWithOffset(start_offset);
  if (it == end()) {
    return std::nullopt;
  }

  const size_t offset_from_entry_start = start_offset - it->file_offset;
  const size_t bytes_in_entry = it->data.size() - offset_from_entry_start;
  TraceBlobView first_blob = it->data.slice_off(
      offset_from_entry_start, std::min(bytes_in_entry, length));

  if (first_blob.size() == length) {
    return std::move(first_blob);
  }

  auto buffer = TraceBlob::Allocate(length);
  uint8_t* ptr = buffer.data();

  memcpy(ptr, first_blob.data(), first_blob.size());
  ptr += first_blob.size();
  length -= first_blob.size();
  ++it;

  while (length != 0) {
    PERFETTO_DCHECK(it != end());
    const size_t bytes_to_copy = std::min(length, it->data.size());
    memcpy(ptr, it->data.data(), bytes_to_copy);
    ptr += bytes_to_copy;
    length -= bytes_to_copy;
    ++it;
  }

  return TraceBlobView(std::move(buffer));
}

FileBuffer::Iterator FileBuffer::FindEntryWithOffset(size_t offset) const {
  if (offset >= end_offset_) {
    return end();
  }

  auto it = std::upper_bound(
      data_.begin(), data_.end(), offset,
      [](size_t offset, const Entry& rhs) { return offset < rhs.file_offset; });
  // This can only happen if too much data was popped.
  PERFETTO_CHECK(it != data_.begin());
  return std::prev(it);
}

}  // namespace perfetto::trace_processor::util
