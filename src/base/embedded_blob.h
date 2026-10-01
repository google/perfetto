/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_BASE_EMBEDDED_BLOB_H_
#define SRC_BASE_EMBEDDED_BLOB_H_

#include <cstddef>
#include <cstdint>
#include <memory>

namespace perfetto::base {

// Owns decoded storage, or borrows the static payload when it is uncompressed.
// Byte pointers and iterators remain valid only for this object's lifetime.
class DecodedBlob {
 public:
  DecodedBlob(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  // Decode the generated codec/size envelope into one exactly sized allocation.
  // The input is trusted generated data; corrupt data or unsupported codecs
  // indicate a build error and are fatal.
  static DecodedBlob Decode(const uint8_t* data, size_t size);

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  const uint8_t* begin() const { return data(); }
  const uint8_t* end() const { return size_ ? data() + size_ : data(); }

 private:
  std::unique_ptr<uint8_t[]> owned_data_;
  const uint8_t* data_;
  size_t size_;
};

}  // namespace perfetto::base

#endif  // SRC_BASE_EMBEDDED_BLOB_H_
