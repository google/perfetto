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

#ifndef FTRACE_READER_SCATTERED_STREAM_DELEGATE_FOR_TESTING_H_
#define FTRACE_READER_SCATTERED_STREAM_DELEGATE_FOR_TESTING_H_

#include "protos/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto {

class ScatteredStreamDelegateForTesting
    : public protozero::ScatteredStreamWriter::Delegate {
 public:
  explicit ScatteredStreamDelegateForTesting(size_t chunk_size);
  ~ScatteredStreamDelegateForTesting() override;

  // protozero::ScatteredStreamWriter::Delegate implementation.
  protozero::ContiguousMemoryRange GetNewBuffer() override;

  // Stitch all the chunks into a single contiguous buffer.
  std::unique_ptr<uint8_t[]> StitchChunks(size_t size);

  const std::vector<std::unique_ptr<uint8_t[]>>& chunks() const {
    return chunks_;
  }

  void set_writer(protozero::ScatteredStreamWriter* writer) {
    writer_ = writer;
  }

 private:
  const size_t chunk_size_;
  protozero::ScatteredStreamWriter* writer_ = nullptr;
  std::vector<size_t> chunks_used_size_;
  std::vector<std::unique_ptr<uint8_t[]>> chunks_;
};

ScatteredStreamDelegateForTesting::ScatteredStreamDelegateForTesting(
    size_t chunk_size)
    : chunk_size_(chunk_size) {}

ScatteredStreamDelegateForTesting::~ScatteredStreamDelegateForTesting() {}

protozero::ContiguousMemoryRange
ScatteredStreamDelegateForTesting::GetNewBuffer() {
  PERFETTO_CHECK(writer_);
  if (chunks_.size()) {
    size_t used = chunk_size_ - writer_->bytes_available();
    chunks_used_size_.push_back(used);
  }
  std::unique_ptr<uint8_t[]> chunk(new uint8_t[chunk_size_]);
  uint8_t* begin = chunk.get();
  memset(begin, 0xff, chunk_size_);
  chunks_.push_back(std::move(chunk));
  return {begin, begin + chunk_size_};
}

std::unique_ptr<uint8_t[]> ScatteredStreamDelegateForTesting::StitchChunks(
    size_t size) {
  std::unique_ptr<uint8_t[]> buffer =
      std::unique_ptr<uint8_t[]>(new uint8_t[size]);
  size_t remaining = size;
  size_t i = 0;
  for (const auto& chunk : chunks_) {
    size_t chunk_size = remaining;
    if (i < chunks_used_size_.size()) {
      chunk_size = chunks_used_size_[i];
    }
    PERFETTO_CHECK(chunk_size <= chunk_size_);
    memcpy(buffer.get() + size - remaining, chunk.get(), chunk_size);
    remaining -= chunk_size;
    i++;
  }

  return buffer;
}

}  // namespace perfetto

#endif  // FTRACE_READER_SCATTERED_STREAM_DELEGATE_FOR_TESTING_H_
