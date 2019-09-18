/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PROTOZERO_PACKED_REPEATED_FIELDS_H_
#define INCLUDE_PERFETTO_PROTOZERO_PACKED_REPEATED_FIELDS_H_

#include <stdint.h>

#include <array>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_utils.h"

namespace protozero {

// This file contains classes used when encoding packed repeated fields.
// To encode such a field, the caller is first expected to accumulate all of the
// values in one of the following types (depending on the wire type of the
// individual elements), defined below:
// * protozero::PackedVarIntBuffer
// * protozero::PackedFixedSizeBuffer</*element_type=*/ uint32_t>
// Then that buffer is passed to the protozero-generated setters as an argument.
// After calling the setter, the buffer can be destroyed.
//
// The buffer classes by themselves do not own the raw memory used to hold the
// accumulated values, so they need to be instantiated via a subclass that
// implements the storage. This file provides one such type, StackAllocated, for
// buffering the values using the stack.
//
// An example of encoding a packed field:
//   protozero::HeapBuffered<protozero::Message> msg;
//   // stack buffer holding up to 128 varints
//   protozero::StackAllocated<protozero::PackedVarIntBuffer, 128> buf;
//   buf.Append(42);
//   buf.Append(-1);
//   msg->set_fieldname(buf);
//   msg.SerializeAsString();
class PackedVarIntBuffer {
 public:
  // worst case encoded size per varint
  using StorageElementType = std::array<uint8_t, 10>;

  template <typename T>
  void Append(T value) {
    PERFETTO_CHECK(++element_size_ <= element_capacity_);
    write_ptr_ = proto_utils::WriteVarInt(value, write_ptr_);
  }

  void Reset() {
    write_ptr_ = storage_begin_;
    element_size_ = 0;
  }

  const uint8_t* data() const { return storage_begin_; }

  size_t size() const {
    return static_cast<size_t>(write_ptr_ - storage_begin_);
  }

 protected:
  PackedVarIntBuffer(StorageElementType* storage, size_t storage_capacity)
      : storage_begin_(reinterpret_cast<uint8_t*>(storage)),
        write_ptr_(reinterpret_cast<uint8_t*>(storage)),
        element_capacity_(storage_capacity) {}

 private:
  // Storage will consist of an array of StorageElementType, but we treat it as
  // a contiguous uint8_t buffer. Note that the varints will not be aligned with
  // StorageElementType boundaries.
  uint8_t* const storage_begin_;
  uint8_t* write_ptr_;

  size_t element_size_ = 0;
  const size_t element_capacity_;
};

template <typename ElementType>
class PackedFixedSizeBuffer {
 public:
  // The template type parameter is the same as the storage type, so one
  // of the type names is used in all places for consistency.
  using StorageElementType = ElementType;

  void Append(StorageElementType value) {
    PERFETTO_CHECK(write_ptr_ < storage_end_);
    *(write_ptr_++) = value;
  }

  void Reset() { write_ptr_ = storage_begin_; }

  const uint8_t* data() const {
    return reinterpret_cast<const uint8_t*>(storage_begin_);
  }

  size_t size() const {
    return static_cast<size_t>(reinterpret_cast<uint8_t*>(write_ptr_) -
                               reinterpret_cast<uint8_t*>(storage_begin_));
  }

 protected:
  PackedFixedSizeBuffer(StorageElementType* storage,
                        size_t max_storage_elements)
      : storage_begin_(storage),
        storage_end_(storage + max_storage_elements),
        write_ptr_(storage) {
    static_assert(
        sizeof(StorageElementType) == 4 || sizeof(StorageElementType) == 8,
        "invalid type width");
  }

 private:
  StorageElementType* const storage_begin_;
  StorageElementType* const storage_end_;
  StorageElementType* write_ptr_;
};

template <typename PackedBuffer, size_t MaxNumElements>
class StackAllocated : public PackedBuffer {
 public:
  StackAllocated() : PackedBuffer(storage_, MaxNumElements) {}

 private:
  typename PackedBuffer::StorageElementType storage_[MaxNumElements];
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_PACKED_REPEATED_FIELDS_H_
