/*
 * Copyright (C) 202 The Android Open Source Project
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

#include "src/trace_processor/importers/art_hprof/art_hprof_event.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"


namespace perfetto::trace_processor::art_hprof {

ArtHprofParserImpl::ArtHprofParserImpl(TraceProcessorContext* context)
    : context_(context) {}

ArtHprofParserImpl::~ArtHprofParserImpl() = default;

void ArtHprofParserImpl::ParseArtHprofEvent(int64_t ts, ArtHprofEvent e) {
  const HeapGraphIR& ir = e.data;
  UniquePid upid = context_->process_tracker->GetOrCreateProcess(e.pid);

  // Process all classes
  std::unordered_map<uint64_t, tables::HeapGraphClassTable::Id> class_object_id_to_id;
  for (const auto& cls : ir.classes) {
    StringId name_id = context_->storage->InternString(base::StringView(cls.name));
    StringId deobfuscated_name_id = cls.deobfuscated_name->empty() ?
        kNullStringId : context_->storage->InternString(base::StringView(*(cls.deobfuscated_name)));
    StringId location_id = cls.location->empty() ?
        kNullStringId : context_->storage->InternString(base::StringView(*(cls.location)));
    StringId kind_id = context_->storage->InternString(base::StringView(cls.kind));

    // Initially set superclass_id to null, we'll update it in second pass
    tables::HeapGraphClassTable::Row class_row(
        name_id,
        deobfuscated_name_id == kNullStringId ? std::nullopt : std::make_optional(deobfuscated_name_id),
        location_id == kNullStringId ? std::nullopt : std::make_optional(location_id),
        std::nullopt,  // superclass_id - will update in second pass
        cls.classloader_id,
        kind_id
    );

    tables::HeapGraphClassTable::Id class_id =
        context_->storage->mutable_heap_graph_class_table()->Insert(class_row).id;
    class_object_id_to_id[cls.class_object_id] = class_id;

    // Second pass to update superclass_id references now that all classes are created
    // for (const auto& cls : ir.classes) {
    //     if (cls.superclass_id.has_value()) {
    //         auto it = class_object_id_to_id.find(*cls.superclass_id);
    //         if (it != class_object_id_to_id.end()) {
    //             // Update the superclass_id field
    //             auto* class_table = context_->storage->mutable_heap_graph_class_table();
    //             tables::HeapGraphClassTable::Id class_id = class_object_id_to_id[cls.class_object_id];
    //             class_table->mutable_superclass_id()->Set(class_id.value, it->second);
    //         }
    //     }
    // }
  }

  // Process all objects
  std::unordered_map<uint64_t, tables::HeapGraphObjectTable::Id> object_id_to_id;
  for (const auto& obj : ir.objects) {
    auto type_id = class_object_id_to_id[obj.type_id];

    // StringId root_type_id = obj.root_type.has_value() ?
    //     context_->storage->InternString(base::StringView(*obj.root_type)) : kNullStringId;
    StringId root_type_id = kNullStringId;

    StringId heap_type_id = obj.heap_type.has_value() ?
        context_->storage->InternString(base::StringView(*obj.heap_type)) : kNullStringId;

    tables::HeapGraphObjectTable::Row object_row{
        upid,
        ts,
        static_cast<int64_t>(obj.self_size),
        0, //static_cast<int64_t>(obj.native_size),
        obj.reference_set_id,
        1,// obj.reachable ? 1 : 0,
        heap_type_id == kNullStringId ? std::nullopt : std::make_optional(heap_type_id),
        type_id,
        root_type_id == kNullStringId ? std::nullopt : std::make_optional(root_type_id),
        0, //static_cast<int32_t>(obj.root_distance)
    };

    tables::HeapGraphObjectTable::Id object_id =
        context_->storage->mutable_heap_graph_object_table()->Insert(object_row).id;
    object_id_to_id[obj.object_id] = object_id;
  }

  // Process all references
  for (const auto& ref : ir.references) {
    auto owner_id = object_id_to_id[ref.owner_id];
    std::optional<tables::HeapGraphObjectTable::Id> owned_id;

    if (ref.owned_id.has_value()) {
      owned_id = object_id_to_id[*ref.owned_id];
    }

    StringId field_name_id = context_->storage->InternString(base::StringView(ref.field_name));
    StringId field_type_name_id = context_->storage->InternString(base::StringView(ref.field_type_name));

    // StringId deobfuscated_field_name_id = ref.deobfuscated_field_name.empty() ?
    //     kNullStringId : context_->storage->InternString(base::StringView(ref.deobfuscated_field_name));
    StringId deobfuscated_field_name_id = kNullStringId;

    tables::HeapGraphReferenceTable::Row reference_row{
        ref.reference_set_id,
        owner_id,
        owned_id,
        field_name_id,
        field_type_name_id,
        deobfuscated_field_name_id == kNullStringId ? std::nullopt :
        std::make_optional(deobfuscated_field_name_id)
    };

    context_->storage->mutable_heap_graph_reference_table()->Insert(reference_row);
  }
}
}  // namespace perfetto::trace_processor::art_hprof
