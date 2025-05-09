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

#include "src/trace_processor/importers/art_hprof/art_hprof_parser_impl.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_event.h"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_event.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor::art_hprof {
ArtHprofParserImpl::~ArtHprofParserImpl() = default;
ArtHprofParserImpl::ArtHprofParserImpl(TraceProcessorContext* context)
    : context_(context) {}

void ArtHprofParserImpl::ParseArtHprofEvent(int64_t ts, ArtHprofEvent event) {
  const HeapGraph& graph = event.data;
  uint32_t os_pid = event.pid;

  // Get or create the process for this pid
  UniquePid upid = context_->process_tracker->GetOrCreateProcess(os_pid);

  if (graph.GetClassCount() == 0 || graph.GetObjectCount() == 0) {
    PERFETTO_DLOG("Empty heap graph, skipping parsing");
    return;
  }

  PERFETTO_LOG("Processing heap graph for PID %u: %zu classes, %zu objects",
               os_pid, graph.GetClassCount(), graph.GetObjectCount());

  // Map from HPROF object IDs to table IDs
  std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id> class_map;
  std::unordered_map<uint64_t, tables::HeapGraphObjectTable::Id> object_map;

  // Process classes first to establish type information
  PopulateClasses(graph, class_map);

  // Process objects next
  PopulateObjects(graph, ts, upid, class_map, object_map);

  // Finally process references
  PopulateReferences(graph, object_map);
}

void ArtHprofParserImpl::PopulateClasses(
    const HeapGraph& graph,
    std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id>& class_map) {
  auto& class_table = *context_->storage->mutable_heap_graph_class_table();
  size_t classes_processed = 0;

  // Process each class from the heap graph
  for (const auto& [class_id, class_def] : graph.GetClasses()) {
    classes_processed++;

    // Intern strings for class metadata
    StringId name_id =
        context_->storage->InternString(base::StringView(class_def.name()));
    StringId kind_id = context_->storage->InternString(
        base::StringView(kUnknownClassKind));  // Default kind for Java classes

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
    uint64_t super_id = class_def.super_class_id();
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

void ArtHprofParserImpl::PopulateObjects(
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
    auto type_it = class_map.find(obj.class_id());
    if (type_it == class_map.end() &&
        obj.object_type() != ObjectType::PRIMITIVE_ARRAY) {
      PERFETTO_FATAL("Unknown class: %" PRIu64 ". Object type: %" PRIu8,
                     obj.class_id(), static_cast<uint8_t>(obj.object_type()));
    }

    // Create object row
    tables::HeapGraphObjectTable::Row object_row;
    object_row.upid = upid;
    object_row.graph_sample_ts = ts;
    object_row.self_size = static_cast<int64_t>(obj.GetSize());
    object_row.native_size = 0;
    object_row.reference_set_id = std::nullopt;
    object_row.reachable = 1;
    object_row.type_id =
        type_it != class_map.end() ? type_it->second : unknown_class_id;

    // Handle heap type
    StringId heap_type_id =
        context_->storage->InternString(base::StringView(obj.heap_type()));
    object_row.heap_type = heap_type_id;

    // Handle root type
    if (obj.is_root() && obj.root_type().has_value()) {
      // Convert root type enum to string
      std::string_view root_type_str =
          HeapGraph::GetRootTypeName(obj.root_type().value());
      StringId root_type_id = context_->storage->InternString(
          base::StringView(root_type_str.data(), root_type_str.size()));
      object_row.root_type = root_type_id;
    }

    object_row.root_distance = -1;  // Will be calculated later

    // Insert object and store mapping
    tables::HeapGraphObjectTable::Id table_id =
        object_table.Insert(object_row).id;
    object_map[obj_id] = table_id;
  }

  PERFETTO_DLOG("Processed %zu objects", objects_processed);
}

std::string ArtHprofParserImpl::GetFieldTypeName(FieldType type) {
  switch (type) {
    case FieldType::OBJECT:
      return kJavaLangObject;
    case FieldType::BOOLEAN:
      return "boolean";
    case FieldType::CHAR:
      return "char";
    case FieldType::FLOAT:
      return "float";
    case FieldType::DOUBLE:
      return "double";
    case FieldType::BYTE:
      return "byte";
    case FieldType::SHORT:
      return "short";
    case FieldType::INT:
      return "int";
    case FieldType::LONG:
      return "long";
  }
  return "unknown";
}

void ArtHprofParserImpl::PopulateReferences(
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
    const auto& refs = obj.references();
    if (!refs.empty()) {
      refs_by_owner[obj_id].insert(refs_by_owner[obj_id].end(), refs.begin(),
                                   refs.end());
      total_reference_count += refs.size();
    }
  }

  PERFETTO_LOG("Found %zu total references from %zu objects",
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
        context_->storage->InternString(base::StringView(class_def.name()));

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
  PERFETTO_LOG("Reference processing complete: %zu valid, %zu dangling",
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
