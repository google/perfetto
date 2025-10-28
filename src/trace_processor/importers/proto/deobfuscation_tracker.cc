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

#include "src/trace_processor/importers/proto/deobfuscation_tracker.h"

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "protos/perfetto/trace/profiling/deobfuscation.pbzero.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/proto/heap_graph_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/profiler_util.h"

namespace perfetto::trace_processor {
namespace {
using ::perfetto::protos::pbzero::DeobfuscationMapping;
using ::perfetto::protos::pbzero::ObfuscatedClass;
using ::perfetto::protos::pbzero::ObfuscatedMember;
using ::protozero::ConstBytes;

using JavaFrameMap = base::
    FlatHashMap<NameInPackage, base::FlatSet<FrameId>, NameInPackage::Hasher>;

std::vector<FrameId> JavaFramesForName(const JavaFrameMap& java_frames_for_name,
                                       NameInPackage name) {
  if (const auto* frames = java_frames_for_name.Find(name); frames) {
    return {frames->begin(), frames->end()};
  }
  return {};
}

}  // namespace

DeobfuscationTracker::DeobfuscationTracker(TraceProcessorContext* context)
    : context_(context) {}

DeobfuscationTracker::~DeobfuscationTracker() = default;

void DeobfuscationTracker::BuildJavaFrameMaps(
    JavaFrameMap& java_frames_for_name,
    std::unordered_set<FrameId>& frames_needing_package_guess) {
  // Iterate over all frames in the table (names are now finalized)
  const auto& frame_table = context_->storage->stack_profile_frame_table();
  const auto& mapping_table = context_->storage->stack_profile_mapping_table();

  for (auto frame_it = frame_table.IterateRows(); frame_it; ++frame_it) {
    const FrameId frame_id = frame_it.id();
    const StringId name_id = frame_it.name();
    const base::StringView function_name =
        context_->storage->GetString(name_id);

    // Only process Java frames (must contain '.')
    if (function_name.find('.') == base::StringView::npos) {
      continue;
    }

    // Extract package from mapping
    const MappingId mapping_id = frame_it.mapping();
    const auto mapping = mapping_table.FindById(mapping_id);
    const base::StringView mapping_name =
        context_->storage->GetString(mapping->name());

    std::optional<std::string> package =
        PackageFromLocation(context_->storage.get(), mapping_name);

    if (package) {
      // Found package from mapping path
      StringId package_id =
          context_->storage->InternString(base::StringView(*package));
      NameInPackage nip{name_id, package_id};
      java_frames_for_name[nip].insert(frame_id);
    } else if (mapping_name.find("/memfd:") == 0) {
      // Special case: memfd mappings
      StringId memfd_id = context_->storage->InternString("memfd");
      NameInPackage nip{name_id, memfd_id};
      java_frames_for_name[nip].insert(frame_id);
    } else {
      // Package unknown - will need guessing from process info
      frames_needing_package_guess.insert(frame_id);
    }
  }
}

void DeobfuscationTracker::AddDeobfuscationMapping(ConstBytes blob) {
  packets_.emplace_back(TraceBlob::CopyFrom(blob.data, blob.size));
}

void DeobfuscationTracker::NotifyEndOfFile() {
  // Maps (name, package) -> set of FrameIds for deobfuscation
  JavaFrameMap java_frames_for_name;

  // Frames needing package guessing (temporary during EOF processing)
  std::unordered_set<FrameId> frames_needing_package_guess;

  // Step 1: Build Java frame maps from complete frame table
  BuildJavaFrameMaps(java_frames_for_name, frames_needing_package_guess);

  // Step 2: Guess packages for frames that couldn't be determined from mappings
  if (!frames_needing_package_guess.empty()) {
    GuessPackages(java_frames_for_name, frames_needing_package_guess);
  }

  // Step 3: Perform deobfuscation using the built maps
  for (const auto& packet : packets_) {
    DeobfuscationMapping::Decoder mapping(packet.data(), packet.size());
    DeobfuscateProfiles(java_frames_for_name, mapping);
    DeobfuscateHeapGraph(mapping);
  }
}

void DeobfuscationTracker::DeobfuscateProfiles(
    const JavaFrameMap& java_frames_for_name,
    const DeobfuscationMapping::Decoder& deobfuscation_mapping) {
  if (deobfuscation_mapping.package_name().size == 0)
    return;

  auto opt_package_name_id = context_->storage->string_pool().GetId(
      deobfuscation_mapping.package_name());
  auto opt_memfd_id = context_->storage->string_pool().GetId("memfd");
  if (!opt_package_name_id && !opt_memfd_id)
    return;

  for (auto class_it = deobfuscation_mapping.obfuscated_classes(); class_it;
       ++class_it) {
    ObfuscatedClass::Decoder cls(*class_it);

    for (auto member_it = cls.obfuscated_methods(); member_it; ++member_it) {
      ObfuscatedMember::Decoder member(*member_it);

      std::string merged_obfuscated = cls.obfuscated_name().ToStdString() +
                                      "." +
                                      member.obfuscated_name().ToStdString();
      auto merged_obfuscated_id = context_->storage->string_pool().GetId(
          base::StringView(merged_obfuscated));
      if (!merged_obfuscated_id)
        continue;

      std::string merged_deobfuscated =
          FullyQualifiedDeobfuscatedName(cls, member);

      std::vector<tables::StackProfileFrameTable::Id> frames;
      if (opt_package_name_id) {
        const std::vector<tables::StackProfileFrameTable::Id> pkg_frames =
            JavaFramesForName(java_frames_for_name,
                              {*merged_obfuscated_id, *opt_package_name_id});
        frames.insert(frames.end(), pkg_frames.begin(), pkg_frames.end());
      }
      if (opt_memfd_id) {
        const std::vector<tables::StackProfileFrameTable::Id> memfd_frames =
            JavaFramesForName(java_frames_for_name,
                              {*merged_obfuscated_id, *opt_memfd_id});
        frames.insert(frames.end(), memfd_frames.begin(), memfd_frames.end());
      }

      for (tables::StackProfileFrameTable::Id frame_id : frames) {
        auto* frames_tbl =
            context_->storage->mutable_stack_profile_frame_table();
        auto rr = *frames_tbl->FindById(frame_id);
        rr.set_deobfuscated_name(context_->storage->InternString(
            base::StringView(merged_deobfuscated)));
      }
    }
  }
}

void DeobfuscationTracker::DeobfuscateHeapGraph(
    const DeobfuscationMapping::Decoder& deobfuscation_mapping) {
  using ReferenceTable = tables::HeapGraphReferenceTable;

  auto* heap_graph_tracker = HeapGraphTracker::Get(context_);

  std::optional<StringId> package_name_id;
  if (deobfuscation_mapping.package_name().size > 0) {
    package_name_id = context_->storage->string_pool().GetId(
        deobfuscation_mapping.package_name());
  }

  auto* reference_table =
      context_->storage->mutable_heap_graph_reference_table();
  for (auto class_it = deobfuscation_mapping.obfuscated_classes(); class_it;
       ++class_it) {
    ObfuscatedClass::Decoder cls(*class_it);
    auto obfuscated_class_name_id =
        context_->storage->string_pool().GetId(cls.obfuscated_name());
    if (!obfuscated_class_name_id) {
      PERFETTO_DLOG("Class string %s not found",
                    cls.obfuscated_name().ToStdString().c_str());
    } else {
      // Deobfuscate heap graph classes
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
      ObfuscatedMember::Decoder member(*member_it);

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

void DeobfuscationTracker::DeobfuscateHeapGraphClass(
    std::optional<StringId> package_name_id,
    StringId obfuscated_class_name_id,
    const ObfuscatedClass::Decoder& cls) {
  using ClassTable = tables::HeapGraphClassTable;

  auto* heap_graph_tracker = HeapGraphTracker::Get(context_);
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

void DeobfuscationTracker::GuessPackageForCallsite(
    JavaFrameMap& java_frames_for_name,
    tables::ProcessTable::Id upid,
    tables::StackProfileCallsiteTable::Id callsite_id,
    std::unordered_set<FrameId>& frames_needing_package_guess) {
  const auto& process_table = context_->storage->process_table();

  auto process = process_table.FindById(upid);
  if (!process.has_value()) {
    return;
  }

  if (!process->android_appid().has_value()) {
    return;
  }

  // Find package from package_list_table
  std::optional<StringId> package;
  for (auto it = context_->storage->package_list_table().IterateRows(); it;
       ++it) {
    if (it.uid() == *process->android_appid()) {
      package = it.package_name();
      break;
    }
  }

  if (!package.has_value()) {
    return;
  }

  // Walk callsite chain and assign package to frames that need it
  const auto& callsite_table =
      context_->storage->stack_profile_callsite_table();
  auto callsite = callsite_table.FindById(callsite_id);
  while (callsite.has_value()) {
    const FrameId frame_id = callsite->frame_id();

    // Check if this frame needs package guessing
    if (frames_needing_package_guess.count(frame_id) != 0) {
      // Add frame to map with guessed package
      auto frame =
          context_->storage->stack_profile_frame_table().FindById(frame_id);
      NameInPackage nip{frame->name(), *package};
      java_frames_for_name[nip].insert(frame_id);

      // Remove from set (package now known)
      frames_needing_package_guess.erase(frame_id);
    }

    auto parent_id = callsite->parent_id();
    callsite.reset();
    if (parent_id.has_value()) {
      callsite = callsite_table.FindById(*parent_id);
    }
  }
}

void DeobfuscationTracker::GuessPackages(
    JavaFrameMap& java_frames_for_name,
    std::unordered_set<FrameId>& frames_needing_package_guess) {
  const auto& heap_profile_allocation_table =
      context_->storage->heap_profile_allocation_table();
  for (auto allocation = heap_profile_allocation_table.IterateRows();
       allocation; ++allocation) {
    auto upid = tables::ProcessTable::Id(allocation.upid());
    auto callsite_id = allocation.callsite_id();

    GuessPackageForCallsite(java_frames_for_name, upid, callsite_id,
                            frames_needing_package_guess);
  }

  const auto& perf_sample_table = context_->storage->perf_sample_table();
  for (auto sample = perf_sample_table.IterateRows(); sample; ++sample) {
    auto thread = context_->storage->thread_table().FindById(
        tables::ThreadTable::Id(sample.utid()));
    if (!thread || !thread->upid().has_value() ||
        !sample.callsite_id().has_value()) {
      continue;
    }
    GuessPackageForCallsite(
        java_frames_for_name, tables::ProcessTable::Id(*thread->upid()),
        *sample.callsite_id(), frames_needing_package_guess);
  }
}

}  // namespace perfetto::trace_processor
