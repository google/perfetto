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

#include "perfetto/ftrace_reader/format_parser.h"

#include <string.h>

#include <iosfwd>
#include <iostream>
#include <memory>
#include <vector>

#include "perfetto/base/utils.h"
#include "perfetto/ftrace_reader/ftrace_to_proto.h"

namespace perfetto {
namespace {

#define MAX_FIELD_LENGTH 127
#define STRINGIFY(x) STRINGIFY2(x)
#define STRINGIFY2(x) #x

const char* kCommonFieldPrefix = "common_";

bool IsCommonFieldName(std::string name) {
  return name.compare(0, strlen(kCommonFieldPrefix), kCommonFieldPrefix) == 0;
}

}  // namespace

bool ParseFtraceEvent(const std::string& input, FtraceEvent* output) {
  std::unique_ptr<char[], base::FreeDeleter> input_copy(strdup(input.c_str()));
  char* s = input_copy.get();

  char buffer[MAX_FIELD_LENGTH + 1];

  bool has_id = false;
  bool has_name = false;

  int id = 0;
  std::string name;
  std::vector<FtraceEvent::Field> common_fields;
  std::vector<FtraceEvent::Field> fields;

  for (char* line = strtok(s, "\n"); line; line = strtok(nullptr, "\n")) {
    if (!has_id && sscanf(line, "ID: %d", &id) == 1) {
      has_id = true;
      continue;
    }

    if (!has_name &&
        sscanf(line, "name: %" STRINGIFY(MAX_FIELD_LENGTH) "s", buffer) == 1) {
      name = std::string(buffer);
      has_name = true;
      continue;
    }

    if (strcmp("format:", line) == 0) {
      continue;
    }

    uint16_t offset = 0;
    uint16_t size = 0;
    int is_signed = 0;
    if (sscanf(line,
               "\tfield:%" STRINGIFY(MAX_FIELD_LENGTH) "[^;];\toffset: "
                                                       "%hu;\tsize: "
                                                       "%hu;\tsigned: %d;",
               buffer, &offset, &size, &is_signed) == 4) {
      std::string type_and_name(buffer);

      FtraceEvent::Field field{type_and_name, offset, size, is_signed == 1};

      if (IsCommonFieldName(GetNameFromTypeAndName(type_and_name))) {
        common_fields.push_back(field);
      } else {
        fields.push_back(field);
      }

      continue;
    }

    if (strncmp(line, "print fmt:", 10) == 0) {
      break;
    }

    if (output)
      fprintf(stderr, "Cannot parse line: \"%s\"\n", line);
    return false;
  }

  if (!has_id || !has_name || fields.size() == 0) {
    if (output)
      fprintf(stderr, "Could not parse format file: %s.\n",
              !has_id ? "no ID found"
                      : !has_name ? "no name found" : "no fields found");
    return false;
  }

  if (!output)
    return true;

  output->id = id;
  output->name = name;
  output->fields = std::move(fields);
  output->common_fields = std::move(common_fields);

  return true;
}

::std::ostream& operator<<(::std::ostream& os,
                           const FtraceEvent::Field& field) {
  PrintTo(field, &os);
  return os;
}

// Allow gtest to pretty print FtraceEvent::Field.
void PrintTo(const FtraceEvent::Field& field, ::std::ostream* os) {
  *os << "FtraceEvent::Field(" << field.type_and_name << ", " << field.offset
      << ", " << field.size << ", " << field.is_signed << ")";
}

}  // namespace perfetto
