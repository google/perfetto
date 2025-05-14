/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/art_hprof/art_hprof_parser.h"
#include "src/trace_processor/importers/art_hprof/art_heap_graph_builder.h"
#include "src/trace_processor/importers/common/process_tracker.h"

namespace perfetto::trace_processor::art_hprof {

ArtHprofParser::ArtHprofParser(TraceProcessorContext* ctx) : context_(ctx) {}

ArtHprofParser::~ArtHprofParser() = default;

base::Status ArtHprofParser::Parse(TraceBlobView blob) {
  PERFETTO_DLOG("TBV length: %zu. Size: %zu. Offset: %zu", blob.length(),
                blob.size(), blob.offset());
  reader_.PushBack(std::move(blob));
  byte_iterator_ = std::make_unique<TraceBlobViewIterator>(std::move(reader_));
  if (!parser_) {
    parser_ = std::make_unique<HeapGraphBuilder>(
        std::unique_ptr<ByteIterator>(byte_iterator_.release()));
  }
  parser_->Parse();

  return base::OkStatus();
}

base::Status ArtHprofParser::NotifyEndOfFile() {
  const HeapGraph graph = parser_->BuildGraph();

  UniquePid upid = context_->process_tracker->GetOrCreateProcess(0);

  if (graph.GetClassCount() == 0 || graph.GetObjectCount() == 0) {
    PERFETTO_DLOG("Empty heap graph, skipping parsing");
    return base::OkStatus();
  }

  PERFETTO_DLOG("Processing heap graph: %zu classes, %zu objects",
                graph.GetClassCount(), graph.GetObjectCount());

  // Map from HPROF object IDs to table IDs
  std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id> class_map;
  std::unordered_map<uint64_t, tables::HeapGraphObjectTable::Id> object_map;

  // Process classes first to establish type information
  PopulateClasses(graph, class_map);

  // Process objects next
  PopulateObjects(graph, static_cast<int64_t>(graph.GetTimestamp()), upid,
                  class_map, object_map);

  // Finally process references
  PopulateReferences(graph, object_map);

  return base::OkStatus();
}

// TraceBlobViewIterator implementation
ArtHprofParser::TraceBlobViewIterator::TraceBlobViewIterator(
    util::TraceBlobViewReader&& reader)
    : reader_(std::move(reader)), current_offset_(0) {}

ArtHprofParser::TraceBlobViewIterator::~TraceBlobViewIterator() = default;

bool ArtHprofParser::TraceBlobViewIterator::ReadU1(uint8_t& value) {
  auto slice = reader_.SliceOff(current_offset_, 1);
  if (!slice)
    return false;
  value = *slice->data();
  current_offset_ += 1;
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadU2(uint16_t& value) {
  uint8_t b1, b2;
  if (!ReadU1(b1) || !ReadU1(b2))
    return false;
  value = static_cast<uint16_t>((static_cast<uint16_t>(b1) << 8) |
                                static_cast<uint16_t>(b2));
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadU4(uint32_t& value) {
  uint8_t b1, b2, b3, b4;
  if (!ReadU1(b1) || !ReadU1(b2) || !ReadU1(b3) || !ReadU1(b4))
    return false;
  value = (static_cast<uint32_t>(b1) << 24) |
          (static_cast<uint32_t>(b2) << 16) | (static_cast<uint32_t>(b3) << 8) |
          static_cast<uint32_t>(b4);
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadId(uint64_t& value,
                                                   uint32_t id_size) {
  if (id_size == 4) {
    uint32_t id;
    if (!ReadU4(id))
      return false;
    value = id;
    return true;
  } else if (id_size == 8) {
    uint32_t high, low;
    if (!ReadU4(high) || !ReadU4(low))
      return false;
    value = (static_cast<uint64_t>(high) << 32) | low;
    return true;
  }
  return false;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadString(std::string& str,
                                                       size_t length) {
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice)
    return false;

  str.resize(length);
  std::memcpy(&str[0], slice->data(), length);
  current_offset_ += length;
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::ReadBytes(
    std::vector<uint8_t>& data,
    size_t length) {
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice)
    return false;

  data.resize(length);
  std::memcpy(data.data(), slice->data(), length);
  current_offset_ += length;
  return true;
}

bool ArtHprofParser::TraceBlobViewIterator::SkipBytes(size_t count) {
  auto slice = reader_.SliceOff(current_offset_, count);
  if (!slice)
    return false;

  current_offset_ += count;
  return true;
}

size_t ArtHprofParser::TraceBlobViewIterator::GetPosition() const {
  return current_offset_;
}

bool ArtHprofParser::TraceBlobViewIterator::IsEof() const {
  return !reader_.SliceOff(current_offset_, 1);
}

void ArtHprofParser::PopulateClasses(
    const HeapGraph& graph,
    std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id>& class_map) {
  auto& class_table = *context_->storage->mutable_heap_graph_class_table();
  size_t classes_processed = 0;

  // Process each class from the heap graph
  for (const auto& [class_id, class_def] : graph.GetClasses()) {
    classes_processed++;

    // Intern strings for class metadata
    StringId name_id =
        context_->storage->InternString(base::StringView(class_def.GetName()));
    StringId kind_id =
        context_->storage->InternString(base::StringView(kUnknownClassKind));

    // Create and insert the class row
    tables::HeapGraphClassTable::Row class_row;
    class_row.name = name_id;
    class_row.deobfuscated_name = std::nullopt;
    class_row.location = std::nullopt;
    class_row.superclass_id = std::nullopt;  // Will update in second pass
    class_row.classloader_id = 0;            // Default
    class_row.kind = kind_id;

    tables::HeapGraphClassTable::Id table_id = class_table.Insert(class_row).id;
    class_map[class_id] = table_id;
  }

  // Update superclass relationships
  for (const auto& [class_id, class_def] : graph.GetClasses()) {
    uint64_t super_id = class_def.GetSuperClassId();
    if (super_id != 0) {
      auto current_it = class_map.find(class_id);
      auto super_it = class_map.find(super_id);

      if (current_it != class_map.end() && super_it != class_map.end()) {
        class_table.mutable_superclass_id()->Set(current_it->second.value,
                                                 super_it->second);
      }
    }
  }

  PERFETTO_DLOG("Processed %zu classes", classes_processed);
}

void ArtHprofParser::PopulateObjects(
    const HeapGraph& graph,
    int64_t ts,
    UniquePid upid,
    const std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id>&
        class_map,
    std::unordered_map<uint64_t, tables::HeapGraphObjectTable::Id>&
        object_map) {
  auto& object_table = *context_->storage->mutable_heap_graph_object_table();
  size_t objects_processed = 0;

  // Create fallback unknown class if needed
  tables::HeapGraphClassTable::Id unknown_class_id;

  for (const auto& [obj_id, obj] : graph.GetObjects()) {
    objects_processed++;

    // Resolve object's type
    auto type_it = class_map.find(obj.GetClassId());
    if (type_it == class_map.end() &&
        obj.GetObjectType() != ObjectType::kPrimitiveArray) {
      PERFETTO_FATAL("Unknown class: %" PRIu64 ". Object type: %" PRIu8,
                     obj.GetClassId(),
                     static_cast<uint8_t>(obj.GetObjectType()));
    }

    // Create object row
    tables::HeapGraphObjectTable::Row object_row;
    object_row.upid = upid;
    object_row.graph_sample_ts = ts;
    object_row.self_size = static_cast<int64_t>(obj.GetSize());
    object_row.native_size = obj.GetNativeSize();
    object_row.reference_set_id = std::nullopt;
    object_row.reachable = obj.IsReachable();
    object_row.type_id =
        type_it != class_map.end() ? type_it->second : unknown_class_id;

    // Handle heap type
    StringId heap_type_id =
        context_->storage->InternString(base::StringView(obj.GetHeapType()));
    object_row.heap_type = heap_type_id;

    // Handle root type
    if (obj.IsRoot() && obj.GetRootType().has_value()) {
      // Convert root type enum to string
      std::string root_type_str =
          HeapGraph::GetRootTypeName(obj.GetRootType().value());
      StringId root_type_id = context_->storage->InternString(
          base::StringView(root_type_str.data(), root_type_str.size()));
      object_row.root_type = root_type_id;
    }

    object_row.root_distance = -1;  // Ignored

    // Insert object and store mapping
    tables::HeapGraphObjectTable::Id table_id =
        object_table.Insert(object_row).id;
    object_map[obj_id] = table_id;
  }

  PERFETTO_DLOG("Processed %zu objects", objects_processed);
}

void ArtHprofParser::PopulateReferences(
    const HeapGraph& graph,
    const std::unordered_map<uint64_t, tables::HeapGraphObjectTable::Id>&
        object_map) {
  auto& object_table = *context_->storage->mutable_heap_graph_object_table();
  auto& reference_table =
      *context_->storage->mutable_heap_graph_reference_table();
  auto& class_table = *context_->storage->mutable_heap_graph_class_table();

  // Group references by owner for efficient reference_set_id assignment
  std::unordered_map<uint64_t, std::vector<Reference>> refs_by_owner;
  size_t total_reference_count = 0;

  // Step 1: Collect all references
  PERFETTO_DLOG("Collecting references from objects...");
  for (const auto& [obj_id, obj] : graph.GetObjects()) {
    const auto& refs = obj.GetReferences();
    if (!refs.empty()) {
      refs_by_owner[obj_id].insert(refs_by_owner[obj_id].end(), refs.begin(),
                                   refs.end());
      total_reference_count += refs.size();
    }
  }

  PERFETTO_DLOG("Found %zu total references from %zu objects",
                total_reference_count, refs_by_owner.size());

  // Step 2: Validate we have reference owners in our object map
  size_t missing_owners = 0;
  for (const auto& [owner_id, refs] : refs_by_owner) {
    if (object_map.find(owner_id) == object_map.end()) {
      missing_owners++;
    }
  }

  if (missing_owners > 0) {
    PERFETTO_DLOG("Warning: %zu reference owners are missing from object map",
                  missing_owners);
  }

  // Step 3: Build class map for type resolution
  std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id> class_map;
  for (const auto& [class_id, class_def] : graph.GetClasses()) {
    StringId name_id =
        context_->storage->InternString(base::StringView(class_def.GetName()));

    // Find the class ID in the table
    for (uint32_t i = 0; i < class_table.row_count(); i++) {
      if (class_table.name()[i] == name_id) {
        class_map[class_id] = tables::HeapGraphClassTable::Id(i);
        break;
      }
    }
  }

  // Step 4: Process references and create reference sets
  uint32_t next_reference_set_id = 1;
  size_t valid_refs = 0;
  size_t dangling_refs = 0;

  for (const auto& [owner_id, refs] : refs_by_owner) {
    // Skip if no references
    if (refs.empty()) {
      continue;
    }

    // Get owner's table ID
    auto owner_it = object_map.find(owner_id);
    if (owner_it == object_map.end()) {
      continue;
    }

    // Create reference set for owner
    uint32_t reference_set_id = next_reference_set_id++;
    object_table.mutable_reference_set_id()->Set(owner_it->second.value,
                                                 reference_set_id);

    // Process all references from this owner
    for (const auto& ref : refs) {
      // Get owned object's table ID if it exists
      std::optional<tables::HeapGraphObjectTable::Id> owned_table_id;
      if (ref.target_id != 0) {
        auto owned_it = object_map.find(ref.target_id);
        if (owned_it != object_map.end()) {
          owned_table_id = owned_it->second;
          valid_refs++;
        } else {
          dangling_refs++;
        }
      }

      // Get the field name
      StringId field_name_id =
          context_->storage->InternString(base::StringView(ref.field_name));

      // Resolve field type from class ID
      StringId field_type_id;
      if (ref.field_class_id != 0) {
        auto class_it = class_map.find(ref.field_class_id);
        if (class_it != class_map.end()) {
          // Get class name from class table
          StringId class_name_id = class_table.name()[class_it->second.value];
          field_type_id = class_name_id;
        } else {
          // Class not found, use default
          field_type_id = context_->storage->InternString(
              base::StringView(kJavaLangObject));
        }
      } else {
        // No class ID, use default
        field_type_id =
            context_->storage->InternString(base::StringView(kJavaLangObject));
      }

      // Create reference record
      tables::HeapGraphReferenceTable::Row reference_row;
      reference_row.reference_set_id = reference_set_id;
      reference_row.owner_id = owner_it->second;
      reference_row.owned_id = owned_table_id;
      reference_row.field_name = field_name_id;
      reference_row.field_type_name = field_type_id;

      reference_table.Insert(reference_row);
    }
  }

  // Check for root objects with references (important for flamegraph)
  size_t roots_with_refs = 0;
  size_t roots_without_refs = 0;

  for (uint32_t i = 0; i < object_table.row_count(); i++) {
    if (object_table.root_type()[i].has_value()) {
      if (object_table.reference_set_id()[i].has_value()) {
        roots_with_refs++;
      } else {
        roots_without_refs++;
      }
    }
  }

  // Final statistics and warnings
  PERFETTO_DLOG("Reference processing complete: %zu valid, %zu dangling",
                valid_refs, dangling_refs);

  if (valid_refs == 0) {
    PERFETTO_LOG(
        "WARNING: No valid references found! Flamegraph will not render.");
  } else if (roots_with_refs == 0 && roots_without_refs > 0) {
    PERFETTO_LOG(
        "WARNING: No root objects have references! Flamegraph may not render "
        "properly.");
  }
}
}  // namespace perfetto::trace_processor::art_hprof
