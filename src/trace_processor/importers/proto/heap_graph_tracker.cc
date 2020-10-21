/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/heap_graph_tracker.h"

#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/proto/profiler_util.h"
#include "src/trace_processor/tables/profiler_tables.h"

#include <set>
#include <utility>

namespace perfetto {
namespace trace_processor {

namespace {

template <typename F>
void ForReferenceSet(const TraceStorage& storage,
                     tables::HeapGraphObjectTable::Id id,
                     F fn) {
  uint32_t row = *storage.heap_graph_object_table().id().IndexOf(id);
  base::Optional<uint32_t> reference_set_id =
      storage.heap_graph_object_table().reference_set_id()[row];
  if (!reference_set_id)
    return;
  uint32_t cur_reference_set_id;
  for (uint32_t reference_row = *reference_set_id;
       reference_row < storage.heap_graph_reference_table().row_count();
       ++reference_row) {
    cur_reference_set_id =
        storage.heap_graph_reference_table().reference_set_id()[reference_row];
    if (cur_reference_set_id != *reference_set_id)
      break;
    if (!fn(reference_row))
      break;
  }
}

std::set<tables::HeapGraphObjectTable::Id> GetChildren(
    const TraceStorage& storage,
    tables::HeapGraphObjectTable::Id id) {
  std::set<tables::HeapGraphObjectTable::Id> children;
  ForReferenceSet(
      storage, id, [&storage, &children, id](uint32_t reference_row) {
        PERFETTO_CHECK(
            storage.heap_graph_reference_table().owner_id()[reference_row] ==
            id);
        children.emplace(
            storage.heap_graph_reference_table().owned_id()[reference_row]);
        return true;
      });
  return children;
}

struct ClassDescriptor {
  StringId name;
  base::Optional<StringId> location;

  bool operator<(const ClassDescriptor& other) const {
    return std::tie(name, location) < std::tie(other.name, other.location);
  }
};

ClassDescriptor GetClassDescriptor(const TraceStorage& storage,
                                   tables::HeapGraphObjectTable::Id obj_id) {
  auto obj_idx = storage.heap_graph_object_table().id().IndexOf(obj_id).value();
  auto type_id = storage.heap_graph_object_table().type_id()[obj_idx];
  auto type_idx =
      storage.heap_graph_class_table().id().IndexOf(type_id).value();
  return {storage.heap_graph_class_table().name()[type_idx],
          storage.heap_graph_class_table().location()[type_idx]};
}

base::Optional<tables::HeapGraphObjectTable::Id> GetReferredObj(
    const TraceStorage& storage,
    uint32_t ref_set_id,
    const std::string& field_name) {
  const auto& refs_tbl = storage.heap_graph_reference_table();

  auto filtered = refs_tbl.Filter(
      {refs_tbl.reference_set_id().eq(ref_set_id),
       refs_tbl.field_name().eq(NullTermStringView(field_name))});
  auto refs_it = filtered.IterateRows();
  if (!refs_it) {
    return {};
  }
  return tables::HeapGraphObjectTable::Id(static_cast<uint32_t>(
      refs_it
          .Get(static_cast<uint32_t>(
              tables::HeapGraphReferenceTable::ColumnIndex::owned_id))
          .AsLong()));
}

// Maps from normalized class name and location, to superclass.
std::map<ClassDescriptor, ClassDescriptor>
BuildSuperclassMap(UniquePid upid, int64_t ts, TraceStorage* storage) {
  std::map<ClassDescriptor, ClassDescriptor> superclass_map;

  // Resolve superclasses by iterating heap graph objects and identifying the
  // superClass field.
  const auto& objects_tbl = storage->heap_graph_object_table();
  auto filtered = objects_tbl.Filter(
      {objects_tbl.upid().eq(upid), objects_tbl.graph_sample_ts().eq(ts)});
  for (auto obj_it = filtered.IterateRows(); obj_it; obj_it.Next()) {
    auto obj_id = tables::HeapGraphObjectTable::Id(static_cast<uint32_t>(
        obj_it
            .Get(static_cast<uint32_t>(
                tables::HeapGraphObjectTable::ColumnIndex::id))
            .AsLong()));
    auto class_descriptor = GetClassDescriptor(*storage, obj_id);
    auto normalized =
        GetNormalizedType(storage->GetString(class_descriptor.name));
    // superClass ptrs are stored on the static class objects
    // ignore arrays (as they are generated objects)
    if (!normalized.is_static_class || normalized.number_of_arrays > 0)
      continue;

    auto opt_ref_set_id = obj_it.Get(static_cast<uint32_t>(
        tables::HeapGraphObjectTable::ColumnIndex::reference_set_id));
    if (opt_ref_set_id.is_null())
      continue;
    auto ref_set_id = static_cast<uint32_t>(opt_ref_set_id.AsLong());
    auto super_obj_id =
        GetReferredObj(*storage, ref_set_id, "java.lang.Class.superClass");
    if (!super_obj_id) {
      // This is expected to be missing for Object and primitive types
      continue;
    }

    // Lookup the super obj type id
    auto super_class_descriptor = GetClassDescriptor(*storage, *super_obj_id);
    auto super_class_name =
        NormalizeTypeName(storage->GetString(super_class_descriptor.name));
    StringId super_class_id = storage->InternString(super_class_name);
    StringId class_id = storage->InternString(normalized.name);
    superclass_map[{class_id, class_descriptor.location}] = {
        super_class_id, super_class_descriptor.location};
  }
  return superclass_map;
}

}  // namespace

void MarkRoot(TraceStorage* storage,
              tables::HeapGraphObjectTable::Id id,
              StringPool::Id type) {
  uint32_t row = *storage->heap_graph_object_table().id().IndexOf(id);
  storage->mutable_heap_graph_object_table()->mutable_root_type()->Set(row,
                                                                       type);

  // Calculate shortest distance to a GC root.
  std::deque<std::pair<int32_t, tables::HeapGraphObjectTable::Id>>
      reachable_nodes{{0, id}};
  while (!reachable_nodes.empty()) {
    tables::HeapGraphObjectTable::Id cur_node;
    int32_t distance;
    std::tie(distance, cur_node) = reachable_nodes.front();
    reachable_nodes.pop_front();
    uint32_t cur_row =
        *storage->heap_graph_object_table().id().IndexOf(cur_node);
    int32_t cur_distance =
        storage->heap_graph_object_table().root_distance()[cur_row];
    if (cur_distance == -1 || cur_distance > distance) {
      if (cur_distance == -1) {
        storage->mutable_heap_graph_object_table()->mutable_reachable()->Set(
            cur_row, 1);
      }
      storage->mutable_heap_graph_object_table()->mutable_root_distance()->Set(
          cur_row, distance);

      for (tables::HeapGraphObjectTable::Id child_node :
           GetChildren(*storage, cur_node)) {
        uint32_t child_row =
            *storage->heap_graph_object_table().id().IndexOf(child_node);
        int32_t child_distance =
            storage->heap_graph_object_table().root_distance()[child_row];
        if (child_distance == -1 || child_distance > distance + 1)
          reachable_nodes.emplace_back(distance + 1, child_node);
      }
    }
  }
}

base::Optional<base::StringView> GetStaticClassTypeName(base::StringView type) {
  static const base::StringView kJavaClassTemplate("java.lang.Class<");
  if (!type.empty() && type.at(type.size() - 1) == '>' &&
      type.substr(0, kJavaClassTemplate.size()) == kJavaClassTemplate) {
    return type.substr(kJavaClassTemplate.size(),
                       type.size() - kJavaClassTemplate.size() - 1);
  }
  return {};
}

size_t NumberOfArrays(base::StringView type) {
  if (type.size() < 2)
    return 0;

  size_t arrays = 0;
  while (type.size() >= 2 * (arrays + 1) &&
         memcmp(type.end() - 2 * (arrays + 1), "[]", 2) == 0) {
    arrays++;
  }

  return arrays;
}

NormalizedType GetNormalizedType(base::StringView type) {
  auto static_class_type_name = GetStaticClassTypeName(type);
  if (static_class_type_name.has_value()) {
    type = static_class_type_name.value();
  }
  size_t number_of_arrays = NumberOfArrays(type);
  return {base::StringView(type.data(), type.size() - number_of_arrays * 2),
          static_class_type_name.has_value(), number_of_arrays};
}

base::StringView NormalizeTypeName(base::StringView type) {
  return GetNormalizedType(type).name;
}

std::string DenormalizeTypeName(NormalizedType normalized,
                                base::StringView deobfuscated_type_name) {
  std::string result = deobfuscated_type_name.ToStdString();
  for (size_t i = 0; i < normalized.number_of_arrays; ++i) {
    result += "[]";
  }
  if (normalized.is_static_class) {
    result = "java.lang.Class<" + result + ">";
  }
  return result;
}

HeapGraphTracker::HeapGraphTracker(TraceProcessorContext* context)
    : context_(context) {}

HeapGraphTracker::SequenceState& HeapGraphTracker::GetOrCreateSequence(
    uint32_t seq_id) {
  return sequence_state_[seq_id];
}

bool HeapGraphTracker::SetPidAndTimestamp(SequenceState* sequence_state,
                                          UniquePid upid,
                                          int64_t ts) {
  if (sequence_state->current_upid != 0 &&
      sequence_state->current_upid != upid) {
    context_->storage->IncrementStats(stats::heap_graph_non_finalized_graph);
    return false;
  }
  if (sequence_state->current_ts != 0 && sequence_state->current_ts != ts) {
    context_->storage->IncrementStats(stats::heap_graph_non_finalized_graph);
    return false;
  }
  sequence_state->current_upid = upid;
  sequence_state->current_ts = ts;
  return true;
}

tables::HeapGraphObjectTable::Id HeapGraphTracker::GetOrInsertObject(
    SequenceState* sequence_state,
    uint64_t object_id) {
  auto it = sequence_state->object_id_to_db_id.find(object_id);
  if (it == sequence_state->object_id_to_db_id.end()) {
    auto id_and_row =
        context_->storage->mutable_heap_graph_object_table()->Insert(
            {sequence_state->current_upid,
             sequence_state->current_ts,
             -1,
             /*reference_set_id=*/base::nullopt,
             /*reachable=*/0,
             {},
             /*root_type=*/base::nullopt,
             /*root_distance*/ -1});
    bool inserted;
    std::tie(it, inserted) =
        sequence_state->object_id_to_db_id.emplace(object_id, id_and_row.id);
  }
  return it->second;
}

tables::HeapGraphClassTable::Id HeapGraphTracker::GetOrInsertType(
    SequenceState* sequence_state,
    uint64_t type_id) {
  auto it = sequence_state->type_id_to_db_id.find(type_id);
  if (it == sequence_state->type_id_to_db_id.end()) {
    auto id_and_row =
        context_->storage->mutable_heap_graph_class_table()->Insert(
            {StringPool::Id(), base::nullopt, base::nullopt});
    bool inserted;
    std::tie(it, inserted) =
        sequence_state->type_id_to_db_id.emplace(type_id, id_and_row.id);
  }
  return it->second;
}

void HeapGraphTracker::AddObject(uint32_t seq_id,
                                 UniquePid upid,
                                 int64_t ts,
                                 SourceObject obj) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);

  if (!SetPidAndTimestamp(&sequence_state, upid, ts))
    return;

  tables::HeapGraphObjectTable::Id owner_id =
      GetOrInsertObject(&sequence_state, obj.object_id);
  tables::HeapGraphClassTable::Id type_id =
      GetOrInsertType(&sequence_state, obj.type_id);

  auto* hgo = context_->storage->mutable_heap_graph_object_table();
  uint32_t row = *hgo->id().IndexOf(owner_id);

  hgo->mutable_self_size()->Set(row, static_cast<int64_t>(obj.self_size));
  hgo->mutable_type_id()->Set(row, type_id);

  if (obj.self_size == 0)
    sequence_state.deferred_size_objects_for_type_[type_id].push_back(owner_id);

  uint32_t reference_set_id =
      context_->storage->heap_graph_reference_table().row_count();
  bool any_references = false;

  for (size_t i = 0; i < obj.referred_objects.size(); ++i) {
    uint64_t owned_object_id = obj.referred_objects[i];
    // This is true for unset reference fields.
    if (owned_object_id == 0)
      continue;
    tables::HeapGraphObjectTable::Id owned_id =
        GetOrInsertObject(&sequence_state, owned_object_id);

    auto ref_id_and_row =
        context_->storage->mutable_heap_graph_reference_table()->Insert(
            {reference_set_id,
             owner_id,
             owned_id,
             {},
             {},
             /*deobfuscated_field_name=*/base::nullopt});
    if (!obj.field_name_ids.empty()) {
      sequence_state.references_for_field_name_id[obj.field_name_ids[i]]
          .push_back(ref_id_and_row.id);
    }
    any_references = true;
  }
  if (any_references) {
    uint32_t owner_row =
        *context_->storage->heap_graph_object_table().id().IndexOf(owner_id);
    context_->storage->mutable_heap_graph_object_table()
        ->mutable_reference_set_id()
        ->Set(owner_row, reference_set_id);
    if (obj.field_name_ids.empty()) {
      sequence_state.deferred_reference_objects_for_type_[type_id].push_back(
          owner_id);
    }
  }
}

void HeapGraphTracker::AddRoot(uint32_t seq_id,
                               UniquePid upid,
                               int64_t ts,
                               SourceRoot root) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  if (!SetPidAndTimestamp(&sequence_state, upid, ts))
    return;

  sequence_state.current_roots.emplace_back(std::move(root));
}

void HeapGraphTracker::AddInternedLocationName(uint32_t seq_id,
                                               uint64_t intern_id,
                                               StringPool::Id strid) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  sequence_state.interned_location_names.emplace(intern_id, strid);
}

void HeapGraphTracker::AddInternedType(uint32_t seq_id,
                                       uint64_t intern_id,
                                       StringPool::Id strid,
                                       uint64_t location_id,
                                       uint64_t object_size,
                                       std::vector<uint64_t> field_name_ids,
                                       uint64_t superclass_id,
                                       uint64_t classloader_id,
                                       bool no_fields) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  sequence_state.interned_types[intern_id].name = strid;
  sequence_state.interned_types[intern_id].location_id = location_id;
  sequence_state.interned_types[intern_id].object_size = object_size;
  sequence_state.interned_types[intern_id].field_name_ids =
      std::move(field_name_ids);
  sequence_state.interned_types[intern_id].superclass_id = superclass_id;
  sequence_state.interned_types[intern_id].classloader_id = classloader_id;
  sequence_state.interned_types[intern_id].no_fields = no_fields;
}

void HeapGraphTracker::AddInternedFieldName(uint32_t seq_id,
                                            uint64_t intern_id,
                                            base::StringView str) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  size_t space = str.find(' ');
  base::StringView type;
  if (space != base::StringView::npos) {
    type = str.substr(0, space);
    str = str.substr(space + 1);
  }
  StringPool::Id field_name = context_->storage->InternString(str);
  StringPool::Id type_name = context_->storage->InternString(type);

  sequence_state.interned_fields.emplace(intern_id,
                                         InternedField{field_name, type_name});

  auto it = sequence_state.references_for_field_name_id.find(intern_id);
  if (it != sequence_state.references_for_field_name_id.end()) {
    auto hgr = context_->storage->mutable_heap_graph_reference_table();
    for (const tables::HeapGraphReferenceTable::Id reference_id : it->second) {
      uint32_t row = *hgr->id().IndexOf(reference_id);
      hgr->mutable_field_name()->Set(row, field_name);
      hgr->mutable_field_type_name()->Set(row, type_name);

      field_to_rows_[field_name].emplace_back(row);
    }
  }
}

void HeapGraphTracker::SetPacketIndex(uint32_t seq_id, uint64_t index) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  bool dropped_packet = false;
  // perfetto_hprof starts counting at index = 0.
  if (!sequence_state.prev_index && index != 0) {
    dropped_packet = true;
  }

  if (sequence_state.prev_index && *sequence_state.prev_index + 1 != index) {
    dropped_packet = true;
  }

  if (dropped_packet) {
    sequence_state.truncated = true;
    if (sequence_state.prev_index) {
      PERFETTO_ELOG("Missing packets between %" PRIu64 " and %" PRIu64,
                    *sequence_state.prev_index, index);
    } else {
      PERFETTO_ELOG("Invalid first packet index %" PRIu64 " (!= 0)", index);
    }

    context_->storage->IncrementIndexedStats(
        stats::heap_graph_missing_packet,
        static_cast<int>(sequence_state.current_upid));
  }
  sequence_state.prev_index = index;
}

// This only works on Android S+ traces. We need to have ingested the whole
// profile before calling this function (e.g. in FinalizeProfile).
HeapGraphTracker::InternedType* HeapGraphTracker::GetSuperClass(
    SequenceState* sequence_state,
    const InternedType* current_type) {
  if (current_type->superclass_id) {
    auto it = sequence_state->interned_types.find(current_type->superclass_id);
    if (it != sequence_state->interned_types.end())
      return &it->second;
  }
  context_->storage->IncrementIndexedStats(
      stats::heap_graph_malformed_packet,
      static_cast<int>(sequence_state->current_upid));
  return nullptr;
}

void HeapGraphTracker::FinalizeProfile(uint32_t seq_id) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  if (sequence_state.truncated) {
    truncated_graphs_.emplace(
        std::make_pair(sequence_state.current_upid, sequence_state.current_ts));
  }

  // We do this in FinalizeProfile because the interned_location_names get
  // written at the end of the dump.
  for (const auto& p : sequence_state.interned_types) {
    uint64_t id = p.first;
    const InternedType& interned_type = p.second;
    base::Optional<StringPool::Id> location_name;
    if (interned_type.location_id) {
      auto it = sequence_state.interned_location_names.find(
          *interned_type.location_id);
      if (it == sequence_state.interned_location_names.end()) {
        context_->storage->IncrementIndexedStats(
            stats::heap_graph_invalid_string_id,
            static_cast<int>(sequence_state.current_upid));
      } else {
        location_name = it->second;
      }
    }
    tables::HeapGraphClassTable::Id type_id =
        GetOrInsertType(&sequence_state, id);

    auto sz_obj_it =
        sequence_state.deferred_size_objects_for_type_.find(type_id);
    if (sz_obj_it != sequence_state.deferred_size_objects_for_type_.end()) {
      for (tables::HeapGraphObjectTable::Id obj_id : sz_obj_it->second) {
        auto* hgo = context_->storage->mutable_heap_graph_object_table();
        uint32_t row = *hgo->id().IndexOf(obj_id);
        hgo->mutable_self_size()->Set(
            row, static_cast<int64_t>(interned_type.object_size));
      }
      sequence_state.deferred_size_objects_for_type_.erase(sz_obj_it);
    }

    auto ref_obj_it =
        sequence_state.deferred_reference_objects_for_type_.find(type_id);
    if (ref_obj_it !=
        sequence_state.deferred_reference_objects_for_type_.end()) {
      for (tables::HeapGraphObjectTable::Id obj_id : ref_obj_it->second) {
        const InternedType* current_type = &interned_type;
        if (interned_type.no_fields) {
          continue;
        }
        size_t field_offset_in_cls = 0;
        ForReferenceSet(
            *context_->storage, obj_id,
            [this, &current_type, &sequence_state,
             &field_offset_in_cls](uint32_t reference_row) {
              while (current_type && field_offset_in_cls >=
                                         current_type->field_name_ids.size()) {
                size_t prev_type_size = current_type->field_name_ids.size();
                current_type = GetSuperClass(&sequence_state, current_type);
                field_offset_in_cls -= prev_type_size;
              }

              if (!current_type) {
                return false;
              }

              uint64_t field_id =
                  current_type->field_name_ids[field_offset_in_cls++];
              auto it = sequence_state.interned_fields.find(field_id);
              if (it == sequence_state.interned_fields.end()) {
                PERFETTO_ELOG("Invalid field id.");
                context_->storage->IncrementIndexedStats(
                    stats::heap_graph_malformed_packet,
                    static_cast<int>(sequence_state.current_upid));
                return true;
              }
              const InternedField& field = it->second;
              auto hgr =
                  context_->storage->mutable_heap_graph_reference_table();
              hgr->mutable_field_name()->Set(reference_row, field.name);
              hgr->mutable_field_type_name()->Set(reference_row,
                                                  field.type_name);
              field_to_rows_[field.name].emplace_back(reference_row);
              return true;
            });
      }
      sequence_state.deferred_reference_objects_for_type_.erase(ref_obj_it);
    }

    auto* hgc = context_->storage->mutable_heap_graph_class_table();
    uint32_t row = *hgc->id().IndexOf(type_id);
    hgc->mutable_name()->Set(row, interned_type.name);
    if (interned_type.classloader_id) {
      auto classloader_object_id =
          GetOrInsertObject(&sequence_state, interned_type.classloader_id);
      hgc->mutable_classloader_id()->Set(row, classloader_object_id.value);
    }
    if (location_name)
      hgc->mutable_location()->Set(row, *location_name);

    base::StringView normalized_type =
        NormalizeTypeName(context_->storage->GetString(interned_type.name));

    // Annoyingly, some apps have a relative path to base.apk. We take this to
    // mean the main package, so we treat it as if the location was unknown.
    bool is_base_apk = false;
    if (location_name) {
      base::StringView base_apk("base.apk");
      is_base_apk = context_->storage->GetString(*location_name)
                        .substr(0, base_apk.size()) == base_apk;
    }

    if (location_name && !is_base_apk) {
      base::Optional<std::string> package_name =
          PackageFromLocation(context_->storage.get(),
                              context_->storage->GetString(*location_name));
      if (package_name) {
        class_to_rows_[std::make_pair(
                           context_->storage->InternString(
                               base::StringView(*package_name)),
                           context_->storage->InternString(normalized_type))]
            .emplace_back(type_id);
      }
    } else {
      // TODO(b/153552977): Remove this workaround.
      // For profiles collected for old versions of perfetto_hprof, we do not
      // have any location information. We store them using the nullopt
      // location, and assume they are all part of the main APK.
      //
      // This is to keep ingestion of old profiles working (especially
      // important for the UI).
      class_to_rows_[std::make_pair(
                         base::nullopt,
                         context_->storage->InternString(normalized_type))]
          .emplace_back(type_id);
    }
  }

  if (!sequence_state.deferred_size_objects_for_type_.empty()) {
    context_->storage->IncrementIndexedStats(
        stats::heap_graph_malformed_packet,
        static_cast<int>(sequence_state.current_upid));
  }

  if (!sequence_state.deferred_reference_objects_for_type_.empty()) {
    context_->storage->IncrementIndexedStats(
        stats::heap_graph_malformed_packet,
        static_cast<int>(sequence_state.current_upid));
  }

  for (const SourceRoot& root : sequence_state.current_roots) {
    for (uint64_t obj_id : root.object_ids) {
      auto it = sequence_state.object_id_to_db_id.find(obj_id);
      // This can only happen for an invalid type string id, which is already
      // reported as an error. Silently continue here.
      if (it == sequence_state.object_id_to_db_id.end())
        continue;

      tables::HeapGraphObjectTable::Id db_id = it->second;
      auto it_and_success = roots_[std::make_pair(sequence_state.current_upid,
                                                  sequence_state.current_ts)]
                                .emplace(db_id);
      if (it_and_success.second)
        MarkRoot(context_->storage.get(), db_id, root.root_type);
    }
  }

  PopulateSuperClasses(sequence_state);
  sequence_state_.erase(seq_id);
}

// TODO(fmayer): For Android S+ traces, use the superclass_id from the trace.
void HeapGraphTracker::PopulateSuperClasses(const SequenceState& seq) {
  // Maps from normalized class name and location, to superclass.
  std::map<ClassDescriptor, ClassDescriptor> superclass_map =
      BuildSuperclassMap(seq.current_upid, seq.current_ts,
                         context_->storage.get());

  auto* classes_tbl = context_->storage->mutable_heap_graph_class_table();
  std::map<ClassDescriptor, tables::HeapGraphClassTable::Id> class_to_id;
  for (uint32_t idx = 0; idx < classes_tbl->row_count(); ++idx) {
    class_to_id[{classes_tbl->name()[idx], classes_tbl->location()[idx]}] =
        classes_tbl->id()[idx];
  }

  // Iterate through the classes table and annotate with superclasses.
  // We iterate all rows on the classes table (even though the superclass
  // mapping was generated on the current sequence) - if we cannot identify
  // a superclass we will just skip.
  for (uint32_t idx = 0; idx < classes_tbl->row_count(); ++idx) {
    auto name = context_->storage->GetString(classes_tbl->name()[idx]);
    auto location = classes_tbl->location()[idx];
    auto normalized = GetNormalizedType(name);
    if (normalized.is_static_class || normalized.number_of_arrays > 0)
      continue;

    StringId class_name_id = context_->storage->InternString(normalized.name);
    auto map_it = superclass_map.find({class_name_id, location});
    if (map_it == superclass_map.end()) {
      continue;
    }

    // Find the row for the superclass id
    auto superclass_it = class_to_id.find(map_it->second);
    if (superclass_it == class_to_id.end()) {
      // This can happen for traces was captured before the patch to
      // explicitly emit interned types (meaning classes without live
      // instances would not appear here).
      continue;
    }
    auto superclass_id = superclass_it->second;
    // Mutate the superclass column
    classes_tbl->mutable_superclass_id()->Set(idx, superclass_id);
  }
}

void FindPathFromRoot(const TraceStorage& storage,
                      tables::HeapGraphObjectTable::Id id,
                      PathFromRoot* path) {
  // We have long retention chains (e.g. from LinkedList). If we use the stack
  // here, we risk running out of stack space. This is why we use a vector to
  // simulate the stack.
  struct StackElem {
    tables::HeapGraphObjectTable::Id node;  // Node in the original graph.
    size_t parent_id;  // id of parent node in the result tree.
    size_t i;          // Index of the next child of this node to handle.
    uint32_t depth;    // Depth in the resulting tree
                       // (including artifical root).
    std::vector<tables::HeapGraphObjectTable::Id> children;
  };

  std::vector<StackElem> stack{{id, PathFromRoot::kRoot, 0, 0, {}}};

  while (!stack.empty()) {
    tables::HeapGraphObjectTable::Id n = stack.back().node;
    uint32_t row = *storage.heap_graph_object_table().id().IndexOf(n);
    size_t parent_id = stack.back().parent_id;
    uint32_t depth = stack.back().depth;
    size_t& i = stack.back().i;
    std::vector<tables::HeapGraphObjectTable::Id>& children =
        stack.back().children;

    tables::HeapGraphClassTable::Id type_id =
        storage.heap_graph_object_table().type_id()[row];

    uint32_t type_row = *storage.heap_graph_class_table().id().IndexOf(type_id);
    base::Optional<StringPool::Id> opt_class_name_id =
        storage.heap_graph_class_table().deobfuscated_name()[type_row];
    if (!opt_class_name_id) {
      opt_class_name_id = storage.heap_graph_class_table().name()[type_row];
    }
    PERFETTO_CHECK(opt_class_name_id);
    StringPool::Id class_name_id = *opt_class_name_id;
    auto it = path->nodes[parent_id].children.find(class_name_id);
    if (it == path->nodes[parent_id].children.end()) {
      size_t path_id = path->nodes.size();
      path->nodes.emplace_back(PathFromRoot::Node{});
      std::tie(it, std::ignore) =
          path->nodes[parent_id].children.emplace(class_name_id, path_id);
      path->nodes.back().class_name_id = class_name_id;
      path->nodes.back().depth = depth;
      path->nodes.back().parent_id = parent_id;
    }
    size_t path_id = it->second;
    PathFromRoot::Node* output_tree_node = &path->nodes[path_id];

    if (i == 0) {
      // This is the first time we are looking at this node, so add its
      // size to the relevant node in the resulting tree.
      output_tree_node->size +=
          storage.heap_graph_object_table().self_size()[row];
      output_tree_node->count++;
      std::set<tables::HeapGraphObjectTable::Id> children_set =
          GetChildren(storage, n);
      children.assign(children_set.cbegin(), children_set.cend());
      PERFETTO_CHECK(children.size() == children_set.size());
    }
    // Otherwise we have already handled this node and just need to get its
    // i-th child.
    if (!children.empty()) {
      PERFETTO_CHECK(i < children.size());
      tables::HeapGraphObjectTable::Id child = children[i];
      uint32_t child_row =
          *storage.heap_graph_object_table().id().IndexOf(child);
      if (++i == children.size())
        stack.pop_back();

      int32_t child_distance =
          storage.heap_graph_object_table().root_distance()[child_row];
      int32_t n_distance =
          storage.heap_graph_object_table().root_distance()[row];
      PERFETTO_CHECK(n_distance >= 0);
      PERFETTO_CHECK(child_distance >= 0);

      bool visited = path->visited.count(child);

      if (child_distance == n_distance + 1 && !visited) {
        path->visited.emplace(child);
        stack.emplace_back(StackElem{child, path_id, 0, depth + 1, {}});
      }
    } else {
      stack.pop_back();
    }
  }
}

std::unique_ptr<tables::ExperimentalFlamegraphNodesTable>
HeapGraphTracker::BuildFlamegraph(const int64_t current_ts,
                                  const UniquePid current_upid) {
  auto profile_type = context_->storage->InternString("graph");
  auto java_mapping = context_->storage->InternString("JAVA");

  std::unique_ptr<tables::ExperimentalFlamegraphNodesTable> tbl(
      new tables::ExperimentalFlamegraphNodesTable(
          context_->storage->mutable_string_pool(), nullptr));

  auto it = roots_.find(std::make_pair(current_upid, current_ts));
  if (it == roots_.end()) {
    // TODO(fmayer): This should not be within the flame graph but some marker
    // in the UI.
    if (IsTruncated(current_upid, current_ts)) {
      tables::ExperimentalFlamegraphNodesTable::Row alloc_row{};
      alloc_row.ts = current_ts;
      alloc_row.upid = current_upid;
      alloc_row.profile_type = profile_type;
      alloc_row.depth = 0;
      alloc_row.name =
          context_->storage->InternString("ERROR: INCOMPLETE GRAPH");
      alloc_row.map_name = java_mapping;
      alloc_row.count = 1;
      alloc_row.cumulative_count = 1;
      alloc_row.size = 1;
      alloc_row.cumulative_size = 1;
      alloc_row.parent_id = base::nullopt;
      tbl->Insert(alloc_row);
      return tbl;
    }
    // We haven't seen this graph, so we should raise an error.
    return nullptr;
  }

  const std::set<tables::HeapGraphObjectTable::Id>& roots = it->second;

  PathFromRoot init_path;
  for (tables::HeapGraphObjectTable::Id root : roots) {
    FindPathFromRoot(*context_->storage, root, &init_path);
  }

  std::vector<int32_t> node_to_cumulative_size(init_path.nodes.size());
  std::vector<int32_t> node_to_cumulative_count(init_path.nodes.size());
  // i > 0 is to skip the artifical root node.
  for (size_t i = init_path.nodes.size() - 1; i > 0; --i) {
    const PathFromRoot::Node& node = init_path.nodes[i];

    node_to_cumulative_size[i] += node.size;
    node_to_cumulative_count[i] += node.count;
    node_to_cumulative_size[node.parent_id] += node_to_cumulative_size[i];
    node_to_cumulative_count[node.parent_id] += node_to_cumulative_count[i];
  }

  std::vector<FlamegraphId> node_to_id(init_path.nodes.size());
  // i = 1 is to skip the artifical root node.
  for (size_t i = 1; i < init_path.nodes.size(); ++i) {
    const PathFromRoot::Node& node = init_path.nodes[i];
    PERFETTO_CHECK(node.parent_id < i);
    base::Optional<FlamegraphId> parent_id;
    if (node.parent_id != 0)
      parent_id = node_to_id[node.parent_id];
    const uint32_t depth = node.depth;

    tables::ExperimentalFlamegraphNodesTable::Row alloc_row{};
    alloc_row.ts = current_ts;
    alloc_row.upid = current_upid;
    alloc_row.profile_type = profile_type;
    alloc_row.depth = depth;
    alloc_row.name = node.class_name_id;
    alloc_row.map_name = java_mapping;
    alloc_row.count = static_cast<int64_t>(node.count);
    alloc_row.cumulative_count =
        static_cast<int64_t>(node_to_cumulative_count[i]);
    alloc_row.size = static_cast<int64_t>(node.size);
    alloc_row.cumulative_size =
        static_cast<int64_t>(node_to_cumulative_size[i]);
    alloc_row.parent_id = parent_id;
    node_to_id[i] = tbl->Insert(alloc_row).id;
  }
  return tbl;
}

void HeapGraphTracker::NotifyEndOfFile() {
  if (!sequence_state_.empty()) {
    context_->storage->IncrementStats(stats::heap_graph_non_finalized_graph);
    // There might still be valuable data even though the trace is truncated.
    while (!sequence_state_.empty()) {
      FinalizeProfile(sequence_state_.begin()->first);
    }
  }
}

bool HeapGraphTracker::IsTruncated(UniquePid upid, int64_t ts) {
  // The graph was finalized but was missing packets.
  if (truncated_graphs_.find(std::make_pair(upid, ts)) !=
      truncated_graphs_.end()) {
    return true;
  }

  // Or the graph was never finalized, so is missing packets at the end.
  for (const auto& p : sequence_state_) {
    const SequenceState& sequence_state = p.second;
    if (sequence_state.current_upid == upid &&
        sequence_state.current_ts == ts) {
      return true;
    }
  }
  return false;
}

HeapGraphTracker::~HeapGraphTracker() = default;

}  // namespace trace_processor
}  // namespace perfetto
