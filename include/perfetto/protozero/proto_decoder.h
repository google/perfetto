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

#include "perfetto/protozero/proto_utils.h"

namespace protozero {

// Reads and decodes protobuf messages from a fixed length buffer. This class
// does not allocate and does no more work than necessary so can be used in
// performance sensitive contexts.
class ProtoDecoder {
 public:
  // The field of a protobuf message. |id| == 0 if the tag is not valid (e.g.
  // because the full tag was unable to be read etc.).
  struct Field {
    struct LengthDelimited {
      const uint8_t* data;
      uint64_t length;
    };

    uint32_t id = 0;
    protozero::proto_utils::FieldType type;
    union {
      uint64_t int_value;
      LengthDelimited length_value;
    };
  };

  // Creates a ProtoDecoder using the given |buffer| with size |length| bytes.
  ProtoDecoder(const uint8_t* buffer, uint64_t length);

  // Reads the next field from the buffer. If the full field cannot be read,
  // the returned struct will have id 0 which is an invalid field id.
  Field ReadField();

  // Returns true if |length_| == |current_position_| - |buffer| and false
  // otherwise.
  bool IsEndOfBuffer();

 private:
  const uint8_t* const buffer_;
  const uint64_t length_;
  const uint8_t* current_position_ = nullptr;
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_PROTO_DECODER_H_
