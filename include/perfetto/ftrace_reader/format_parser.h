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

#ifndef INCLUDE_PERFETTO_FTRACE_READER_FORMAT_PARSER_H_
#define INCLUDE_PERFETTO_FTRACE_READER_FORMAT_PARSER_H_

#include <stdint.h>
#include <string>

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
  uint32_t id;
  std::vector<Field> common_fields;
  std::vector<Field> fields;
};

std::string GetNameFromTypeAndName(const std::string& type_and_name);

// Allow gtest to pretty print FtraceEvent::Field.
::std::ostream& operator<<(::std::ostream& os, const FtraceEvent::Field&);
void PrintTo(const FtraceEvent::Field& args, ::std::ostream* os);

bool ParseFtraceEvent(const std::string& input, FtraceEvent* output = nullptr);

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_FTRACE_READER_FORMAT_PARSER_H_
