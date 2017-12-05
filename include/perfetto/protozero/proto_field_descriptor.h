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

#ifndef INCLUDE_PERFETTO_PROTOZERO_PROTO_FIELD_DESCRIPTOR_H_
#define INCLUDE_PERFETTO_PROTOZERO_PROTO_FIELD_DESCRIPTOR_H_

#include <stdint.h>

namespace protozero {

// Used for minimal reflection support in auto-generated .pbzero.h files.
class ProtoFieldDescriptor {
 public:
  enum Type {
    TYPE_INVALID = 0,
    TYPE_DOUBLE = 1,
    TYPE_FLOAT = 2,
    TYPE_INT64 = 3,
    TYPE_UINT64 = 4,
    TYPE_INT32 = 5,
    TYPE_FIXED64 = 6,
    TYPE_FIXED32 = 7,
    TYPE_BOOL = 8,
    TYPE_STRING = 9,
    TYPE_MESSAGE = 11,
    // TYPE_GROUP = 10 is not supported.
    TYPE_BYTES = 12,
    TYPE_UINT32 = 13,
    TYPE_ENUM = 14,
    TYPE_SFIXED32 = 15,
    TYPE_SFIXED64 = 16,
    TYPE_SINT32 = 17,
    TYPE_SINT64 = 18,
  };

  ProtoFieldDescriptor(const char* name,
                       Type type,
                       uint32_t number,
                       bool is_repeated)
      : name_(name), type_(type), number_(number), is_repeated_(is_repeated) {}

  const char* name() const { return name_; }
  Type type() const { return type_; }
  uint32_t number() const { return number_; }
  bool is_repeated() const { return is_repeated_; }
  bool is_valid() const { return type_ != Type::TYPE_INVALID; }

 private:
  const char* const name_;
  const Type type_;
  const uint32_t number_;
  const bool is_repeated_;
};

}  // namespace protozero

#endif  // INCLUDE_PERFETTO_PROTOZERO_PROTO_FIELD_DESCRIPTOR_H_
