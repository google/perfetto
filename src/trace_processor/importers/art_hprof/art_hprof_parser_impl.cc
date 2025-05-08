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

  PERFETTO_DLOG("Processing heap graph with %zu classes and %zu objects",
                graph.GetClassCount(), graph.GetObjectCount());

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
    StringId name_id = context_->storage->InternString(
        base::StringView(class_def.name()));
    StringId kind_id = context_->storage->InternString(
        base::StringView("runtime class"));  // Default kind for Java classes

    // Create and insert the class row
    tables::HeapGraphClassTable::Row class_row;
    class_row.name = name_id;
    class_row.deobfuscated_name = std::nullopt;
    class_row.location = std::nullopt;
    class_row.superclass_id = std::nullopt;  // Will update in second pass
    class_row.classloader_id = 0;  // Default
    class_row.kind = kind_id;

    tables::HeapGraphClassTable::Id table_id = class_table.Insert(class_row).id;
    class_map[class_id] = table_id;

    // Log sampling
    if (classes_processed <= 10 || classes_processed % 1000 == 0) {
      PERFETTO_DLOG("Inserted class %zu: ID=%" PRIu64 ", name=%s, table_id=%u",
                    classes_processed, class_id,
                    class_def.name().c_str(), table_id.value);
    }
  }

  // Update superclass relationships
  for (const auto& [class_id, class_def] : graph.GetClasses()) {
    uint64_t super_id = class_def.super_class_id();
    if (super_id != 0) {
      auto current_it = class_map.find(class_id);
      auto super_it = class_map.find(super_id);

      if (current_it != class_map.end() && super_it != class_map.end()) {
        class_table.mutable_superclass_id()->Set(
            current_it->second.value, super_it->second);
      }
    }
  }

  PERFETTO_DLOG("Processed %zu classes", classes_processed);
}

void ArtHprofParserImpl::PopulateObjects(
    const HeapGraph& graph,
    int64_t ts,
    UniquePid upid,
    const std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id>& class_map,
    std::unordered_map<uint64_t, tables::HeapGraphObjectTable::Id>& object_map) {

  auto& object_table = *context_->storage->mutable_heap_graph_object_table();
  size_t objects_processed = 0;

  // Create fallback unknown class if needed
  tables::HeapGraphClassTable::Id unknown_class_id;
  bool created_unknown_class = false;

  for (const auto& [obj_id, obj] : graph.GetObjects()) {
    objects_processed++;

    // Resolve object's type
    auto type_it = class_map.find(obj.class_id());
    if (type_it == class_map.end()) {
      if (!created_unknown_class) {
        // Create fallback unknown class
        auto& class_table = *context_->storage->mutable_heap_graph_class_table();
        StringId unknown_name = context_->storage->InternString(
            base::StringView("unknown"));
        StringId unknown_kind = context_->storage->InternString(
            base::StringView("unknown"));

        tables::HeapGraphClassTable::Row unknown_row;
        unknown_row.name = unknown_name;
        unknown_row.kind = unknown_kind;
        unknown_class_id = class_table.Insert(unknown_row).id;
        created_unknown_class = true;
      }
    }

    // Create object row
    tables::HeapGraphObjectTable::Row object_row;
    object_row.upid = upid;
    object_row.graph_sample_ts = ts;
    object_row.self_size = static_cast<int64_t>(obj.GetSize());
    object_row.native_size = 0;
    object_row.reference_set_id = std::nullopt;  // Will be set during reference processing
    object_row.reachable = 1;
    object_row.type_id = type_it != class_map.end() ? type_it->second : unknown_class_id;

    // Handle heap type
    if (obj.heap_type() != HeapType::HEAP_TYPE_DEFAULT) {
      StringId heap_type_id = context_->storage->InternString(
          base::StringView(HeapGraph::GetHeapType(obj.heap_type())));
      object_row.heap_type = heap_type_id;
    }

    // Handle root type - FIXED: Check if it's a root and properly set the root_type
    if (obj.is_root() && obj.root_type().has_value()) {
      // Convert root type enum to string
      std::string root_type_str = HeapGraph::GetRootType(obj.root_type().value());
      StringId root_type_id = context_->storage->InternString(
          base::StringView(root_type_str));
      object_row.root_type = root_type_id;

      // Log root types for debugging
      PERFETTO_DLOG("Setting root type for object ID=%" PRIu64 ": %s",
                    obj_id, root_type_str.c_str());
    }

    object_row.root_distance = -1;  // Will be calculated later

    // Insert object and store mapping
    tables::HeapGraphObjectTable::Id table_id = object_table.Insert(object_row).id;
    object_map[obj_id] = table_id;

    if (objects_processed <= 10 || objects_processed % 10000 == 0) {
      PERFETTO_DLOG("Inserted object %zu: HPROF ID=%" PRIu64 ", table_id=%u",
                    objects_processed, obj_id, table_id.value);
    }
  }

  PERFETTO_DLOG("Processed %zu objects", objects_processed);
}

std::string ArtHprofParserImpl::GetFieldTypeName(FieldType type) {
  switch (type) {
    case FIELD_TYPE_OBJECT: return "java.lang.Object";
    case FIELD_TYPE_BOOLEAN: return "boolean";
    case FIELD_TYPE_CHAR: return "char";
    case FIELD_TYPE_FLOAT: return "float";
    case FIELD_TYPE_DOUBLE: return "double";
    case FIELD_TYPE_BYTE: return "byte";
    case FIELD_TYPE_SHORT: return "short";
    case FIELD_TYPE_INT: return "int";
    case FIELD_TYPE_LONG: return "long";
  }
}

void ArtHprofParserImpl::PopulateReferences(
    const HeapGraph& graph,
    const std::unordered_map<uint64_t, tables::HeapGraphObjectTable::Id>& object_map) {

  auto& object_table = *context_->storage->mutable_heap_graph_object_table();
  auto& reference_table = *context_->storage->mutable_heap_graph_reference_table();
  auto& class_table = *context_->storage->mutable_heap_graph_class_table();

  // Group references by owner for efficient reference_set_id assignment
  std::unordered_map<uint64_t, std::vector<Reference>> refs_by_owner;
  size_t total_reference_count = 0;

  // Step 1: Collect all references
  PERFETTO_LOG("Collecting all references from objects...");
  for (const auto& [obj_id, obj] : graph.GetObjects()) {
    const auto& refs = obj.references();
    if (!refs.empty()) {
      refs_by_owner[obj_id].insert(
          refs_by_owner[obj_id].end(), refs.begin(), refs.end());
      total_reference_count += refs.size();
    }
  }

  PERFETTO_LOG("Found %zu total references from %zu owners",
               total_reference_count, refs_by_owner.size());

  // Step 2: Validate we have reference owners in our object map
  size_t missing_owners = 0;
  for (const auto& [owner_id, refs] : refs_by_owner) {
    if (object_map.find(owner_id) == object_map.end()) {
      missing_owners++;
      if (missing_owners <= 10) {
        PERFETTO_LOG("Warning: Reference owner %" PRIu64 " not found in object map", owner_id);
      }
    }
  }

  if (missing_owners > 0) {
    PERFETTO_LOG("Warning: %zu reference owners are missing from object map", missing_owners);
  }

  // Step 3: Build class map for type resolution
  PERFETTO_LOG("Building class map for type resolution...");
  std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id> class_map;
  for (const auto& [class_id, class_def] : graph.GetClasses()) {
    StringId name_id = context_->storage->InternString(
        base::StringView(class_def.name()));

    // Find the class ID in the table
    for (uint32_t i = 0; i < class_table.row_count(); i++) {
      if (class_table.name()[i] == name_id) {
        class_map[class_id] = tables::HeapGraphClassTable::Id(i);
        break;
      }
    }
  }
  PERFETTO_LOG("Built class map with %zu classes", class_map.size());

  // Step 4: Process references and create reference sets
  PERFETTO_LOG("Creating reference sets and populating reference table...");
  uint32_t next_reference_set_id = 1;
  size_t refs_processed = 0;
  size_t valid_refs = 0;
  size_t dangling_refs = 0;
  size_t self_refs = 0;

  // Stats for troubleshooting
  std::unordered_map<std::string, size_t> ref_type_stats;
  size_t owners_with_refs_in_db = 0;

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
    object_table.mutable_reference_set_id()->Set(
        owner_it->second.value, reference_set_id);

    // Track owners with references
    owners_with_refs_in_db++;

    bool has_valid_refs = false;

    // Process all references from this owner
    for (const auto& ref : refs) {
      refs_processed++;

      // Self-reference check
      if (ref.owner_id == ref.target_id) {
        self_refs++;
      }

      // Get owned object's table ID if it exists
      std::optional<tables::HeapGraphObjectTable::Id> owned_table_id;
      if (ref.target_id != 0) {
        auto owned_it = object_map.find(ref.target_id);
        if (owned_it != object_map.end()) {
          owned_table_id = owned_it->second;
          valid_refs++;
          has_valid_refs = true;
        } else {
          dangling_refs++;
          if (dangling_refs <= 10) {
            PERFETTO_LOG("Warning: Target object %" PRIu64 " not found for reference from %" PRIu64,
                       ref.target_id, owner_id);
          }
        }
      }

      // Get the field name
      StringId field_name_id = context_->storage->InternString(
          base::StringView(ref.field_name));

      // Track reference types for debugging
      ref_type_stats[ref.field_name]++;

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
              base::StringView("java.lang.Object"));
        }
      } else {
        // No class ID, use default
        field_type_id = context_->storage->InternString(
            base::StringView("java.lang.Object"));
      }

      // Create reference record
      tables::HeapGraphReferenceTable::Row reference_row;
      reference_row.reference_set_id = reference_set_id;
      reference_row.owner_id = owner_it->second;
      reference_row.owned_id = owned_table_id;
      reference_row.field_name = field_name_id;
      reference_row.field_type_name = field_type_id;

      reference_table.Insert(reference_row);

      // Progress logging
      if (refs_processed <= 10 || refs_processed % 50000 == 0) {
        PERFETTO_LOG(
            "Reference %zu: owner=%" PRIu64 " -> target=%" PRIu64
            " (field=%s, valid=%d)",
            refs_processed, owner_id, ref.target_id,
            ref.field_name.c_str(), owned_table_id.has_value());
      }
    }

    // Handle objects with no valid refs (could be an issue for flamegraph)
    if (!has_valid_refs) {
      PERFETTO_DLOG(
          "Object %" PRIu64 " has references but none are valid objects",
          owner_id);
    }
  }

  // Step 5: Final validation and statistics
  PERFETTO_LOG("=== Reference Processing Complete ===");
  PERFETTO_LOG("Total references processed: %zu", refs_processed);

  // Fix the type conversion issues by explicitly casting to double
  if (refs_processed > 0) {
    double valid_percentage = static_cast<double>(valid_refs) * 100.0 / static_cast<double>(refs_processed);
    double dangling_percentage = static_cast<double>(dangling_refs) * 100.0 / static_cast<double>(refs_processed);

    PERFETTO_LOG("Valid references: %zu (%.1f%%)", valid_refs, valid_percentage);
    PERFETTO_LOG("Dangling references: %zu (%.1f%%)", dangling_refs, dangling_percentage);
  } else {
    PERFETTO_LOG("Valid references: %zu (0.0%%)", valid_refs);
    PERFETTO_LOG("Dangling references: %zu (0.0%%)", dangling_refs);
  }

  PERFETTO_LOG("Self-references: %zu", self_refs);
  PERFETTO_LOG("Objects with reference sets in DB: %zu", owners_with_refs_in_db);

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

  PERFETTO_LOG("Root objects with references: %zu", roots_with_refs);
  PERFETTO_LOG("Root objects without references: %zu", roots_without_refs);

  // Print top reference types for debugging
  PERFETTO_LOG("Top reference types:");
  std::vector<std::pair<std::string, size_t>> sorted_ref_types(
      ref_type_stats.begin(), ref_type_stats.end());
  std::sort(sorted_ref_types.begin(), sorted_ref_types.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  const size_t max_types_to_show = 10;
  for (size_t i = 0; i < std::min(max_types_to_show, sorted_ref_types.size()); i++) {
    PERFETTO_LOG("  %s: %zu",
                sorted_ref_types[i].first.c_str(),
                sorted_ref_types[i].second);
  }

  // Final check for reference integrity
  if (valid_refs == 0) {
    PERFETTO_LOG("WARNING: No valid references found! Flamegraph will not render.");
  } else if (roots_with_refs == 0) {
    PERFETTO_LOG("WARNING: No root objects have references! Flamegraph may not render properly.");
  }
}
}  // namespace perfetto::trace_processor::art_hprof
