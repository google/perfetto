/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/dynamic/experimental_annotated_stack_generator.h"

#include "perfetto/ext/base/optional.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "perfetto/ext/base/string_utils.h"

namespace perfetto {
namespace trace_processor {

namespace {

enum class MapType {
  kArtInterp,
  kArtJit,
  kArtAot,
  kNativeLibart,
  kNativeOther,
  kOther
};

// Mapping examples:
//   /system/lib64/libc.so
//   /system/framework/framework.jar
//   /memfd:jit-cache (deleted)
//   [vdso]
// TODO(rsavitski): consider moving this to a hidden column on
// stack_profile_mapping, once this logic is sufficiently stable.
MapType ClassifyMap(NullTermStringView map) {
  if (map.empty())
    return MapType::kOther;

  // Primary mapping where modern ART puts jitted code.
  // TODO(rsavitski): look into /memfd:jit-zygote-cache.
  if (!strncmp(map.c_str(), "/memfd:jit-cache", 16))
    return MapType::kArtJit;

  size_t last_slash_pos = map.rfind('/');
  if (last_slash_pos != NullTermStringView::npos) {
    if (!strncmp(map.c_str() + last_slash_pos, "/libart.so", 10))
      return MapType::kNativeLibart;
    if (!strncmp(map.c_str() + last_slash_pos, "/libartd.so", 11))
      return MapType::kNativeLibart;
  }

  size_t extension_pos = map.rfind('.');
  if (extension_pos != NullTermStringView::npos) {
    if (!strncmp(map.c_str() + extension_pos, ".so", 3))
      return MapType::kNativeOther;
    // dex with verification speedup info, produced by dex2oat
    if (!strncmp(map.c_str() + extension_pos, ".vdex", 5))
      return MapType::kArtInterp;
    // possibly uncompressed dex in a jar archive
    if (!strncmp(map.c_str() + extension_pos, ".jar", 4))
      return MapType::kArtInterp;
    // ahead of time compiled ELFs
    if (!strncmp(map.c_str() + extension_pos, ".oat", 4))
      return MapType::kArtAot;
    // older/alternative name for .oat
    if (!strncmp(map.c_str() + extension_pos, ".odex", 5))
      return MapType::kArtAot;
  }
  return MapType::kOther;
}

uint32_t GetConstraintColumnIndex(TraceProcessorContext* context) {
  // The dynamic table adds two columns on top of the callsite table. Last
  // column is the hidden constrain (i.e. input arg) column.
  return context->storage->stack_profile_callsite_table().GetColumnCount() + 1;
}

}  // namespace

std::string ExperimentalAnnotatedStackGenerator::TableName() {
  return "experimental_annotated_callstack";
}

Table::Schema ExperimentalAnnotatedStackGenerator::CreateSchema() {
  auto schema = tables::StackProfileCallsiteTable::Schema();
  schema.columns.push_back(Table::Schema::Column{
      "annotation", SqlValue::Type::kString, /* is_id = */ false,
      /* is_sorted = */ false, /* is_hidden = */ false, false /* is_set_id */});
  schema.columns.push_back(Table::Schema::Column{
      "start_id", SqlValue::Type::kLong, /* is_id = */ false,
      /* is_sorted = */ false, /* is_hidden = */ true, false /* is_set_id */});
  return schema;
}

base::Status ExperimentalAnnotatedStackGenerator::ValidateConstraints(
    const QueryConstraints& qc) {
  const auto& cs = qc.constraints();
  int column = static_cast<int>(GetConstraintColumnIndex(context_));

  auto id_fn = [column](const QueryConstraints::Constraint& c) {
    return c.column == column && c.op == SQLITE_INDEX_CONSTRAINT_EQ;
  };
  bool has_id_cs = std::find_if(cs.begin(), cs.end(), id_fn) != cs.end();
  return has_id_cs ? base::OkStatus()
                   : base::ErrStatus("Failed to find required constraints");
}

base::Status ExperimentalAnnotatedStackGenerator::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&,
    const BitVector&,
    std::unique_ptr<Table>& table_return) {
  const auto& cs_table = context_->storage->stack_profile_callsite_table();
  const auto& f_table = context_->storage->stack_profile_frame_table();
  const auto& m_table = context_->storage->stack_profile_mapping_table();

  // Input (id of the callsite leaf) is the constraint on the hidden |start_id|
  // column.
  uint32_t constraint_col = GetConstraintColumnIndex(context_);
  auto constraint_it =
      std::find_if(cs.begin(), cs.end(), [constraint_col](const Constraint& c) {
        return c.col_idx == constraint_col && c.op == FilterOp::kEq;
      });
  PERFETTO_DCHECK(constraint_it != cs.end());
  if (constraint_it == cs.end() ||
      constraint_it->value.type != SqlValue::Type::kLong) {
    return base::ErrStatus("invalid input callsite id");
  }

  uint32_t start_id = static_cast<uint32_t>(constraint_it->value.AsLong());
  base::Optional<uint32_t> start_row =
      cs_table.id().IndexOf(CallsiteId(start_id));
  if (!start_row) {
    return base::ErrStatus("callsite with id %" PRIu32 " not found", start_id);
  }

  // Iteratively walk the parent_id chain to construct the list of callstack
  // entries, each pointing at a frame.
  std::vector<uint32_t> cs_rows;
  cs_rows.push_back(*start_row);
  base::Optional<CallsiteId> maybe_parent_id = cs_table.parent_id()[*start_row];
  while (maybe_parent_id) {
    uint32_t parent_row = cs_table.id().IndexOf(*maybe_parent_id).value();
    cs_rows.push_back(parent_row);
    maybe_parent_id = cs_table.parent_id()[parent_row];
  }

  // Walk the callsites root-to-leaf, annotating:
  // * managed frames with their execution state (interpreted/jit/aot)
  // * common ART frames, which are usually not relevant
  //
  // This is not a per-frame decision, because we do not want to filter out ART
  // frames immediately after a JNI transition (such frames are often relevant).
  //
  // As a consequence of the logic being based on a root-to-leaf walk, a given
  // callsite will always have the same annotation, as the parent path is always
  // the same, and children callsites do not affect their parents' annotations.
  //
  // This could also be implemented as a hidden column on the callsite table
  // (populated at import time), but we want to be more flexible for now.
  StringId art_jni_trampoline =
      context_->storage->InternString("art_jni_trampoline");

  StringId common_frame = context_->storage->InternString("common-frame");
  StringId art_interp = context_->storage->InternString("interp");
  StringId art_jit = context_->storage->InternString("jit");
  StringId art_aot = context_->storage->InternString("aot");

  // Annotation FSM states:
  // * kInitial: default, native-only callstacks never leave this state.
  // * kEraseLibart: we've seen a managed frame, and will now "erase" (i.e. tag
  //                 as a common-frame) frames belonging to the ART runtime.
  // * kKeepNext: we've seen a special JNI trampoline for managed->native
  //              transition, keep the immediate child (even if it is in ART),
  //              and then go back to kEraseLibart.
  // Regardless of the state, managed frames get annotated with their execution
  // mode, based on the mapping.
  enum class State { kInitial, kEraseLibart, kKeepNext };
  State annotation_state = State::kInitial;

  std::vector<StringPool::Id> annotations_reversed;
  for (auto it = cs_rows.rbegin(); it != cs_rows.rend(); ++it) {
    FrameId frame_id = cs_table.frame_id()[*it];
    uint32_t frame_row = f_table.id().IndexOf(frame_id).value();

    MappingId map_id = f_table.mapping()[frame_row];
    uint32_t map_row = m_table.id().IndexOf(map_id).value();

    // Keep immediate callee of a JNI trampoline, but keep tagging all
    // successive libart frames as common.
    if (annotation_state == State::kKeepNext) {
      annotations_reversed.push_back(kNullStringId);
      annotation_state = State::kEraseLibart;
      continue;
    }

    // Special-case "art_jni_trampoline" frames, keeping their immediate callee
    // even if it is in libart, as it could be a native implementation of a
    // managed method. Example for "java.lang.reflect.Method.Invoke":
    //   art_jni_trampoline
    //   art::Method_invoke(_JNIEnv*, _jobject*, _jobject*, _jobjectArray*)
    //
    // Simpleperf also relies on this frame name, so it should be fairly stable.
    // TODO(rsavitski): consider detecting standard JNI upcall entrypoints -
    // _JNIEnv::Call*. These are sometimes inlined into other DSOs, so erasing
    // only the libart frames does not clean up all of the JNI-related frames.
    StringId fname_id = f_table.name()[frame_row];
    if (fname_id == art_jni_trampoline) {
      annotations_reversed.push_back(common_frame);
      annotation_state = State::kKeepNext;
      continue;
    }

    NullTermStringView map_view =
        context_->storage->GetString(m_table.name()[map_row]);
    MapType map_type = ClassifyMap(map_view);

    // Annotate managed frames.
    if (map_type == MapType::kArtInterp ||  //
        map_type == MapType::kArtJit ||     //
        map_type == MapType::kArtAot) {
      if (map_type == MapType::kArtInterp)
        annotations_reversed.push_back(art_interp);
      else if (map_type == MapType::kArtJit)
        annotations_reversed.push_back(art_jit);
      else if (map_type == MapType::kArtAot)
        annotations_reversed.push_back(art_aot);

      // Now know to be in a managed callstack - erase subsequent ART frames.
      if (annotation_state == State::kInitial)
        annotation_state = State::kEraseLibart;
      continue;
    }

    if (annotation_state == State::kEraseLibart &&
        map_type == MapType::kNativeLibart) {
      annotations_reversed.push_back(common_frame);
      continue;
    }

    annotations_reversed.push_back(kNullStringId);
  }

  // Build the dynamic table.
  auto base_rowmap = RowMap(std::move(cs_rows));

  PERFETTO_DCHECK(base_rowmap.size() == annotations_reversed.size());
  std::unique_ptr<NullableVector<StringPool::Id>> annotation_vals(
      new NullableVector<StringPool::Id>());
  for (auto it = annotations_reversed.rbegin();
       it != annotations_reversed.rend(); ++it) {
    annotation_vals->Append(*it);
  }

  // Hidden column - always the input, i.e. the callsite leaf.
  std::unique_ptr<NullableVector<uint32_t>> start_id_vals(
      new NullableVector<uint32_t>());
  for (uint32_t i = 0; i < base_rowmap.size(); i++)
    start_id_vals->Append(start_id);

  table_return.reset(new Table(
      cs_table.Apply(std::move(base_rowmap))
          .ExtendWithColumn("annotation", std::move(annotation_vals),
                            TypedColumn<StringPool::Id>::default_flags())
          .ExtendWithColumn("start_id", std::move(start_id_vals),
                            TypedColumn<uint32_t>::default_flags() |
                                TypedColumn<uint32_t>::kHidden)));
  return base::OkStatus();
}

uint32_t ExperimentalAnnotatedStackGenerator::EstimateRowCount() {
  return 1;
}

}  // namespace trace_processor
}  // namespace perfetto
