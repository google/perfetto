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

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/tracks_internal.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

TrackTracker::TrackTracker(TraceProcessorContext* context)
    : context_(context),
      args_tracker_(context),
      description_key_id_(context->storage->InternString("description")),
      machine_dimension_id_(context->storage->InternString("machine")),
      process_dimension_id_(context->storage->InternString("process")),
      thread_dimension_id_(context->storage->InternString("thread")),
      cpu_dimension_id_(context->storage->InternString("cpu")),
      gpu_dimension_id_(context->storage->InternString("gpu")) {}

TrackId TrackTracker::AddTrack(const tracks::BlueprintBase& blueprint,
                               StringId name,
                               StringId counter_unit,
                               StringId description,
                               GlobalArgsTracker::CompactArg* d_args,
                               uint32_t d_size,
                               const SetArgsCallback& args) {
  tables::TrackTable::Row row(name);
  // The canonical (machine aware) `ucpu`/`ugpu` identities win over the raw
  // `cpu`/`gpu` numbers when a track has both.
  std::optional<int64_t> cpu;
  std::optional<int64_t> ucpu;
  std::optional<int64_t> gpu;
  std::optional<int64_t> ugpu;
  const auto* dims = blueprint.dimension_blueprints.data();
  for (uint32_t i = 0; i < d_size; ++i) {
    base::StringView str(dims[i].name.data(), dims[i].name.size());
    bool is_int = d_args[i].value.type == Variadic::kInt;
    int64_t int_value = is_int ? d_args[i].value.int_value : 0;
    if (str == "cpu" && is_int) {
      context_->cpu_tracker->MarkCpuValid(static_cast<uint32_t>(int_value));
      cpu = int_value;
    } else if (str == "ucpu" && is_int) {
      ucpu = int_value;
    } else if (str == "gpu" && is_int) {
      gpu = int_value;
    } else if (str == "ugpu" && is_int) {
      ugpu = int_value;
    } else if (str == "utid" && is_int) {
      row.utid = static_cast<uint32_t>(int_value);
    } else if (str == "upid" && is_int) {
      row.upid = static_cast<uint32_t>(int_value);
    }
    StringId key = context_->storage->InternString(str);
    d_args[i].key = key;
    d_args[i].flat_key = key;
  }

  row.machine_id = context_->machine_id();
  row.type = context_->storage->InternString(
      base::StringView(blueprint.type.data(), blueprint.type.size()));
  if (d_size > 0) {
    row.dimension_arg_set_id =
        context_->global_args_tracker->AddArgSet(d_args, 0, d_size);
  }
  row.event_type = context_->storage->InternString(blueprint.event_type);
  row.counter_unit = counter_unit;
  TrackId id = context_->storage->mutable_track_table()->Insert(row).id;

  // Well known dimensions are written for every track as it is created, so
  // that `track_dimension` is a plain table rather than synthesized at query
  // time. Note that tracks only associated with a thread don't get a
  // `process` dimension: the process of a thread is often only known later in
  // the trace, so it is reached through `thread.upid` instead.
  AddWellKnownDimension(id, machine_dimension_id_, row.machine_id.value);
  if (row.upid) {
    AddWellKnownDimension(id, process_dimension_id_, *row.upid);
  }
  if (row.utid) {
    AddWellKnownDimension(id, thread_dimension_id_, *row.utid);
  }
  if (ucpu || cpu) {
    AddWellKnownDimension(id, cpu_dimension_id_, ucpu ? *ucpu : *cpu);
  }
  if (ugpu || gpu) {
    AddWellKnownDimension(id, gpu_dimension_id_, ugpu ? *ugpu : *gpu);
  }

  if (description != kNullStringId || args) {
    auto inserter = args_tracker_.AddArgsTo(id);
    if (description != kNullStringId) {
      inserter.AddArg(description_key_id_, Variadic::String(description));
    }
    if (args) {
      args(inserter);
    }
  }
  return id;
}

void TrackTracker::AddWellKnownDimension(TrackId track,
                                         StringId name,
                                         int64_t value) {
  tables::TrackDimensionTable::Row row;
  row.track_id = track;
  row.name = name;
  row.int_value = value;
  row.is_well_known = 1;
  context_->storage->mutable_track_dimension_table()->Insert(row);
}

}  // namespace perfetto::trace_processor
