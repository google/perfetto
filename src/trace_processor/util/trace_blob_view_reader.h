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

#ifndef SRC_TRACE_PROCESSOR_UTIL_TRACE_BLOB_VIEW_READER_H_
#define SRC_TRACE_PROCESSOR_UTIL_TRACE_BLOB_VIEW_READER_H_

#include <cstddef>
#include <optional>

#include "perfetto/ext/base/circular_queue.h"
#include "perfetto/trace_processor/trace_blob_view.h"

namespace perfetto::trace_processor::util {

// Helper class which handles all the complexity of reading pieces of data which
// span across multiple TraceBlobView chunks. It takes care of:
//  1) Buffering data until it can be read.
//  2) Stitching together the cross-chunk spanning pieces.
//  3) Dropping data when it is no longer necessary to be buffered.
class TraceBlobViewReader {
 public:
  // Adds a `TraceBlobView` at the back.
  void PushBack(TraceBlobView);

  // Shrinks the buffer by dropping data from the front of the buffer until the
  // given offset is reached. If not enough data is present as much data as
  // possible will be dropped and `false` will be returned.
  //
  // NOTE: If `offset` < 'file_offset()' this method will CHECK fail.
  bool PopFrontUntil(size_t offset);

  // Shrinks the buffer by dropping `bytes` from the front of the buffer. If not
  // enough data is present as much data as possible will be dropped and `false`
  // will be returned.
  bool PopFrontBytes(size_t bytes) {
    return PopFrontUntil(start_offset() + bytes);
  }

  // Creates a TraceBlobView by slicing this reader starting at |offset| and
  // spanning |length| bytes.
  //
  // If possible, this method will try to avoid copies and simply slice an
  // input TraceBlobView. However, that may not be possible and it so, it will
  // allocate a new chunk of memory and copy over the data instead.
  //
  // NOTE: If `offset` < 'file_offset()' this method will CHECK fail.
  std::optional<TraceBlobView> SliceOff(size_t offset, size_t length) const;

  // Returns the offset to the start of the available data.
  size_t start_offset() const {
    return data_.empty() ? end_offset_ : data_.front().start_offset;
  }

  // Returns the offset to the end of the available data.
  size_t end_offset() const { return end_offset_; }

  // Returns the number of bytes of buffered data.
  size_t avail() const { return end_offset() - start_offset(); }

  bool empty() const { return data_.empty(); }

 private:
  struct Entry {
    // File offset of the first byte in `data`.
    size_t start_offset;
    TraceBlobView data;
  };
  using Iterator = base::CircularQueue<Entry>::Iterator;

  // CircularQueue has no const_iterator, so mutable is needed to access it from
  // const methods.
  mutable base::CircularQueue<Entry> data_;
  size_t end_offset_ = 0;
};

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_TRACE_BLOB_VIEW_READER_H_
