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

#ifndef SRC_TRACE_PROCESSOR_UTIL_FILE_BUFFER_H_
#define SRC_TRACE_PROCESSOR_UTIL_FILE_BUFFER_H_

#include <cstddef>
#include <optional>

#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/trace_processor/trace_blob_view.h"

namespace perfetto::trace_processor::util {

// Helper class that exposes a window into the contents of a file. Data can be
// appended to the end of the buffer (increasing the size of the window) or
// removed from the front (decreasing the size of the window).
//
// TraceProcessor reads trace files in chunks and streams those to the
// `ChunkedTraceReader` instance. But sometimes the reader needs to look into
// the future (i.e. some data that has not yet arrived) before being able to
// process the current data. In such a case the reader would have to buffer data
// until the "future" data arrives. This class encapsulates that functionality.
class FileBuffer {
 public:
  // Trivial empty ctor.
  FileBuffer() = default;

  bool empty() const { return data_.empty(); }

  // Returns the offset to the start of the buffered window of data.
  size_t file_offset() const {
    return data_.empty() ? end_offset_ : data_.front().file_offset;
  }

  // Adds a `TraceBlobView` at the back.
  void PushBack(TraceBlobView view);

  // Shrinks the buffer by dropping data from the front of the buffer until the
  // given offset is reached. If not enough data is present as much data as
  // possible will be dropped and `false` will be returned.
  // ATTENTION: If `offset` < 'file_offset()' (i.e. you try to access data
  // previously popped) this method will CHECK fail.
  bool PopFrontUntil(size_t offset);

  // Shrinks the buffer by dropping `bytes` from the front of the buffer. If not
  // enough data is present as much data as possible will be dropped and `false`
  // will be returned.
  bool PopFrontBytes(size_t bytes) {
    return PopFrontUntil(file_offset() + bytes);
  }

  // Similar to `TraceBlobView::slice_off`, creates a slice with data starting
  // at `offset` and of the given `length`. This method might need to allocate a
  // new buffer and copy data into it (if the requested data spans multiple
  // TraceBlobView instances). If not enough data is present `std::nullopt` is
  // returned.
  //
  // ATTENTION: If `offset` < 'file_offset()' (i.e. you try to access data
  // previously popped) this method will CHECK fail.
  std::optional<TraceBlobView> SliceOff(size_t offset, size_t length) const;

 private:
  struct Entry {
    // File offset of the first byte in `data`.
    size_t file_offset;
    TraceBlobView data;
  };
  using Iterator = base::CircularQueue<Entry>::Iterator;
  // Finds the `TraceBlobView` at `offset` and returns a slice starting at that
  // offset and spanning the rest of the `TraceBlobView`. It also returns an
  // iterator to the next `TraceBlobView` instance (which might be `end()`).
  Iterator FindEntryWithOffset(size_t offset) const;

  Iterator end() const { return data_.end(); }

  // CircularQueue has no const_iterator, so mutable is needed to access it from
  // const methods.
  // CircularQueue has no const_iterator, so mutable is needed to access it from
  // const methods.
  mutable base::CircularQueue<Entry> data_;
  size_t end_offset_ = 0;
};

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_FILE_BUFFER_H_
