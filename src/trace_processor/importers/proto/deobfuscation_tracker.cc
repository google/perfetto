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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
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
#include "src/trace_processor/core/dataframe/specs.h"
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

// Returns true if `line` falls within the optional range [start, end].
// Missing bounds are treated as unbounded (always match).
bool LineInRange(uint32_t line,
                 std::optional<uint32_t> start,
                 std::optional<uint32_t> end) {
  if (start.has_value() && line < *start) {
    return false;
  }
  if (end.has_value() && line > *end) {
    return false;
  }
  return true;
}

std::vector<FrameId> JavaFramesForName(const JavaFrameMap& java_frames_for_name,
                                       NameInPackage name) {
  if (const auto* frames = java_frames_for_name.Find(name); frames) {
    return {frames->begin(), frames->end()};
  }
  return {};
}

/// In-memory tree for a merged class hierarchy. Parsed once per
// ObfuscatedClass to avoid redundant protobuf decoding for each object.
struct ParsedMergedClassNode {
  base::StringView name;

  // Field used to disambiguate children. Optional for unconditioned merges.
  std::optional<StringId> class_id_field_name;

  base::FlatHashMap<int32_t, std::unique_ptr<ParsedMergedClassNode>>
      discriminator_id_to_child;
  // Children with no non-zero discriminator IDs.
  std::vector<std::unique_ptr<ParsedMergedClassNode>> no_id_children;

  bool HasChildren() const {
    return discriminator_id_to_child.size() != 0 || !no_id_children.empty();
  }

  // Recursively decodes a serialized MergedClasses proto into an in-memory
  // tree. Children with non-zero discriminator IDs are indexed in
  // `discriminator_id_to_child` for fast lookup. Children with discriminator ID
  // 0 or no ID are stored in `no_id_children`. Note that 0 and no ID are
  // identical since there is no way to tell if a field was explicitly set to 0
  // or just uninitialized.
  static ParsedMergedClassNode Parse(protozero::ConstBytes merged_cls_bytes,
                                     TraceStorage& storage,
                                     base::StringView owner_name) {
    ObfuscatedClass::MergedClasses::Decoder mcs(merged_cls_bytes);
    ParsedMergedClassNode node;
    node.name = owner_name;
    if (mcs.has_class_id_field_name()) {
      node.class_id_field_name =
          storage.InternString(mcs.class_id_field_name());
    }
    for (auto it = mcs.merged_classes(); it; ++it) {
      ObfuscatedClass::MergedClass::Decoder mc(*it);
      base::StringView child_name =
          mc.has_name() ? mc.name() : base::StringView();
      auto child_node = mc.has_merged_classes()
                            ? std::make_unique<ParsedMergedClassNode>(Parse(
                                  mc.merged_classes(), storage, child_name))
                            : std::make_unique<ParsedMergedClassNode>();
      child_node->name = child_name;
      if (mc.has_class_id() && mc.class_id() != 0) {
        node.discriminator_id_to_child.Insert(mc.class_id(),
                                              std::move(child_node));
      } else {
        node.no_id_children.push_back(std::move(child_node));
      }
    }
    return node;
  }

  // Disambiguates which class an object belongs to by matching its non-zero
  // discriminators (e.g. $cid, $cid2) against the tree. Returns the resolved
  // class name on a unique match, or std::nullopt if ambiguous or unknown.
  std::optional<base::StringView> Resolve(
      const HeapGraphTracker::DiscriminatorList& discriminators) const {
    size_t non_zero_total = 0;
    for (const auto& [unused_field_name, value] : discriminators) {
      if (value != 0) {
        ++non_zero_total;
      }
    }

    return ResolveInternal(discriminators, non_zero_total, 0);
  }

 private:
  // Recursively walks the tree matching non-zero discriminators:
  // - Once all non-zero discriminators are matched, returns this node's name if
  //   it is a leaf, or if it uniquely represents the owner class (a single
  //   matching no-id child). Otherwise returns std::nullopt (ambiguous).
  // - If class_id_field_name matches a discriminator value, recurses into that
  //   child; otherwise searches no_id_children branches.
  std::optional<base::StringView> ResolveInternal(
      const HeapGraphTracker::DiscriminatorList& discriminators,
      size_t non_zero_total,
      size_t matched_non_zero_count) const {
    if (matched_non_zero_count == non_zero_total) {
      // If there are no children, this leaf node is the unique match.
      if (!HasChildren()) {
        return std::make_optional(name);
      }
      // When one merged_classes owns another, typically the first element
      // of that one has the same name as the owner class. If an object has
      // no non-zero discriminators, we only return a resolution if there is a
      // single child with no discriminator id and its name matches the owner
      // class name. Otherwise, it is ambiguous.
      if (no_id_children.size() == 1 && no_id_children[0]->name == name) {
        return std::make_optional(name);
      }
      return std::nullopt;
    }
    if (!HasChildren()) {
      // matched_non_zero_count != non_zero_total shouldn't generally happen for
      // a well formed map file and program.
      return std::nullopt;
    }

    if (class_id_field_name.has_value()) {
      // There should never be more than a couple discriminators;
      // linear search is sufficient.
      for (const auto& [field_name, value] : discriminators) {
        if (field_name != *class_id_field_name) {
          continue;
        }
        if (value == 0) {
          break;
        }
        auto* child = discriminator_id_to_child.Find(value);
        if (child != nullptr) {
          return (*child)->ResolveInternal(discriminators, non_zero_total,
                                           matched_non_zero_count + 1);
        }
        return std::nullopt;
      }
    }
    // Discriminator was 0 or unset. Search zero/no-id branches.
    for (const auto& child : no_id_children) {
      auto res = child->ResolveInternal(discriminators, non_zero_total,
                                        matched_non_zero_count);
      if (res.has_value()) {
        return res;
      }
    }
    return std::nullopt;
  }
};

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
    const auto mapping = mapping_table[mapping_id];
    const base::StringView mapping_name =
        context_->storage->GetString(mapping.name());

    std::optional<std::string> package =
        PackageFromLocation(context_->global_stats_tracker.get(), mapping_name);

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

void DeobfuscationTracker::OnEventsFullyExtracted() {
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

  HeapGraphTracker::Get(context_)->ClearDisambiguatedObjects();
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

  // Collect all method mappings with line info for inline support.
  // Key: merged_obfuscated_id (e.g., "a.b") -> vector of mappings
  struct MethodMappingInfo {
    StringId deobfuscated_name;
    std::optional<uint32_t> obfuscated_line_start;
    std::optional<uint32_t> obfuscated_line_end;
    std::optional<uint32_t> source_line_start;
  };
  base::FlatHashMap<StringId, std::vector<MethodMappingInfo>> method_mappings;

  for (auto class_it = deobfuscation_mapping.obfuscated_classes(); class_it;
       ++class_it) {
    ObfuscatedClass::Decoder cls(*class_it);
    for (auto member_it = cls.obfuscated_methods(); member_it; ++member_it) {
      ObfuscatedMember::Decoder member(*member_it);

      std::string merged_obfuscated = cls.obfuscated_name().ToStdString() +
                                      "." +
                                      member.obfuscated_name().ToStdString();
      StringId merged_obfuscated_id =
          context_->storage->InternString(base::StringView(merged_obfuscated));

      std::string merged_deobfuscated =
          FullyQualifiedDeobfuscatedName(cls, member);
      StringId deobfuscated_id = context_->storage->InternString(
          base::StringView(merged_deobfuscated));

      MethodMappingInfo info;
      info.deobfuscated_name = deobfuscated_id;
      if (member.has_obfuscated_line_start()) {
        info.obfuscated_line_start = member.obfuscated_line_start();
      }
      if (member.has_obfuscated_line_end()) {
        info.obfuscated_line_end = member.obfuscated_line_end();
      }
      if (member.has_source_line_start()) {
        info.source_line_start = member.source_line_start();
      }
      method_mappings[merged_obfuscated_id].push_back(info);
    }
  }

  auto symbol_cursor = context_->storage->symbol_table().CreateCursor({
      dataframe::FilterSpec{
          tables::SymbolTable::ColumnIndex::symbol_set_id,
          0,
          dataframe::Eq{},
          {},
      },
      dataframe::FilterSpec{
          tables::SymbolTable::ColumnIndex::line_number,
          1,
          dataframe::IsNotNull{},
          {},
      },
  });
  // Deobfuscate frames using the collected mappings.
  auto* frames_tbl = context_->storage->mutable_stack_profile_frame_table();
  for (auto it = method_mappings.GetIterator(); it; ++it) {
    StringId merged_obfuscated_id = it.key();
    const auto& mappings = it.value();

    // Look up frames with this obfuscated name.
    std::vector<tables::StackProfileFrameTable::Id> frames;
    if (opt_package_name_id) {
      for (FrameId fid :
           JavaFramesForName(java_frames_for_name,
                             {merged_obfuscated_id, *opt_package_name_id})) {
        frames.push_back(fid);
      }
    }
    if (opt_memfd_id) {
      for (FrameId fid : JavaFramesForName(
               java_frames_for_name, {merged_obfuscated_id, *opt_memfd_id})) {
        frames.push_back(fid);
      }
    }

    for (tables::StackProfileFrameTable::Id frame_id : frames) {
      auto frame = (*frames_tbl)[frame_id];

      // Try to get line number from existing symbol entry. Note that the
      // symbol table is not just populated during symbolization, it's also
      // populated by simpleperf, pprof, V8 JIT inside the trace itself.
      std::optional<uint32_t> obfuscated_line;
      if (frame.symbol_set_id().has_value()) {
        symbol_cursor.SetFilterValueUnchecked(0, *frame.symbol_set_id());
        symbol_cursor.Execute();
        if (!symbol_cursor.Eof()) {
          obfuscated_line = symbol_cursor.line_number();
        }
      }

      // Find mappings matching this line number (forms the inline chain).
      std::vector<const MethodMappingInfo*> chain;
      if (obfuscated_line.has_value()) {
        for (const auto& info : mappings) {
          if (LineInRange(*obfuscated_line, info.obfuscated_line_start,
                          info.obfuscated_line_end)) {
            chain.push_back(&info);
          }
        }
      }

      if (!chain.empty()) {
        // Create symbol entries for the deobfuscated inline chain.
        auto* symbol_tbl = context_->storage->mutable_symbol_table();
        uint32_t new_symbol_set_id =
            context_->storage->symbol_table().row_count();

        for (size_t i = 0; i < chain.size(); ++i) {
          symbol_tbl->Insert({new_symbol_set_id, chain[i]->deobfuscated_name,
                              kNullStringId,  // source_file
                              chain[i]->source_line_start,
                              (i < chain.size() - 1)});  // inlined
        }

        auto rr = (*frames_tbl)[frame_id];
        rr.set_symbol_set_id(new_symbol_set_id);
        rr.set_deobfuscated_name(chain.back()->deobfuscated_name);
      } else {
        // Fallback: check if all mappings resolve to the same name.
        // If not, mark as ambiguous following existing convention.
        auto rr = (*frames_tbl)[frame_id];

        // Collect unique deobfuscated names (sorted for deterministic output).
        std::set<std::string> unique_names;
        for (const auto& m : mappings) {
          unique_names.insert(
              context_->storage->GetString(m.deobfuscated_name).ToStdString());
        }

        if (unique_names.size() == 1) {
          // All mappings agree on the same name.
          rr.set_deobfuscated_name(mappings.front().deobfuscated_name);
        } else {
          // Ambiguous: multiple distinct names, can't disambiguate without
          // line number. Build "Name1 | Name2" string following existing
          // convention from FlattenClasses() in deobfuscator.cc.
          std::string ambiguous_str;
          bool first = true;
          for (const std::string& name : unique_names) {
            if (!first) {
              ambiguous_str += " | ";
            }
            ambiguous_str += name;
            first = false;
          }
          rr.set_deobfuscated_name(
              context_->storage->InternString(base::StringView(ambiguous_str)));
        }
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
  if (!cls_objects) {
    PERFETTO_DLOG("Class %s not found",
                  cls.obfuscated_name().ToStdString().c_str());
    return;
  }

  std::optional<ParsedMergedClassNode> parsed_tree;
  if (cls.has_merged_classes()) {
    base::StringView owner_name = cls.has_deobfuscated_name()
                                      ? cls.deobfuscated_name()
                                      : base::StringView();
    parsed_tree = ParsedMergedClassNode::Parse(cls.merged_classes(),
                                               *context_->storage, owner_name);
  }

  auto* class_table = context_->storage->mutable_heap_graph_class_table();
  auto* object_table = context_->storage->mutable_heap_graph_object_table();
  for (ClassTable::RowNumber class_row_num : *cls_objects) {
    auto class_ref = class_row_num.ToRowReference(class_table);

    const StringId obfuscated_type_name_id = class_ref.name();
    const base::StringView obfuscated_type_name =
        context_->storage->GetString(obfuscated_type_name_id);
    NormalizedType normalized_type = GetNormalizedType(obfuscated_type_name);
    std::string base_deobfuscated_type_name =
        DenormalizeTypeName(normalized_type, cls.deobfuscated_name());
    StringId base_deobfuscated_type_name_id = context_->storage->InternString(
        base::StringView(base_deobfuscated_type_name));
    class_ref.set_deobfuscated_name(base_deobfuscated_type_name_id);

    if (parsed_tree.has_value()) {
      const auto* disambiguated_objects =
          heap_graph_tracker->ObjectsForMergedClass(class_ref.id());
      if (disambiguated_objects != nullptr) {
        base::FlatHashMap<base::StringView, ClassTable::Id>
            class_name_to_class_row;
        if (cls.has_deobfuscated_name()) {
          class_name_to_class_row.Insert(cls.deobfuscated_name(),
                                         class_ref.id());
        }
        for (const auto& disambiguated_obj : *disambiguated_objects) {
          auto obj_ref =
              disambiguated_obj.row_number.ToRowReference(object_table);

          std::optional<base::StringView> true_class_name =
              parsed_tree->Resolve(disambiguated_obj.discriminators);

          if (true_class_name.has_value()) {
            ClassTable::Id target_class_id;
            auto* cached_id = class_name_to_class_row.Find(*true_class_name);
            if (cached_id) {
              target_class_id = *cached_id;
            } else {
              std::string deobfuscated_type_name =
                  DenormalizeTypeName(normalized_type, *true_class_name);
              StringId deobfuscated_type_name_id =
                  context_->storage->InternString(
                      base::StringView(deobfuscated_type_name));

              ClassTable::Row new_class_row;
              new_class_row.name = class_ref.name();
              new_class_row.deobfuscated_name = deobfuscated_type_name_id;
              new_class_row.location = class_ref.location();
              new_class_row.superclass_id = class_ref.superclass_id();
              new_class_row.classloader_id = class_ref.classloader_id();
              new_class_row.kind = class_ref.kind();
              target_class_id = class_table->Insert(new_class_row).id;
              class_name_to_class_row.Insert(*true_class_name, target_class_id);
            }
            obj_ref.set_type_id(target_class_id);
          }
        }
      }
    }
  }
}

void DeobfuscationTracker::GuessPackageForCallsite(
    JavaFrameMap& java_frames_for_name,
    tables::ProcessTable::Id upid,
    tables::StackProfileCallsiteTable::Id callsite_id,
    std::unordered_set<FrameId>& frames_needing_package_guess) {
  const auto& process_table = context_->storage->process_table();

  auto process = process_table[upid];

  if (!process.android_appid().has_value()) {
    return;
  }

  // Find package from package_list_table
  std::optional<StringId> package;
  for (auto it = context_->storage->package_list_table().IterateRows(); it;
       ++it) {
    if (it.uid() == *process.android_appid()) {
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
  std::optional<tables::StackProfileCallsiteTable::Id> current_id = callsite_id;
  while (current_id.has_value()) {
    auto callsite = callsite_table[*current_id];
    const FrameId frame_id = callsite.frame_id();

    // Check if this frame needs package guessing
    if (frames_needing_package_guess.count(frame_id) != 0) {
      // Add frame to map with guessed package
      auto frame = context_->storage->stack_profile_frame_table()[frame_id];
      NameInPackage nip{frame.name(), *package};
      java_frames_for_name[nip].insert(frame_id);

      // Remove from set (package now known)
      frames_needing_package_guess.erase(frame_id);
    }

    current_id = callsite.parent_id();
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

  const auto& profiler_sample_table =
      context_->storage->profiler_sample_table();
  const auto& task_context_table =
      context_->storage->profiler_task_context_table();
  for (auto sample = profiler_sample_table.IterateRows(); sample; ++sample) {
    if (!sample.task_context_id() || !sample.callsite_id()) {
      continue;
    }
    auto task_context = task_context_table[*sample.task_context_id()];
    if (!task_context.upid()) {
      continue;
    }
    GuessPackageForCallsite(
        java_frames_for_name, tables::ProcessTable::Id(*task_context.upid()),
        *sample.callsite_id(), frames_needing_package_guess);
  }

  const auto& callsite_table =
      context_->storage->heap_graph_thread_callsite_table();
  const auto& heap_graph_table = context_->storage->heap_graph_table();
  for (auto it = callsite_table.IterateRows(); it; ++it) {
    auto heap_graph_id = it.heap_graph_id();
    auto upid_val = heap_graph_table[heap_graph_id].upid();
    auto upid = tables::ProcessTable::Id(upid_val);
    auto callsite_id = it.callsite_id();
    if (!callsite_id.has_value()) {
      continue;
    }
    GuessPackageForCallsite(java_frames_for_name, upid, *callsite_id,
                            frames_needing_package_guess);
  }
}

}  // namespace perfetto::trace_processor
