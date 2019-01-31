/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PROTOZERO_PROTO_DECODER_H_
#define INCLUDE_PERFETTO_PROTOZERO_PROTO_DECODER_H_

#include <stdint.h>
#include <memory>

#include "perfetto/base/logging.h"
#include "perfetto/base/string_view.h"
#include "perfetto/protozero/proto_utils.h"

namespace protozero {

// Reads and decodes protobuf messages from a fixed length buffer. This class
// does not allocate and does no more work than necessary so can be used in
// performance sensitive contexts.
class ProtoDecoder {
 public:
  using StringView = ::perfetto::base::StringView;

  // The field of a protobuf message. |id| == 0 if the tag is not valid (e.g.
  // because the full tag was unable to be read etc.).
  struct Field {
    struct LengthDelimited {
      const uint8_t* data;
      size_t length;
    };

    uint32_t id = 0;
    proto_utils::ProtoWireType type;
    union {
      uint64_t int_value;
      LengthDelimited length_limited;
    };

    inline bool as_bool() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kVarInt);
      return static_cast<bool>(int_value);
    }

    inline uint32_t as_uint32() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kVarInt ||
                      type == proto_utils::ProtoWireType::kFixed32);
      return static_cast<uint32_t>(int_value);
    }

    inline int32_t as_int32() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kVarInt ||
                      type == proto_utils::ProtoWireType::kFixed32);
      return static_cast<int32_t>(int_value);
    }

    inline uint64_t as_uint64() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kVarInt ||
                      type == proto_utils::ProtoWireType::kFixed64);
      return int_value;
    }

    inline int64_t as_int64() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kVarInt ||
                      type == proto_utils::ProtoWireType::kFixed64);
      return static_cast<int64_t>(int_value);
    }

    // A relaxed version for when we are storing any int as an int64
    // in the raw events table.
    inline int64_t as_integer() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kVarInt ||
                      type == proto_utils::ProtoWireType::kFixed64 ||
                      type == proto_utils::ProtoWireType::kFixed32);
      return static_cast<int64_t>(int_value);
    }

    inline float as_float() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kFixed32);
      float res;
      uint32_t value32 = static_cast<uint32_t>(int_value);
      memcpy(&res, &value32, sizeof(res));
      return res;
    }

    inline double as_double() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kFixed64);
      double res;
      memcpy(&res, &int_value, sizeof(res));
      return res;
    }

    // A relaxed version for when we are storing floats and doubles
    // as real in the raw events table.
    inline double as_real() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kFixed64 ||
                      type == proto_utils::ProtoWireType::kFixed32);
      double res;
      uint64_t value64 = static_cast<uint64_t>(int_value);
      memcpy(&res, &value64, sizeof(res));
      return res;
    }

    inline StringView as_string() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kLengthDelimited);
      return StringView(reinterpret_cast<const char*>(length_limited.data),
                        length_limited.length);
    }

    inline const uint8_t* data() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kLengthDelimited);
      return length_limited.data;
    }

    inline size_t size() const {
      PERFETTO_DCHECK(type == proto_utils::ProtoWireType::kLengthDelimited);
      return static_cast<size_t>(length_limited.length);
    }
  };

  // Creates a ProtoDecoder using the given |buffer| with size |length| bytes.
  inline ProtoDecoder(const uint8_t* buffer, uint64_t length)
      : buffer_(buffer), length_(length), current_position_(buffer) {}

  // Reads the next field from the buffer. If the full field cannot be read,
  // the returned struct will have id 0 which is an invalid field id.
  Field ReadField();

  template <int field_id>
  inline bool FindIntField(uint64_t* field_value) {
    bool res = false;
    for (auto f = ReadField(); f.id != 0; f = ReadField()) {
      if (f.id == field_id) {
        *field_value = f.int_value;
        res = true;
        break;
      }
    }
    Reset();
    return res;
  }

  template <int field_id>
  inline bool FindStringField(StringView* field_value) {
    bool res = false;
    for (auto f = ReadField(); f.id != 0; f = ReadField()) {
      if (f.id == field_id) {
        *field_value = f.as_string();
        res = true;
        break;
      }
    }
    Reset();
    return res;
  }

  // Returns true if |length_| == |current_position_| - |buffer| and false
  // otherwise.
  inline bool IsEndOfBuffer() {
    PERFETTO_DCHECK(current_position_ >= buffer_);
    return length_ == static_cast<uint64_t>(current_position_ - buffer_);
  }

  // Resets the current position to the start of the buffer.
  inline void Reset() { current_position_ = buffer_; }

  // Resets to the given position (must be within the buffer).
  inline void Reset(const uint8_t* pos) {
    PERFETTO_DCHECK(pos >= buffer_ && pos < buffer_ + length_);
    current_position_ = pos;
  }

  // Return's offset inside the buffer.
  inline uint64_t offset() const {
    return static_cast<uint64_t>(current_position_ - buffer_);
  }

  inline const uint8_t* buffer() const { return buffer_; }
  inline uint64_t length() const { return length_; }

 private:
  const uint8_t* const buffer_;
  const uint64_t length_;  // The outer buffer can be larger than 4GB.
  const uint8_t* current_position_ = nullptr;
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_PROTO_DECODER_H_
