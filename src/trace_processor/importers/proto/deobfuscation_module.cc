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

#include "src/trace_processor/importers/proto/deobfuscation_module.h"

#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/profiling/deobfuscation.pbzero.h"
#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/profiler_util.h"

namespace perfetto::trace_processor {

using ::perfetto::protos::pbzero::TracePacket;
using ::protozero::ConstBytes;

DeobfuscationModule::DeobfuscationModule(TraceProcessorContext* context)
    : context_(context) {
  // note: deobfuscation mappings also handled by ProfileModule.
  RegisterForField(TracePacket::kDeobfuscationMappingFieldNumber, context);
}

DeobfuscationModule::~DeobfuscationModule() = default;

void DeobfuscationModule::ParseTracePacketData(
    const TracePacket::Decoder& decoder,
    int64_t,
    const TracePacketData&,
    uint32_t field_id) {
  switch (field_id) {
    case TracePacket::kDeobfuscationMappingFieldNumber: {
      ParseDeobfuscationMapping(decoder.deobfuscation_mapping());
      return;
    }
    default:
      break;
  }
}

void DeobfuscationModule::DeobfuscateHeapGraphClass(
    std::optional<StringId> package_name_id,
    StringId obfuscated_class_name_id,
    const protos::pbzero::ObfuscatedClass::Decoder& cls) {
  using ClassTable = tables::HeapGraphClassTable;

  auto* heap_graph_tracker = HeapGraphTracker::GetOrCreate(context_);
  const std::vector<ClassTable::RowNumber>* cls_objects =
      heap_graph_tracker->RowsForType(package_name_id,
                                      obfuscated_class_name_id);
  if (cls_objects) {
    auto* class_table = context_->storage->mutable_heap_graph_class_table();
    for (ClassTable::RowNumber class_row_num : *cls_objects) {
      auto class_ref = class_row_num.ToRowReference(class_table);
      const StringId obfuscated_type_name_id = class_ref.name();
      const base::StringView obfuscated_type_name =
          context_->storage->GetString(obfuscated_type_name_id);
      NormalizedType normalized_type = GetNormalizedType(obfuscated_type_name);
      std::string deobfuscated_type_name =
          DenormalizeTypeName(normalized_type, cls.deobfuscated_name());
      StringId deobfuscated_type_name_id = context_->storage->InternString(
          base::StringView(deobfuscated_type_name));
      class_ref.set_deobfuscated_name(deobfuscated_type_name_id);
    }
  } else {
    PERFETTO_DLOG("Class %s not found",
                  cls.obfuscated_name().ToStdString().c_str());
  }
}

void DeobfuscationModule::ParseDeobfuscationMapping(ConstBytes blob) {
  auto* heap_graph_tracker = HeapGraphTracker::GetOrCreate(context_);
  heap_graph_tracker->FinalizeAllProfiles();

  protos::pbzero::DeobfuscationMapping::Decoder deobfuscation_mapping(
      blob.data, blob.size);
  ParseDeobfuscationMappingForHeapGraph(deobfuscation_mapping,
                                        heap_graph_tracker);
}

void DeobfuscationModule::ParseDeobfuscationMappingForHeapGraph(
    const protos::pbzero::DeobfuscationMapping::Decoder& deobfuscation_mapping,
    HeapGraphTracker* heap_graph_tracker) {
  using ReferenceTable = tables::HeapGraphReferenceTable;

  std::optional<StringId> package_name_id;
  if (deobfuscation_mapping.package_name().size > 0) {
    package_name_id = context_->storage->string_pool().GetId(
        deobfuscation_mapping.package_name());
  }

  auto* reference_table =
      context_->storage->mutable_heap_graph_reference_table();
  for (auto class_it = deobfuscation_mapping.obfuscated_classes(); class_it;
       ++class_it) {
    protos::pbzero::ObfuscatedClass::Decoder cls(*class_it);
    auto obfuscated_class_name_id =
        context_->storage->string_pool().GetId(cls.obfuscated_name());
    if (!obfuscated_class_name_id) {
      PERFETTO_DLOG("Class string %s not found",
                    cls.obfuscated_name().ToStdString().c_str());
    } else {
      // TODO(b/153552977): Remove this work-around for legacy traces.
      // For traces without location information, deobfuscate all matching
      // classes.
      DeobfuscateHeapGraphClass(std::nullopt, *obfuscated_class_name_id, cls);
      if (package_name_id) {
        DeobfuscateHeapGraphClass(package_name_id, *obfuscated_class_name_id,
                                  cls);
      }
    }
    for (auto member_it = cls.obfuscated_members(); member_it; ++member_it) {
      protos::pbzero::ObfuscatedMember::Decoder member(*member_it);

      std::string merged_obfuscated = cls.obfuscated_name().ToStdString() +
                                      "." +
                                      member.obfuscated_name().ToStdString();
      std::string merged_deobfuscated =
          FullyQualifiedDeobfuscatedName(cls, member);

      auto obfuscated_field_name_id = context_->storage->string_pool().GetId(
          base::StringView(merged_obfuscated));
      if (!obfuscated_field_name_id) {
        PERFETTO_DLOG("Field string %s not found", merged_obfuscated.c_str());
        continue;
      }

      const std::vector<ReferenceTable::RowNumber>* field_references =
          heap_graph_tracker->RowsForField(*obfuscated_field_name_id);
      if (field_references) {
        auto interned_deobfuscated_name = context_->storage->InternString(
            base::StringView(merged_deobfuscated));
        for (ReferenceTable::RowNumber row_number : *field_references) {
          auto row_ref = row_number.ToRowReference(reference_table);
          row_ref.set_deobfuscated_field_name(interned_deobfuscated_name);
        }
      } else {
        PERFETTO_DLOG("Field %s not found", merged_obfuscated.c_str());
      }
    }
  }
}

}  // namespace perfetto::trace_processor
