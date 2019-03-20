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

#ifndef INCLUDE_PERFETTO_PROTOZERO_FIELD_H_
#define INCLUDE_PERFETTO_PROTOZERO_FIELD_H_

#include <stdint.h>

#include "perfetto/base/logging.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/protozero/proto_utils.h"

namespace protozero {

struct ConstBytes {
  const uint8_t* data;
  size_t size;
};

struct ConstChars {
  // Allow implicit conversion to perfetto's base::StringView without depending
  // on perfetto/base or viceversa.
  static constexpr bool kConvertibleToStringView = true;

  const char* data;
  size_t size;
};

// A protobuf field decoded by the protozero proto decoders. It exposes
// convenience accessors with minimal debug checks.
// This class is used both by the iterator-based ProtoDecoder and by the
// one-shot TypedProtoDecoder.
// If the field is not valid the accessors consistently return zero-integers or
// null strings.
class Field {
 public:
  inline bool valid() const { return id_ != 0; }
  inline uint16_t id() const { return id_; }
  explicit inline operator bool() const { return valid(); }

  inline proto_utils::ProtoWireType type() const {
    auto res = static_cast<proto_utils::ProtoWireType>(type_);
    PERFETTO_DCHECK(res == proto_utils::ProtoWireType::kVarInt ||
                    res == proto_utils::ProtoWireType::kLengthDelimited ||
                    res == proto_utils::ProtoWireType::kFixed32 ||
                    res == proto_utils::ProtoWireType::kFixed64);
    return res;
  }

  inline bool as_bool() const {
    PERFETTO_DCHECK(!valid() || type() == proto_utils::ProtoWireType::kVarInt);
    return static_cast<bool>(int_value_);
  }

  inline uint32_t as_uint32() const {
    PERFETTO_DCHECK(!valid() || type() == proto_utils::ProtoWireType::kVarInt ||
                    type() == proto_utils::ProtoWireType::kFixed32);
    return static_cast<uint32_t>(int_value_);
  }

  inline int32_t as_int32() const {
    PERFETTO_DCHECK(!valid() || type() == proto_utils::ProtoWireType::kVarInt ||
                    type() == proto_utils::ProtoWireType::kFixed32);
    return static_cast<int32_t>(int_value_);
  }

  inline uint64_t as_uint64() const {
    PERFETTO_DCHECK(!valid() || type() == proto_utils::ProtoWireType::kVarInt ||
                    type() == proto_utils::ProtoWireType::kFixed32 ||
                    type() == proto_utils::ProtoWireType::kFixed64);
    return int_value_;
  }

  inline int64_t as_int64() const {
    PERFETTO_DCHECK(!valid() || type() == proto_utils::ProtoWireType::kVarInt ||
                    type() == proto_utils::ProtoWireType::kFixed32 ||
                    type() == proto_utils::ProtoWireType::kFixed64);
    return static_cast<int64_t>(int_value_);
  }

  inline float as_float() const {
    PERFETTO_DCHECK(!valid() || type() == proto_utils::ProtoWireType::kFixed32);
    float res;
    uint32_t value32 = static_cast<uint32_t>(int_value_);
    memcpy(&res, &value32, sizeof(res));
    return res;
  }

  inline double as_double() const {
    PERFETTO_DCHECK(!valid() || type() == proto_utils::ProtoWireType::kFixed64);
    double res;
    memcpy(&res, &int_value_, sizeof(res));
    return res;
  }

  inline ConstChars as_string() const {
    PERFETTO_DCHECK(!valid() ||
                    type() == proto_utils::ProtoWireType::kLengthDelimited);
    return ConstChars{reinterpret_cast<const char*>(data()), size_};
  }

  inline ConstBytes as_bytes() const {
    PERFETTO_DCHECK(!valid() ||
                    type() == proto_utils::ProtoWireType::kLengthDelimited);
    return ConstBytes{data(), size_};
  }

  inline const uint8_t* data() const {
    PERFETTO_DCHECK(!valid() ||
                    type() == proto_utils::ProtoWireType::kLengthDelimited);
    return reinterpret_cast<const uint8_t*>(int_value_);
  }

  inline size_t size() const {
    PERFETTO_DCHECK(!valid() ||
                    type() == proto_utils::ProtoWireType::kLengthDelimited);
    return size_;
  }

  inline uint64_t raw_int_value() const { return int_value_; }

  inline void initialize(uint16_t id,
                         uint8_t type,
                         uint64_t int_value,
                         uint32_t size) {
    id_ = id;
    type_ = type;
    int_value_ = int_value;
    size_ = size;
  }

 private:
  // Fields are deliberately not initialized to keep the class trivially
  // constructible. It makes a large perf difference for ProtoDecoder.

  uint64_t int_value_;  // In kLengthDelimited this contains the data() addr.
  uint32_t size_;       // Only valid when when type == kLengthDelimited.
  uint16_t id_;         // Proto field ordinal.
  uint8_t type_;        // proto_utils::ProtoWireType.
};

// The Field struct is used in a lot of perf-sensitive contexts.
static_assert(sizeof(Field) == 16, "Field struct too big");

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_FIELD_H_
