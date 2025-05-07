/*
 * Copyright (C) 2025 The Android Open Source Project
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
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace perfetto::trace_processor::art_hprof {

/**
 * Represents a field value in the heap graph with various possible types.
 */
struct HeapGraphValue {
  enum class Type {
    NONE,
    BOOLEAN,
    BYTE,
    CHAR,
    SHORT,
    INT,
    FLOAT,
    LONG,
    DOUBLE,
    OBJECT_ID,
    STRING
  };

  Type type = Type::NONE;

  // Use variant instead of union for type safety and better compatibility
  std::variant<std::monostate,  // for NONE
               bool,            // for BOOLEAN
               int8_t,          // for BYTE
               char16_t,        // for CHAR
               int16_t,         // for SHORT
               int32_t,         // for INT
               float,           // for FLOAT
               int64_t,         // for LONG
               double,          // for DOUBLE
               uint64_t         // for OBJECT_ID
               >
      primitive_value;

  // String values remain separate since they can't be part of a variant with
  // primitive types
  std::string string_value;

  // Default constructor
  HeapGraphValue() : type(Type::NONE), primitive_value(std::monostate{}) {}
};

/**
 * Represents an object instance in the heap graph.
 */
struct HeapGraphObject {
  uint64_t object_id = 0;
  uint64_t type_id = 0;
  int64_t self_size = 0;
  int32_t root_distance;
  std::optional<std::string> heap_type;
  std::unordered_map<std::string, HeapGraphValue> field_values;
  std::vector<uint64_t> references;
  std::optional<uint32_t> reference_set_id;
  std::optional<std::string> root_type;
  std::optional<bool> reachable;
};

/**
 * Represents a reference between objects in the heap graph.
 */
struct HeapGraphReference {
  uint64_t owner_id = 0;
  std::optional<uint64_t> owned_id;
  std::string field_name;
  std::string field_type_name;
  uint32_t reference_set_id = 0;
};

/**
 * Represents a class definition in the heap graph.
 */
struct HeapGraphClass {
  std::string name;
  std::optional<std::string> deobfuscated_name;
  std::optional<std::string> location;
  std::optional<uint64_t> superclass_id;
  std::optional<uint32_t> classloader_id;
  std::string kind;
  uint64_t class_object_id = 0;
};

/**
 * Intermediate representation for heap graph data.
 */
struct HeapGraph {
  std::vector<HeapGraphClass> classes;
  std::vector<HeapGraphObject> objects;
  std::vector<HeapGraphReference> references;
};

/**
 * Event structure for HPROF data from the Android Runtime.
 */
struct ArtHprofEvent {
  // Process ID if applicable
  uint32_t pid = 0;

  // The parsed heap graph data
  HeapGraph data;

  // Constructor
  explicit ArtHprofEvent(HeapGraph ir) : data(std::move(ir)) {}
};

}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_EVENT_H_
