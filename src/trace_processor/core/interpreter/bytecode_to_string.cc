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

#include "src/trace_processor/core/interpreter/bytecode_to_string.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/core/common/sort_types.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"

namespace perfetto::trace_processor::core::interpreter {

namespace {

void AddFormatted(const char* name,
                  const char* label,
                  uint32_t value,
                  std::vector<std::string>* fields) {
  if (strncmp(name, "pad", 3) == 0) {
    return;
  }
  if (label) {
    fields->push_back(
        base::StackString<64>("%s=%s(%u)", name, label, value).ToStdString());
  } else {
    fields->push_back(
        base::StackString<64>("%s=%u", name, value).ToStdString());
  }
}

}  // namespace

void AddField(const char* name, uint32_t v, std::vector<std::string>* f) {
  AddFormatted(name, nullptr, v, f);
}
void AddField(const char* name,
              const HandleBase& v,
              std::vector<std::string>* f) {
  AddFormatted(name, "Register", v.index, f);
}
void AddField(const char* name, NonNullOp v, std::vector<std::string>* f) {
  AddFormatted(name, "NonNullOp", v.index(), f);
}
void AddField(const char* name,
              FilterValueHandle v,
              std::vector<std::string>* f) {
  AddFormatted(name, "FilterValue", v.index, f);
}
void AddField(const char* name, BoundModifier v, std::vector<std::string>* f) {
  AddFormatted(name, "BoundModifier", v.index(), f);
}
void AddField(const char* name, SortDirection v, std::vector<std::string>* f) {
  AddFormatted(name, "SortDirection", static_cast<uint32_t>(v), f);
}
void AddField(const char* name, NullsLocation v, std::vector<std::string>* f) {
  AddFormatted(name, "NullsLocation", v.index(), f);
}

std::string BytecodeFieldsFormat(const std::vector<std::string>& fields) {
  std::string res;
  res.append("[");
  res.append(base::Join(fields, ", "));
  res.append("]");
  return res;
}

std::string ToString(const Bytecode& op) {
  // One string rather than an array of pointers, which would need relocations.
#define PERFETTO_DATAFRAME_BYTECODE_NAME(...) #__VA_ARGS__ "\0"
  static constexpr char kNames[] =
      PERFETTO_DATAFRAME_BYTECODE_LIST(PERFETTO_DATAFRAME_BYTECODE_NAME);
#undef PERFETTO_DATAFRAME_BYTECODE_NAME
#define PERFETTO_DATAFRAME_BYTECODE_CASE_TO_STRING(...)      \
  case base::variant_index<BytecodeVariant, __VA_ARGS__>():  \
    fields = static_cast<const __VA_ARGS__&>(op).ToString(); \
    break;
  std::string fields;
  switch (op.option) {
    PERFETTO_DATAFRAME_BYTECODE_LIST(PERFETTO_DATAFRAME_BYTECODE_CASE_TO_STRING)
    default:
      PERFETTO_FATAL("Unknown opcode %u", op.option);
  }
#undef PERFETTO_DATAFRAME_BYTECODE_CASE_TO_STRING
  const char* name = kNames;
  for (uint32_t i = 0; i < op.option; ++i) {
    name += strlen(name) + 1;
  }
  return std::string(name) + ": " + fields;
}

}  // namespace perfetto::trace_processor::core::interpreter
