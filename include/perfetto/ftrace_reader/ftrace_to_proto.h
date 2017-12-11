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

#ifndef INCLUDE_PERFETTO_FTRACE_READER_FTRACE_TO_PROTO_H_
#define INCLUDE_PERFETTO_FTRACE_READER_FTRACE_TO_PROTO_H_

#include <stdint.h>

#include <iosfwd>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace perfetto {

struct FtraceEvent {
  struct Field {
    std::string type_and_name;
    uint16_t offset;
    uint16_t size;
    bool is_signed;

    bool operator==(const Field& other) const {
      return std::tie(type_and_name, offset, size, is_signed) ==
             std::tie(other.type_and_name, other.offset, other.size,
                      other.is_signed);
    }
  };

  std::string name;
  int id;
  std::vector<Field> common_fields;
  std::vector<Field> fields;
};

struct Proto {
  struct Field {
    std::string type;
    std::string name;
    uint32_t number;
  };
  std::string name;
  std::vector<Field> fields;

  std::string ToString();
};

bool GenerateProto(const FtraceEvent& format, Proto* proto_out);
std::string InferProtoType(const FtraceEvent::Field& field);
std::string GetNameFromTypeAndName(const std::string& type_and_name);

// Allow gtest to pretty print FtraceEvent::Field.
::std::ostream& operator<<(::std::ostream& os, const FtraceEvent::Field&);
void PrintTo(const FtraceEvent::Field& args, ::std::ostream* os);

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_FTRACE_READER_FTRACE_TO_PROTO_H_
