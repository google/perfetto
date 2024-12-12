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

#include "src/trace_processor/importers/common/track_tracker.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/tracks_internal.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {
namespace {

bool IsLegacyStringIdNameAllowed(tracks::TrackClassification classification) {
  // **DO NOT** add new values here. Use TrackTracker::AutoName instead.
  return classification == tracks::unknown;
}

bool IsLegacyCharArrayNameAllowed(tracks::TrackClassification classification) {
  // **DO NOT** add new values here. Use TrackTracker::AutoName instead.
  return classification == tracks::cpu_funcgraph ||
         classification == tracks::cpu_irq ||
         classification == tracks::cpu_mali_irq ||
         classification == tracks::cpu_napi_gro ||
         classification == tracks::cpu_softirq ||
         classification == tracks::pkvm_hypervisor;
}

}  // namespace

TrackTracker::TrackTracker(TraceProcessorContext* context)
    : source_key_(context->storage->InternString("source")),
      trace_id_key_(context->storage->InternString("trace_id")),
      trace_id_is_process_scoped_key_(
          context->storage->InternString("trace_id_is_process_scoped")),
      source_scope_key_(context->storage->InternString("source_scope")),
      category_key_(context->storage->InternString("category")),
      scope_id_(context->storage->InternString("scope")),
      cookie_id_(context->storage->InternString("cookie")),
      fuchsia_source_(context->storage->InternString("fuchsia")),
      chrome_source_(context->storage->InternString("chrome")),
      utid_id_(context->storage->InternString("utid")),
      upid_id_(context->storage->InternString("upid")),
      cpu_id_(context->storage->InternString("cpu")),
      uid_id_(context->storage->InternString("uid")),
      gpu_id_(context->storage->InternString("gpu")),
      name_id_(context->storage->InternString("name")),
      context_(context),
      args_tracker_(context) {}

TrackId TrackTracker::CreateTrack(tracks::TrackClassification classification,
                                  std::optional<Dimensions> dimensions,
                                  const TrackName& name) {
  tables::TrackTable::Row row(StringIdFromTrackName(classification, name));
  row.classification =
      context_->storage->InternString(tracks::ToString(classification));
  if (dimensions) {
    row.dimension_arg_set_id = dimensions->arg_set_id;
  }
  row.machine_id = context_->machine_id();

  return context_->storage->mutable_track_table()->Insert(row).id;
}

TrackId TrackTracker::CreateProcessTrack(
    tracks::TrackClassification classification,
    UniquePid upid,
    std::optional<Dimensions> dims,
    const TrackName& name) {
  Dimensions dims_id =
      dims ? *dims : SingleDimension(upid_id_, Variadic::Integer(upid));

  tables::ProcessTrackTable::Row row(
      StringIdFromTrackName(classification, name));
  row.upid = upid;
  row.dimension_arg_set_id = dims_id.arg_set_id;
  row.classification =
      context_->storage->InternString(tracks::ToString(classification));
  row.machine_id = context_->machine_id();

  return context_->storage->mutable_process_track_table()->Insert(row).id;
}

TrackId TrackTracker::CreateThreadTrack(
    tracks::TrackClassification classification,
    UniqueTid utid,
    const TrackName& name) {
  Dimensions dims_id = SingleDimension(utid_id_, Variadic::Integer(utid));

  tables::ThreadTrackTable::Row row(
      StringIdFromTrackName(classification, name));
  row.utid = utid;
  row.classification =
      context_->storage->InternString(tracks::ToString(classification));
  row.dimension_arg_set_id = dims_id.arg_set_id;
  row.machine_id = context_->machine_id();

  return context_->storage->mutable_thread_track_table()->Insert(row).id;
}

TrackId TrackTracker::InternProcessTrack(
    tracks::TrackClassification classification,
    UniquePid upid,
    const TrackName& name) {
  Dimensions dims_id = SingleDimension(upid_id_, Variadic::Integer(upid));

  auto* it = tracks_.Find({classification, dims_id});
  if (it)
    return *it;

  TrackId track_id =
      CreateProcessTrack(classification, upid, std::nullopt, name);
  tracks_[{classification, dims_id}] = track_id;
  return track_id;
}

TrackId TrackTracker::InternThreadTrack(UniqueTid utid, const TrackName& name) {
  Dimensions dims = SingleDimension(utid_id_, Variadic::Integer(utid));

  auto* it = tracks_.Find({tracks::thread, dims});
  if (it)
    return *it;
  TrackId track_id = CreateThreadTrack(tracks::thread, utid, name);
  tracks_[{tracks::thread, dims}] = track_id;
  return track_id;
}

TrackId TrackTracker::InternCpuTrack(tracks::TrackClassification classification,
                                     uint32_t cpu,
                                     const TrackName& name) {
  MarkCpuValid(cpu);

  Dimensions dims_id = SingleDimension(cpu_id_, Variadic::Integer(cpu));
  auto* it = tracks_.Find({classification, dims_id});
  if (it) {
    return *it;
  }

  tables::CpuTrackTable::Row row(StringIdFromTrackName(classification, name));
  row.cpu = cpu;
  row.machine_id = context_->machine_id();
  row.classification =
      context_->storage->InternString(tracks::ToString(classification));
  row.dimension_arg_set_id = dims_id.arg_set_id;

  TrackId track_id =
      context_->storage->mutable_cpu_track_table()->Insert(row).id;
  tracks_[{classification, dims_id}] = track_id;
  return track_id;
}

TrackId TrackTracker::LegacyInternLegacyChromeAsyncTrack(
    StringId raw_name,
    uint32_t upid,
    int64_t trace_id,
    bool trace_id_is_process_scoped,
    StringId source_scope) {
  DimensionsBuilder dims_builder = CreateDimensionsBuilder();
  dims_builder.AppendDimension(scope_id_, Variadic::String(source_scope));
  if (trace_id_is_process_scoped) {
    dims_builder.AppendUpid(upid);
  }
  dims_builder.AppendDimension(cookie_id_, Variadic::Integer(trace_id));

  const StringId name =
      context_->process_track_translation_table->TranslateName(raw_name);

  TrackMapKey key;
  key.classification = tracks::unknown;
  key.dimensions = std::move(dims_builder).Build();

  auto* it = tracks_.Find(key);
  if (it) {
    if (name != kNullStringId) {
      // The track may have been created for an end event without name. In
      // that case, update it with this event's name.
      auto& tracks = *context_->storage->mutable_track_table();
      auto rr = *tracks.FindById(*it);
      if (rr.name() == kNullStringId) {
        rr.set_name(name);
      }
    }
    return *it;
  }

  // Legacy async tracks are always drawn in the context of a process, even if
  // the ID's scope is global.
  tables::ProcessTrackTable::Row track(name);
  track.upid = upid;
  track.classification =
      context_->storage->InternString(tracks::ToString(tracks::unknown));
  track.dimension_arg_set_id = key.dimensions->arg_set_id;
  track.machine_id = context_->machine_id();

  TrackId id =
      context_->storage->mutable_process_track_table()->Insert(track).id;
  tracks_[key] = id;

  args_tracker_.AddArgsTo(id)
      .AddArg(source_key_, Variadic::String(chrome_source_))
      .AddArg(trace_id_key_, Variadic::Integer(trace_id))
      .AddArg(trace_id_is_process_scoped_key_,
              Variadic::Boolean(trace_id_is_process_scoped))
      .AddArg(source_scope_key_, Variadic::String(source_scope));
  args_tracker_.Flush();

  return id;
}

StringId TrackTracker::StringIdFromTrackName(
    tracks::TrackClassification classification,
    const TrackTracker::TrackName& name) {
  switch (name.index()) {
    case base::variant_index<TrackName, AutoName>():
      return kNullStringId;
    case base::variant_index<TrackName, LegacyStringIdName>():
      PERFETTO_DCHECK(IsLegacyStringIdNameAllowed(classification));
      return std::get<LegacyStringIdName>(name).id;
    case base::variant_index<TrackName, LegacyCharArrayName>():
      PERFETTO_DCHECK(IsLegacyCharArrayNameAllowed(classification));
      return context_->storage->InternString(
          std::get<LegacyCharArrayName>(name).name);
  }
  PERFETTO_FATAL("For GCC");
}

TrackId TrackTracker::AddTrack(const tracks::BlueprintBase& blueprint,
                               StringId name,
                               StringId counter_unit,
                               GlobalArgsTracker::CompactArg* d_args,
                               uint32_t d_size,
                               const SetArgsCallback& args) {
  const auto* dims = blueprint.dimension_blueprints.data();
  for (uint32_t i = 0; i < d_size; ++i) {
    StringId key = context_->storage->InternString(
        base::StringView(dims[i].name.data(), dims[i].name.size()));
    d_args[i].key = key;
    d_args[i].flat_key = key;
  }

  tables::TrackTable::Row row(name);
  row.machine_id = context_->machine_id();
  row.classification = context_->storage->InternString(base::StringView(
      blueprint.classification.data(), blueprint.classification.size()));
  if (d_size > 0) {
    row.dimension_arg_set_id =
        context_->global_args_tracker->AddArgSet(d_args, 0, d_size);
  }
  row.event_type = context_->storage->InternString(blueprint.event_type);
  row.counter_unit = counter_unit;
  TrackId id = context_->storage->mutable_track_table()->Insert(row).id;
  if (args) {
    auto inserter = args_tracker_.AddArgsTo(id);
    args(inserter);
    args_tracker_.Flush();
  }
  return id;
}

void TrackTracker::MarkCpuValid(uint32_t cpu) {
  context_->cpu_tracker->MarkCpuValid(cpu);
}

}  // namespace perfetto::trace_processor
