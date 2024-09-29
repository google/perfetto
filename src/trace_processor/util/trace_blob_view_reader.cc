
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

#include "src/trace_processor/util/trace_blob_view_reader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"

namespace perfetto::trace_processor::util {

void TraceBlobViewReader::PushBack(TraceBlobView data) {
  if (data.size() == 0) {
    return;
  }
  const size_t size = data.size();
  data_.emplace_back(Entry{end_offset_, std::move(data)});
  end_offset_ += size;
}

bool TraceBlobViewReader::PopFrontUntil(const size_t target_offset) {
  PERFETTO_CHECK(start_offset() <= target_offset);
  while (!data_.empty()) {
    Entry& entry = data_.front();
    if (target_offset == entry.start_offset) {
      return true;
    }
    const size_t bytes_to_pop = target_offset - entry.start_offset;
    if (entry.data.size() > bytes_to_pop) {
      entry.data =
          entry.data.slice_off(bytes_to_pop, entry.data.size() - bytes_to_pop);
      entry.start_offset += bytes_to_pop;
      return true;
    }
    data_.pop_front();
  }
  return target_offset == end_offset_;
}

std::optional<TraceBlobView> TraceBlobViewReader::SliceOff(
    size_t offset,
    size_t length) const {
  // If the length is zero, then a zero-sized blob view is always approrpriate.
  if (PERFETTO_UNLIKELY(length == 0)) {
    return TraceBlobView();
  }

  PERFETTO_DCHECK(offset >= start_offset());

  // Fast path: the slice fits entirely inside the first TBV, we can just slice
  // that directly without doing any searching. This will happen most of the
  // time when this class is used so optimize for it.
  bool is_fast_path =
      !data_.empty() &&
      offset + length <= data_.front().start_offset + data_.front().data.size();
  if (PERFETTO_LIKELY(is_fast_path)) {
    return data_.front().data.slice_off(offset - data_.front().start_offset,
                                        length);
  }

  // If we don't have any TBVs or the end of the slice does not fit, then we
  // cannot possibly return a full slice.
  if (PERFETTO_UNLIKELY(data_.empty() || offset + length > end_offset_)) {
    return std::nullopt;
  }

  // Find the first block finishes *after* start_offset i.e. there is at least
  // one byte in that block which will end up in the slice. We know this *must*
  // exist because of the above check.
  auto rit = std::upper_bound(
      data_.begin(), data_.end(), offset, [](size_t offset, const Entry& rhs) {
        return offset < rhs.start_offset + rhs.data.size();
      });
  PERFETTO_CHECK(rit != data_.end());

  // If the slice fits entirely in the block we found, then just slice that
  // block avoiding any copies.
  size_t rel_off = offset - rit->start_offset;
  if (rel_off + length <= rit->data.size()) {
    return rit->data.slice_off(rel_off, length);
  }

  // Otherwise, allocate some memory and make a copy.
  auto buffer = TraceBlob::Allocate(length);
  uint8_t* ptr = buffer.data();
  uint8_t* end = buffer.data() + buffer.size();

  // Copy all bytes in this block which overlap with the slice.
  memcpy(ptr, rit->data.data() + rel_off, rit->data.length() - rel_off);
  ptr += rit->data.length() - rel_off;

  for (auto it = rit + 1; ptr != end; ++it) {
    auto len = std::min(static_cast<size_t>(end - ptr), it->data.size());
    memcpy(ptr, it->data.data(), len);
    ptr += len;
  }
  return TraceBlobView(std::move(buffer));
}

}  // namespace perfetto::trace_processor::util
