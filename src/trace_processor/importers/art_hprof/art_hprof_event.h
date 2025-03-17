/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_EVENT_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_EVENT_H_

#include <cstdint>
#include <optional>

#include "src/trace_processor/containers/string_pool.h"

namespace perfetto::trace_processor::art_hprof {
struct HeapGraphValue {
  enum ValueType {
    NONE, BOOLEAN, BYTE, CHAR, SHORT, INT, FLOAT, LONG, DOUBLE, OBJECT_ID, STRING
  };

  ValueType type = NONE;
  union {
    bool bool_value;
    int8_t byte_value;
    char16_t char_value;
    int16_t short_value;
    int32_t int_value;
    float float_value;
    int64_t long_value;
    double double_value;
    uint64_t object_id_value;
  };
  std::string string_value;
};

struct HeapGraphObject {
  uint64_t object_id = 0;
  uint64_t type_id = 0;
  int64_t self_size = 0;
  std::optional<std::string> heap_type;
  std::unordered_map<std::string, HeapGraphValue> field_values;
  std::vector<uint64_t> references;
  std::optional<uint32_t> reference_set_id;
};

struct HeapGraphReference {
  uint64_t owner_id = 0;
  std::optional<uint64_t> owned_id;
  std::string field_name;
  std::string field_type_name;
  uint32_t reference_set_id = 0;
};

struct HeapGraphClass {
  std::string name;
  std::optional<std::string> deobfuscated_name;
  std::optional<std::string> location;
  std::optional<uint64_t> superclass_id;
  std::optional<uint32_t> classloader_id;
  std::string kind;
  uint64_t class_object_id = 0;
};

// Streamlined IR data structures
struct HeapGraphIR {
  std::vector<HeapGraphClass> classes;
  std::vector<HeapGraphObject> objects;
  std::vector<HeapGraphReference> references;
};

// Struct to hold event data from the HPROF parser
struct ArtHprofEvent {
  // Thread ID if applicable
  uint32_t pid = 0;

  // The actual data based on event type
  HeapGraphIR data;

  // Constructors for different event types
  explicit ArtHprofEvent(HeapGraphIR ir): data(std::move(ir)) {}
};

}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_EVENT_H_
