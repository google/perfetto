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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_READER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_READER_H_

#include <stdint.h>
#include <cstddef>
#include <cstring>
#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {

// Reader class for tokenizing and parsing. Currently used by perf importer, but
// it's design is not related to perf. Responsible for hiding away the
// complexity of reading values from TraceBlobView and glueing the tbvs together
// in case there is data between many of them.
class Reader {
 public:
  Reader() = default;
  explicit Reader(TraceBlobView tbv) : tbv_(std::move(tbv)) {}

  // Updates old TraceBlobView with new one. If there is data left in the old
  // one, it will be saved in the buffer.
  void Append(TraceBlobView tbv) {
    uint64_t size_before = BytesAvailable();
    buffer_.insert(buffer_.end(), tbv_.data() + blob_offset_,
                   tbv_.data() + tbv_.size());
    tbv_ = std::move(tbv);
    blob_offset_ = 0;

    // Post condition. Checks whether no data has been lost in the Append.
    PERFETTO_DCHECK(BytesAvailable() == size_before + tbv.size());
  }

  // Reads the |obj| and updates |file_offset_| of the reader.
  // NOTE: Assumes count of bytes available is higher than sizeof(T).
  template <typename T>
  void Read(T& obj) {
    Peek(obj);
    Skip<T>();
  }

  // Reads the T value for std::optional<T>.
  // NOTE: Assumes count of bytes available is higher than sizeof(T).
  template <typename T>
  void ReadOptional(std::optional<T>& obj) {
    T val;
    Read(val);
    obj = val;
  }

  // Reads all of the data in the |vec| and updates |file_offset_| of the
  // reader.
  // NOTE: Assumes count of bytes available is higher than sizeof(T).
  template <typename T>
  void ReadVector(std::vector<T>& vec) {
    PERFETTO_DCHECK(CanReadSize(sizeof(T) * vec.size()));
    for (T& val : vec) {
      Read(val);
    }
  }

  // Updates the |file_offset_| by the sizeof(T).
  // NOTE: Assumes count of bytes available is higher than sizeof(T).
  template <typename T>
  void Skip() {
    Skip(sizeof(T));
  }

  // Updates the |file_offset_| by the |bytes_to_skip|.
  // NOTE: Assumes count of bytes available is higher than sizeof(T).
  void Skip(uint64_t bytes_to_skip) {
    uint64_t bytes_available_before = BytesAvailable();
    PERFETTO_DCHECK(CanReadSize(bytes_to_skip));
    size_t skip = static_cast<size_t>(bytes_to_skip);

    // Incrementing file offset is not related to the way data is split.
    file_offset_ += skip;
    size_t bytes_in_buffer = BytesInBuffer();

    // Empty buffer. Increment |blob_offset_|.
    if (PERFETTO_LIKELY(bytes_in_buffer == 0)) {
      buffer_offset_ = 0;
      buffer_.clear();
      blob_offset_ += skip;
    } else {
      SkipSlow(skip);
    }
    PERFETTO_DCHECK(BytesAvailable() == bytes_available_before - skip);
  }

  // Peeks the |obj| without updating the |file_offset_| of the reader.
  // NOTE: Assumes count of bytes available is higher than sizeof(T).
  template <typename T>
  void Peek(T& obj) const {
    PERFETTO_DCHECK(CanReadSize(sizeof(T)));
    size_t bytes_available_before = BytesAvailable();

    // Read from blob.
    if (PERFETTO_LIKELY(BytesInBuffer() == 0)) {
      memcpy(&obj, tbv_.data() + blob_offset_, sizeof(T));
    } else {
      PeekSlow(reinterpret_cast<uint8_t*>(&obj), sizeof(T));
    }

    PERFETTO_DCHECK(BytesAvailable() == bytes_available_before);
  }

  // Creates TraceBlobView with data of |data_size| bytes from current offset.
  // NOTE: Assumes count of bytes available is higher than sizeof(T).
  TraceBlobView PeekTraceBlobView(uint64_t data_size) const {
    PERFETTO_DCHECK(CanReadSize(data_size));
    size_t size = static_cast<size_t>(data_size);
    size_t bytes_in_buffer = BytesInBuffer();

    // Data is in blob, so it's enough to slice the existing |tbv_|.
    if (PERFETTO_LIKELY(bytes_in_buffer == 0)) {
      return tbv_.slice(tbv_.data() + blob_offset_, size);
    }
    return PeekTraceBlobViewSlow(size);
  }

  // Returns if there is enough data to read offsets between |start| and |end|.
  bool CanAccessFileRange(uint64_t start, uint64_t end) const {
    return CanAccessFileOffset(static_cast<size_t>(start)) &&
           CanAccessFileOffset(static_cast<size_t>(end));
  }

  // Returns if there is enough data to read |size| bytes.
  bool CanReadSize(uint64_t size) const { return size <= BytesAvailable(); }

  uint64_t current_file_offset() const { return file_offset_; }

 private:
  void SkipSlow(size_t bytes_to_skip);

  void PeekSlow(uint8_t* obj_data, size_t) const;

  TraceBlobView PeekTraceBlobViewSlow(size_t) const;

  size_t BytesInBuffer() const {
    PERFETTO_DCHECK(buffer_.size() >= buffer_offset_);
    return buffer_.size() - buffer_offset_;
  }
  size_t BytesInBlob() const { return tbv_.size() - blob_offset_; }
  size_t BytesAvailable() const { return BytesInBuffer() + BytesInBlob(); }

  bool CanAccessFileOffset(size_t off) const {
    return off >= file_offset_ && off <= file_offset_ + BytesAvailable();
  }

  TraceBlobView tbv_;
  std::vector<uint8_t> buffer_;

  // Where we are in relation to the current blob.
  size_t blob_offset_ = 0;
  // Where we are in relation to the file.
  size_t file_offset_ = 0;
  // Where we are in relation to the buffer.
  size_t buffer_offset_ = 0;
};
}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_DATA_READER_H_
