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

#ifndef SRC_PROTOZERO_SCATTERED_STREAM_DELEGATE_FOR_TESTING_H_
#define SRC_PROTOZERO_SCATTERED_STREAM_DELEGATE_FOR_TESTING_H_

#include <memory>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/scattered_stream_writer.h"

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

}  // namespace perfetto

#endif  // SRC_PROTOZERO_SCATTERED_STREAM_DELEGATE_FOR_TESTING_H_
