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

#ifndef SRC_TRACE_PROCESSOR_CORE_COMMON_ROW_LAYOUT_H_
#define SRC_TRACE_PROCESSOR_CORE_COMMON_ROW_LAYOUT_H_

#include <cstdint>
#include <cstring>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/endian.h"
#include "perfetto/base/logging.h"

namespace perfetto::trace_processor::core {

// Lays rows out in fixed-width slots whose bytes compare as their values do.
// A nullable slot starts with 0xFF if the row has a value and 0 if not;
// a descending slot's bits are inverted.
class RowLayout {
 public:
  enum class Type : uint8_t { kUint32, kInt32, kInt64, kDouble };
  struct Column {
    Type type;
    bool nullable = false;
    bool descending = false;
  };
  struct Slot {
    uint32_t offset;
    uint32_t stride;
    bool nullable;
    bool descending;
  };

  RowLayout() = default;
  explicit RowLayout(const std::vector<Column>& columns) {
    for (const Column& column : columns) {
      slots_.push_back({stride_, 0, column.nullable, column.descending});
      stride_ += (column.nullable ? 1u : 0u) + ValueSize(column.type);
    }
    for (Slot& slot : slots_) {
      slot.stride = stride_;
    }
  }

  uint32_t stride() const { return stride_; }

  const Slot& slot(uint32_t column) const { return slots_[column]; }

  static uint32_t ValueSize(Type type) {
    switch (type) {
      case Type::kUint32:
      case Type::kInt32:
        return sizeof(uint32_t);
      case Type::kInt64:
      case Type::kDouble:
        return sizeof(uint64_t);
    }
    PERFETTO_FATAL("Unknown type");
  }

  // `get(i, &value)` returns false if row i has no value.
  template <typename T, typename Get>
  PERFETTO_ALWAYS_INLINE static void Write(const Slot& slot,
                                           uint32_t count,
                                           Get get,
                                           uint8_t* rows) {
    uint8_t* to = rows + slot.offset;
    if (slot.nullable) {
      for (uint32_t i = 0; i < count; ++i, to += slot.stride) {
        T value{};
        bool present = get(i, &value);
        auto byte = static_cast<uint8_t>(present ? 0xFF : 0);
        *to = slot.descending ? static_cast<uint8_t>(~byte) : byte;
        if (present) {
          WriteValue(value, slot.descending, to + 1);
        } else {
          memset(to + 1, 0, sizeof(T));
        }
      }
    } else {
      for (uint32_t i = 0; i < count; ++i, to += slot.stride) {
        T value{};
        [[maybe_unused]] bool present = get(i, &value);
        PERFETTO_DCHECK(present);
        WriteValue(value, slot.descending, to);
      }
    }
  }

 private:
  template <typename T>
  PERFETTO_ALWAYS_INLINE static void WriteValue(T x, bool invert, uint8_t* to) {
    auto bits = Encode(x);
    bits = invert ? static_cast<decltype(bits)>(~bits) : bits;
    memcpy(to, &bits, sizeof(bits));
  }

  // The inspiration behind these functions comes from:
  // https://arrow.apache.org/blog/2022/11/07/multi-column-sorts-in-arrow-rust-part-2/
  PERFETTO_ALWAYS_INLINE static uint32_t Encode(uint32_t x) {
    return base::HostToBE32(x);
  }
  PERFETTO_ALWAYS_INLINE static uint32_t Encode(int32_t x) {
    return base::HostToBE32(
        static_cast<uint32_t>(x ^ static_cast<int32_t>(0x80000000)));
  }
  PERFETTO_ALWAYS_INLINE static uint64_t Encode(int64_t x) {
    return base::HostToBE64(
        static_cast<uint64_t>(x ^ static_cast<int64_t>(0x8000000000000000)));
  }
  PERFETTO_ALWAYS_INLINE static uint64_t Encode(double x) {
    int64_t bits;
    memcpy(&bits, &x, sizeof(double));
    bits ^= static_cast<int64_t>(static_cast<uint64_t>(bits >> 63) >> 1);
    return Encode(bits);
  }

  std::vector<Slot> slots_;
  uint32_t stride_ = 0;
};

}  // namespace perfetto::trace_processor::core

#endif  // SRC_TRACE_PROCESSOR_CORE_COMMON_ROW_LAYOUT_H_
