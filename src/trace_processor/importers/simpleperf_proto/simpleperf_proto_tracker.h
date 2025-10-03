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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_SIMPLEPERF_PROTO_SIMPLEPERF_PROTO_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_SIMPLEPERF_PROTO_SIMPLEPERF_PROTO_TRACKER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/trace_processor/importers/common/virtual_memory_mapping.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor::simpleperf_proto_importer {

// Tracker for simpleperf metadata that needs to be shared between tokenizer
// and parser. Lives in the tokenizer and is passed to the parser via events.
class SimpleperfProtoTracker {
 public:
  SimpleperfProtoTracker() = default;
  ~SimpleperfProtoTracker() = default;

  // Store symbol table for a file
  void AddSymbolTable(uint32_t file_id, std::vector<StringId> symbols) {
    symbol_tables_[file_id] = std::move(symbols);
  }

  // Store mapping for a file
  void AddFileMapping(uint32_t file_id, DummyMemoryMapping* mapping) {
    file_mappings_[file_id] = mapping;
  }

  // Store event types
  void AddEventType(StringId event_type) { event_types_.push_back(event_type); }

  // Lookup symbol by file_id and symbol_id
  std::optional<StringId> GetSymbol(uint32_t file_id, int32_t symbol_id) const {
    if (symbol_id < 0) {
      return std::nullopt;
    }
    auto it = symbol_tables_.find(file_id);
    if (it == symbol_tables_.end()) {
      return std::nullopt;
    }
    const auto& symbols = it->second;
    if (static_cast<size_t>(symbol_id) >= symbols.size()) {
      return std::nullopt;
    }
    return symbols[static_cast<size_t>(symbol_id)];
  }

  // Lookup mapping by file_id
  DummyMemoryMapping* GetMapping(uint32_t file_id) const {
    auto it = file_mappings_.find(file_id);
    if (it == file_mappings_.end()) {
      return nullptr;
    }
    return it->second;
  }

  // Lookup event type by event_type_id
  std::optional<StringId> GetEventType(uint32_t event_type_id) const {
    if (event_type_id >= event_types_.size()) {
      return std::nullopt;
    }
    return event_types_[event_type_id];
  }

 private:
  // Map from file_id to symbol table (list of symbol names)
  std::unordered_map<uint32_t, std::vector<StringId>> symbol_tables_;

  // Map from file_id to DummyMemoryMapping pointer
  std::unordered_map<uint32_t, DummyMemoryMapping*> file_mappings_;

  // List of event types indexed by event_type_id
  std::vector<StringId> event_types_;
};

}  // namespace perfetto::trace_processor::simpleperf_proto_importer

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_SIMPLEPERF_PROTO_SIMPLEPERF_PROTO_TRACKER_H_
