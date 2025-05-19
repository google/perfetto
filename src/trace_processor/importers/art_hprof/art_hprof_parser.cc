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
  reader_.PushBack(std::move(blob));
  byte_iterator_ = std::make_unique<TraceBlobViewIterator>(std::move(reader_));
  if (!parser_) {
    parser_ = std::make_unique<HeapGraphBuilder>(
        std::unique_ptr<ByteIterator>(byte_iterator_.release()), context_);
  }
  parser_->Parse();

  return base::OkStatus();
}

base::Status ArtHprofParser::NotifyEndOfFile() {
  const HeapGraph graph = parser_->BuildGraph();

  UniquePid upid = context_->process_tracker->GetOrCreateProcess(0);

  if (graph.GetClassCount() == 0 || graph.GetObjectCount() == 0) {
    return base::OkStatus();
  }

  // Map from HPROF object IDs to table IDs
  base::FlatHashMap<uint64_t, tables::HeapGraphClassTable::Id> class_map;
  base::FlatHashMap<uint64_t, tables::HeapGraphObjectTable::Id> object_map;

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
    base::FlatHashMap<uint64_t, tables::HeapGraphClassTable::Id>& class_map) {
  auto& class_table = *context_->storage->mutable_heap_graph_class_table();
  // Process each class from the heap graph
  for (auto it = graph.GetClasses().GetIterator(); it; ++it) {
    auto class_id = it.key();
    auto& class_def = it.value();

    // Intern strings for class metadata
    StringId name_id = context_->storage->InternString(class_def.GetName());
    StringId kind_id = context_->storage->InternString(kUnknownClassKind);

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
  for (auto it = graph.GetClasses().GetIterator(); it; ++it) {
    auto class_id = it.key();
    auto& class_def = it.value();
    uint64_t super_id = class_def.GetSuperClassId();
    if (super_id != 0) {
      auto current = class_map.Find(class_id);
      auto super = class_map.Find(super_id);

      if (current && super) {
        class_table.mutable_superclass_id()->Set(current->value, *super);
      }
    }
  }
}

void ArtHprofParser::PopulateObjects(
    const HeapGraph& graph,
    int64_t ts,
    UniquePid upid,
    const base::FlatHashMap<uint64_t, tables::HeapGraphClassTable::Id>&
        class_map,
    base::FlatHashMap<uint64_t, tables::HeapGraphObjectTable::Id>& object_map) {
  auto& object_table = *context_->storage->mutable_heap_graph_object_table();

  // Create fallback unknown class if needed
  tables::HeapGraphClassTable::Id unknown_class_id;

  for (auto it = graph.GetObjects().GetIterator(); it; ++it) {
    auto obj_id = it.key();
    auto& obj = it.value();

    // Resolve object's type
    auto type = class_map.Find(obj.GetClassId());
    if (!type && obj.GetObjectType() != ObjectType::kPrimitiveArray) {
      context_->storage->IncrementStats(stats::hprof_class_errors);
      continue;
    }

    // Create object row
    tables::HeapGraphObjectTable::Row object_row;
    object_row.upid = upid;
    object_row.graph_sample_ts = ts;
    object_row.self_size = static_cast<int64_t>(obj.GetSize());
    object_row.native_size = obj.GetNativeSize();
    object_row.reference_set_id = std::nullopt;
    object_row.reachable = obj.IsReachable();
    object_row.type_id = type ? *type : unknown_class_id;

    // Handle heap type
    StringId heap_type_id = context_->storage->InternString(obj.GetHeapType());
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
}

void ArtHprofParser::PopulateReferences(
    const HeapGraph& graph,
    const base::FlatHashMap<uint64_t, tables::HeapGraphObjectTable::Id>&
        object_map) {
  auto& object_table = *context_->storage->mutable_heap_graph_object_table();
  auto& reference_table =
      *context_->storage->mutable_heap_graph_reference_table();
  auto& class_table = *context_->storage->mutable_heap_graph_class_table();

  // Group references by owner for efficient reference_set_id assignment
  base::FlatHashMap<uint64_t, std::vector<Reference>> refs_by_owner;

  // Step 1: Collect all references
  for (auto it = graph.GetObjects().GetIterator(); it; ++it) {
    auto obj_id = it.key();
    auto& obj = it.value();

    const auto& refs = obj.GetReferences();
    if (!refs.empty()) {
      refs_by_owner[obj_id].insert(refs_by_owner[obj_id].end(), refs.begin(),
                                   refs.end());
    }
  }

  // Step 2: Validate we have reference owners in our object map
  size_t missing_owners = 0;
  for (auto it = refs_by_owner.GetIterator(); it; ++it) {
    auto owner_id = it.key();
    if (!object_map.Find(owner_id)) {
      missing_owners++;
    }
  }

  if (missing_owners > 0) {
    context_->storage->IncrementStats(stats::hprof_reference_errors);
  }

  // Step 3: Build class map for type resolution
  base::FlatHashMap<uint64_t, tables::HeapGraphClassTable::Id> class_map;
  for (auto it = graph.GetClasses().GetIterator(); it; ++it) {
    auto class_id = it.key();
    auto& class_def = it.value();
    StringId name_id = context_->storage->InternString(class_def.GetName());

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

  for (auto it = refs_by_owner.GetIterator(); it; ++it) {
    auto owner_id = it.key();
    auto& refs = it.value();
    // Skip if no references
    if (refs.empty()) {
      continue;
    }

    // Get owner's table ID
    auto owner = object_map.Find(owner_id);
    if (!owner) {
      continue;
    }

    // Create reference set for owner
    uint32_t reference_set_id = next_reference_set_id++;
    object_table.mutable_reference_set_id()->Set(owner->value,
                                                 reference_set_id);

    // Process all references from this owner
    for (const auto& ref : refs) {
      // Get owned object's table ID if it exists
      std::optional<tables::HeapGraphObjectTable::Id> owned_table_id;
      if (ref.target_id != 0) {
        auto owned = object_map.Find(ref.target_id);
        if (owned) {
          owned_table_id = *owned;
        } else {
          context_->storage->IncrementStats(stats::hprof_reference_errors);
        }
      }

      // Get the field name
      StringId field_name_id = context_->storage->InternString(ref.field_name);

      // Resolve field type from class ID
      StringId field_type_id;
      auto cls = class_map.Find(*ref.field_class_id);
      if (cls) {
        // Get class name from class table
        StringId class_name_id = class_table.name()[cls->value];
        field_type_id = class_name_id;
      } else {
        context_->storage->IncrementStats(stats::hprof_class_errors);
        continue;
      }

      // Create reference record
      tables::HeapGraphReferenceTable::Row reference_row;
      reference_row.reference_set_id = reference_set_id;
      reference_row.owner_id = *owner;
      reference_row.owned_id = owned_table_id;
      reference_row.field_name = field_name_id;
      reference_row.field_type_name = field_type_id;

      reference_table.Insert(reference_row);
    }
  }
}
}  // namespace perfetto::trace_processor::art_hprof
