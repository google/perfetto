/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PROTOZERO_SCATTERED_HEAP_BUFFER_H_
#define INCLUDE_PERFETTO_PROTOZERO_SCATTERED_HEAP_BUFFER_H_

#include <memory>
#include <vector>

#include "perfetto/base/export.h"
#include "perfetto/base/logging.h"
#include "perfetto/protozero/scattered_stream_writer.h"

namespace protozero {

class PERFETTO_EXPORT ScatteredHeapBuffer
    : public protozero::ScatteredStreamWriter::Delegate {
 public:
  class PERFETTO_EXPORT Slice {
   public:
    explicit Slice(size_t size);
    Slice(Slice&& slice) noexcept;
    ~Slice();

    inline protozero::ContiguousMemoryRange GetTotalRange() const {
      return {buffer_.get(), buffer_.get() + size_};
    }

    inline protozero::ContiguousMemoryRange GetUsedRange() const {
      return {buffer_.get(), buffer_.get() + size_ - unused_bytes_};
    }

    uint8_t* start() const { return buffer_.get(); }
    size_t size() const { return size_; }
    size_t unused_bytes() const { return unused_bytes_; }
    void set_unused_bytes(size_t unused_bytes) {
      PERFETTO_DCHECK(unused_bytes_ <= size_);
      unused_bytes_ = unused_bytes;
    }

   private:
    std::unique_ptr<uint8_t[]> buffer_;
    const size_t size_;
    size_t unused_bytes_;
  };

  ScatteredHeapBuffer(size_t initial_slice_size_bytes = 128,
                      size_t maximum_slice_size_bytes = 128 * 1024);
  ~ScatteredHeapBuffer() override;

  // protozero::ScatteredStreamWriter::Delegate implementation.
  protozero::ContiguousMemoryRange GetNewBuffer() override;

  // Stitch all the slices into a single contiguous buffer.
  std::vector<uint8_t> StitchSlices();

  const std::vector<Slice>& slices() const { return slices_; }

  void set_writer(protozero::ScatteredStreamWriter* writer) {
    writer_ = writer;
  }

  // Update unused_bytes() of the current |Slice| based on the writer's state.
  void AdjustUsedSizeOfCurrentSlice();

  // Returns the total size the slices occupy in heap memory (including unused).
  size_t GetTotalSize();

 private:
  size_t next_slice_size_;
  const size_t maximum_slice_size_;
  protozero::ScatteredStreamWriter* writer_ = nullptr;
  std::vector<Slice> slices_;
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_SCATTERED_HEAP_BUFFER_H_
