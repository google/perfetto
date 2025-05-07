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

/**
 * Main entry point for parsing an ArtHprof event.
 * This function processes the heap graph IR data and populates the trace
 * processor database tables with class, object, and reference information.
 */
void ArtHprofParserImpl::ParseArtHprofEvent(int64_t ts, ArtHprofEvent event) {
  const HeapGraph& ir = event.data;
  uint32_t os_pid = event.pid;

  // Get or create the process for this pid
  UniquePid upid = context_->process_tracker->GetOrCreateProcess(os_pid);

  // Get mutable references to the storage tables we'll be populating
  auto& class_table = *context_->storage->mutable_heap_graph_class_table();
  auto& object_table = *context_->storage->mutable_heap_graph_object_table();
  auto& reference_table =
      *context_->storage->mutable_heap_graph_reference_table();

  // Log basic info about what we're processing
  PERFETTO_DLOG(
      "Processing ArtHprofEvent with %zu classes, %zu objects, %zu references",
      ir.classes.size(), ir.objects.size(), ir.references.size());

  // If there are no classes or objects, there's nothing to do
  if (ir.classes.empty() || ir.objects.empty()) {
    PERFETTO_DLOG("Empty heap graph, skipping parsing");
    return;
  }

  //-----------------------------------------------------------------------------
  // PHASE 1: Process all classes
  //-----------------------------------------------------------------------------
  // Map HPROF's class_object_id to our table's HeapGraphClassTable::Id
  std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id>
      class_hprof_id_to_table_id;

  size_t classes_processed = 0;
  for (const auto& cls_ir : ir.classes) {
    classes_processed++;

    // Intern strings for class metadata
    StringId name_id =
        context_->storage->InternString(base::StringView(cls_ir.name));

    std::optional<StringId> deobfuscated_name_id_opt;
    if (cls_ir.deobfuscated_name.has_value() &&
        !cls_ir.deobfuscated_name->empty()) {
      deobfuscated_name_id_opt = context_->storage->InternString(
          base::StringView(*cls_ir.deobfuscated_name));
    }

    std::optional<StringId> location_id_opt;
    if (cls_ir.location.has_value() && !cls_ir.location->empty()) {
      location_id_opt =
          context_->storage->InternString(base::StringView(*cls_ir.location));
    }

    StringId kind_id =
        context_->storage->InternString(base::StringView(cls_ir.kind));

    // Create and insert the class row
    tables::HeapGraphClassTable::Row class_row;
    class_row.name = name_id;
    class_row.deobfuscated_name = deobfuscated_name_id_opt;
    class_row.location = location_id_opt;
    class_row.superclass_id = std::nullopt;  // To be filled in the second pass
    class_row.classloader_id = cls_ir.classloader_id;
    class_row.kind = kind_id;

    tables::HeapGraphClassTable::Id table_id = class_table.Insert(class_row).id;
    class_hprof_id_to_table_id[cls_ir.class_object_id] = table_id;

    // Log only a sample for performance
    if (classes_processed <= 10 || classes_processed % 1000 == 0) {
      PERFETTO_DLOG("Inserted class %zu: ID=%" PRIu64 ", name=%s, table_id=%u",
                    classes_processed, cls_ir.class_object_id,
                    cls_ir.name.c_str(), table_id.value);
    }
  }

  PERFETTO_DLOG("Processed %zu classes", classes_processed);

  //-----------------------------------------------------------------------------
  // PHASE 2: Update superclass relationships
  //-----------------------------------------------------------------------------
  int superclass_updates = 0;
  for (const auto& cls_ir : ir.classes) {
    if (cls_ir.superclass_id.has_value() && *cls_ir.superclass_id != 0) {
      auto current_class_table_id_it =
          class_hprof_id_to_table_id.find(cls_ir.class_object_id);
      auto super_class_table_id_it =
          class_hprof_id_to_table_id.find(*cls_ir.superclass_id);

      if (current_class_table_id_it != class_hprof_id_to_table_id.end() &&
          super_class_table_id_it != class_hprof_id_to_table_id.end()) {
        tables::HeapGraphClassTable::Id current_cls_tbl_id =
            current_class_table_id_it->second;
        tables::HeapGraphClassTable::Id super_cls_tbl_id =
            super_class_table_id_it->second;

        class_table.mutable_superclass_id()->Set(current_cls_tbl_id.value,
                                                 super_cls_tbl_id);
        superclass_updates++;
      } else {
        PERFETTO_DLOG(
            "Superclass ID or Class ID not found in map during "
            "superclass update. Class HPROF ID: %" PRIu64
            ", Superclass HPROF ID: %" PRIu64,
            cls_ir.class_object_id, *cls_ir.superclass_id);
      }
    }
  }

  PERFETTO_DLOG("Updated %d superclass relationships", superclass_updates);

  //-----------------------------------------------------------------------------
  // PHASE 3: Insert objects
  //-----------------------------------------------------------------------------
  // Map HPROF's object_id to our table's HeapGraphObjectTable::Id
  std::unordered_map<uint64_t, tables::HeapGraphObjectTable::Id>
      object_hprof_id_to_table_id;

  // Process all objects
  size_t objects_processed = 0;

  for (const auto& obj_ir : ir.objects) {
    objects_processed++;

    // Resolve the object's type (class)
    auto type_id_it = class_hprof_id_to_table_id.find(obj_ir.type_id);
    if (type_id_it == class_hprof_id_to_table_id.end()) {
      PERFETTO_DLOG("Class HPROF ID %" PRIu64 " for object HPROF ID %" PRIu64
                    " not found. Skipping object.",
                    obj_ir.type_id, obj_ir.object_id);
      continue;
    }
    tables::HeapGraphClassTable::Id actual_type_id = type_id_it->second;

    // Process optional heap type
    std::optional<StringId> heap_type_id_opt;
    if (obj_ir.heap_type.has_value() && !obj_ir.heap_type->empty()) {
      heap_type_id_opt =
          context_->storage->InternString(base::StringView(*obj_ir.heap_type));
    }

    // Process root type
    std::optional<StringId> root_type_id_opt;
    if (obj_ir.root_type.has_value()) {
      root_type_id_opt =
          context_->storage->InternString(base::StringView(*obj_ir.root_type));

      // Log when we set a root type
      if (objects_processed <= 10 || objects_processed % 1000 == 0) {
        PERFETTO_DLOG("Setting root type for object %" PRIu64 ": %s",
                      obj_ir.object_id, obj_ir.root_type->c_str());
      }
    }

    // Create and insert the object row
    tables::HeapGraphObjectTable::Row object_row;
    object_row.upid = upid;
    object_row.graph_sample_ts = ts;
    object_row.self_size = static_cast<int64_t>(obj_ir.self_size);
    object_row.native_size = 0;  // Default
    object_row.reference_set_id = std::nullopt;
    object_row.reachable = 1;  // Default
    object_row.heap_type = heap_type_id_opt;
    object_row.type_id = actual_type_id;
    object_row.root_type = root_type_id_opt;  // Set root type
    object_row.root_distance = -1;            // Will be calculated later

    tables::HeapGraphObjectTable::Id actual_owner_table_id =
        object_table.Insert(object_row).id;
    object_hprof_id_to_table_id[obj_ir.object_id] = actual_owner_table_id;

    // Log only a sample for performance
    if (objects_processed <= 10 || objects_processed % 10000 == 0) {
      PERFETTO_DLOG("Inserted object %zu: HPROF ID=%" PRIu64 ", table_id=%u",
                    objects_processed, obj_ir.object_id,
                    actual_owner_table_id.value);
    }
  }

  PERFETTO_DLOG("Inserted %zu objects", objects_processed);

  //-----------------------------------------------------------------------------
  // PHASE 3.5: Group references by owner and prepare reference set IDs
  //-----------------------------------------------------------------------------
  // Group references by owner_id for more efficient processing
  std::unordered_map<uint64_t, std::vector<const HeapGraphReference*>>
      refs_by_owner;
  for (const auto& ref_ir : ir.references) {
    refs_by_owner[ref_ir.owner_id].push_back(&ref_ir);
  }

  PERFETTO_DLOG("Grouped references by %zu unique owners",
                refs_by_owner.size());

  // Process reference groups (all references from the same owner)
  uint32_t next_reference_set_id = 0;
  size_t refs_processed = 0;
  size_t refs_skipped_owner = 0;
  size_t refs_skipped_owned = 0;

  for (const auto& [owner_id, refs] : refs_by_owner) {
    // Skip if no owner in our table
    auto owner_it = object_hprof_id_to_table_id.find(owner_id);
    if (owner_it == object_hprof_id_to_table_id.end()) {
      refs_skipped_owner += refs.size();
      PERFETTO_DLOG("Reference owner not found: %" PRIu64
                    " (%zu references skipped)",
                    owner_id, refs.size());
      continue;
    }

    // Skip if no references (shouldn't happen but just to be safe)
    if (refs.empty()) {
      continue;
    }

    // Assign a single reference_set_id for this owner
    uint32_t reference_set_id = next_reference_set_id;
    tables::HeapGraphObjectTable::Id actual_owner_id = owner_it->second;

    // Update the object table with this reference_set_id
    object_table.mutable_reference_set_id()->Set(actual_owner_id.value,
                                                 reference_set_id);

    PERFETTO_DLOG("Assigned reference_set_id %u to object %" PRIu64
                  " with %zu references",
                  reference_set_id, owner_id, refs.size());

    // Process all references from this owner with the same reference_set_id
    for (const HeapGraphReference* ref_ptr : refs) {
      next_reference_set_id++;
      const auto& ref_ir = *ref_ptr;

      // Resolve the owned object if it exists
      std::optional<tables::HeapGraphObjectTable::Id> actual_owned_id_opt;
      if (ref_ir.owned_id.has_value() && *ref_ir.owned_id != 0) {
        auto owned_it = object_hprof_id_to_table_id.find(*ref_ir.owned_id);
        if (owned_it != object_hprof_id_to_table_id.end()) {
          actual_owned_id_opt = owned_it->second;
        } else {
          refs_skipped_owned++;
          if (refs_skipped_owned <= 10 || refs_skipped_owned % 10000 == 0) {
            PERFETTO_DLOG("Reference owned not found: owner=%" PRIu64
                          ", owned=%" PRIu64,
                          ref_ir.owner_id, *ref_ir.owned_id);
          }
        }
      }

      // Intern field name and type strings
      StringId field_name_id =
          context_->storage->InternString(base::StringView(ref_ir.field_name));
      StringId field_type_name_id = context_->storage->InternString(
          base::StringView(ref_ir.field_type_name));

      // Create and insert the reference
      tables::HeapGraphReferenceTable::Row reference_row;
      reference_row.reference_set_id = reference_set_id;
      reference_row.owner_id = actual_owner_id;
      reference_row.owned_id = actual_owned_id_opt;
      reference_row.field_name = field_name_id;
      reference_row.field_type_name = field_type_name_id;
      reference_row.deobfuscated_field_name = std::nullopt;

      reference_table.Insert(reference_row);
      refs_processed++;

      // Log only a sample
      if (refs_processed <= 10 || refs_processed % 10000 == 0) {
        PERFETTO_DLOG("Processed reference %zu: owner=%" PRIu64
                      ", owned=%s, field=%s",
                      refs_processed, ref_ir.owner_id,
                      ref_ir.owned_id.has_value()
                          ? std::to_string(*ref_ir.owned_id).c_str()
                          : "null",
                      ref_ir.field_name.c_str());
      }
    }
  }

  //-----------------------------------------------------------------------------
  // PHASE 5: Calculate root distances and mark reachable objects
  //-----------------------------------------------------------------------------

  // Check reference_set_id sharing
  std::unordered_map<uint32_t, uint32_t> refs_per_set_id;
  for (uint32_t i = 0; i < reference_table.row_count(); i++) {
    uint32_t set_id = reference_table.reference_set_id()[i];
    refs_per_set_id[set_id]++;
  }

  size_t single_ref_sets = 0;
  size_t multi_ref_sets = 0;
  size_t max_refs_in_set = 0;
  for (const auto& [set_id, count] : refs_per_set_id) {
    if (count == 1) {
      single_ref_sets++;
    } else {
      multi_ref_sets++;
      if (count > max_refs_in_set) {
        max_refs_in_set = count;
      }
    }
  }

  PERFETTO_LOG(
      "Reference set ID stats: %zu sets with single ref, %zu sets with "
      "multiple refs",
      single_ref_sets, multi_ref_sets);
  PERFETTO_LOG("Max references in a single set: %zu", max_refs_in_set);

  PERFETTO_LOG(
      "ArtHprofEvent parsing complete: %zu classes, %zu objects, "
      "%zu references processed, %zu owner refs skipped, %zu owned refs "
      "skipped",
      classes_processed, objects_processed, refs_processed, refs_skipped_owner,
      refs_skipped_owned);
}

}  // namespace perfetto::trace_processor::art_hprof
