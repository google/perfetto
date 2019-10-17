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

#include "src/trace_processor/track_tracker.h"

#include "src/trace_processor/args_tracker.h"

namespace perfetto {
namespace trace_processor {

// static
constexpr TrackId TrackTracker::kDefaultDescriptorTrackUuid;

TrackTracker::TrackTracker(TraceProcessorContext* context)
    : source_key_(context->storage->InternString("source")),
      source_id_key_(context->storage->InternString("source_id")),
      source_scope_key_(context->storage->InternString("source_scope")),
      fuchsia_source_(context->storage->InternString("fuchsia")),
      chrome_source_(context->storage->InternString("chrome")),
      android_source_(context->storage->InternString("android")),
      descriptor_source_(context->storage->InternString("descriptor")),
      default_descriptor_track_name_(
          context->storage->InternString("Default Track")),
      context_(context) {}

TrackId TrackTracker::InternThreadTrack(UniqueTid utid) {
  auto it = thread_tracks_.find(utid);
  if (it != thread_tracks_.end())
    return it->second;

  tables::ThreadTrackTable::Row row;
  row.utid = utid;
  auto id = context_->storage->mutable_thread_track_table()->Insert(row);
  thread_tracks_[utid] = id;
  return id;
}

TrackId TrackTracker::InternFuchsiaAsyncTrack(StringId name,
                                              int64_t correlation_id) {
  auto it = fuchsia_async_tracks_.find(correlation_id);
  if (it != fuchsia_async_tracks_.end())
    return it->second;

  tables::TrackTable::Row row(name);
  auto id = context_->storage->mutable_track_table()->Insert(row);
  fuchsia_async_tracks_[correlation_id] = id;

  RowId row_id = TraceStorage::CreateRowId(TableId::kTrack, id);
  context_->args_tracker->AddArg(row_id, source_key_, source_key_,
                                 Variadic::String(fuchsia_source_));
  context_->args_tracker->AddArg(row_id, source_id_key_, source_id_key_,
                                 Variadic::Integer(correlation_id));
  return id;
}

TrackId TrackTracker::InternGpuTrack(const tables::GpuTrackTable::Row& row) {
  GpuTrackTuple tuple{row.name.id, row.scope, row.context_id.value_or(0)};

  auto it = gpu_tracks_.find(tuple);
  if (it != gpu_tracks_.end())
    return it->second;

  auto id = context_->storage->mutable_gpu_track_table()->Insert(row);
  gpu_tracks_[tuple] = id;
  return id;
}

TrackId TrackTracker::InternLegacyChromeAsyncTrack(
    StringId name,
    uint32_t upid,
    int64_t source_id,
    bool source_id_is_process_scoped,
    StringId source_scope) {
  ChromeTrackTuple tuple;
  if (source_id_is_process_scoped)
    tuple.upid = upid;
  tuple.source_id = source_id;
  tuple.source_scope = source_scope;

  auto it = chrome_tracks_.find(tuple);
  if (it != chrome_tracks_.end())
    return it->second;

  // Legacy async tracks are always drawn in the context of a process, even if
  // the ID's scope is global.
  tables::ProcessTrackTable::Row track(name);
  track.upid = upid;
  TrackId id = context_->storage->mutable_process_track_table()->Insert(track);
  chrome_tracks_[tuple] = id;

  RowId row_id = TraceStorage::CreateRowId(TableId::kTrack, id);
  context_->args_tracker->AddArg(row_id, source_key_, source_key_,
                                 Variadic::String(chrome_source_));
  context_->args_tracker->AddArg(row_id, source_id_key_, source_id_key_,
                                 Variadic::Integer(source_id));
  context_->args_tracker->AddArg(row_id, source_scope_key_, source_scope_key_,
                                 Variadic::String(source_scope));
  return id;
}

TrackId TrackTracker::InternAndroidAsyncTrack(StringId name,
                                              UniquePid upid,
                                              int64_t cookie) {
  AndroidAsyncTrackTuple tuple{upid, cookie, name};

  auto it = android_async_tracks_.find(tuple);
  if (it != android_async_tracks_.end())
    return it->second;

  tables::ProcessTrackTable::Row row(name);
  row.upid = upid;
  auto id = context_->storage->mutable_process_track_table()->Insert(row);
  android_async_tracks_[tuple] = id;

  RowId row_id = TraceStorage::CreateRowId(TableId::kTrack, id);
  context_->args_tracker->AddArg(row_id, source_key_, source_key_,
                                 Variadic::String(android_source_));
  context_->args_tracker->AddArg(row_id, source_id_key_, source_id_key_,
                                 Variadic::Integer(cookie));
  return id;
}

TrackId TrackTracker::InternLegacyChromeProcessInstantTrack(UniquePid upid) {
  auto it = chrome_process_instant_tracks_.find(upid);
  if (it != chrome_process_instant_tracks_.end())
    return it->second;

  tables::ProcessTrackTable::Row row;
  row.upid = upid;
  auto id = context_->storage->mutable_process_track_table()->Insert(row);
  chrome_process_instant_tracks_[upid] = id;
  return id;
}

TrackId TrackTracker::GetOrCreateLegacyChromeGlobalInstantTrack() {
  if (!chrome_global_instant_track_id_) {
    chrome_global_instant_track_id_ =
        context_->storage->mutable_track_table()->Insert({});
  }
  return *chrome_global_instant_track_id_;
}

TrackId TrackTracker::UpdateDescriptorTrack(uint64_t uuid,
                                            StringId name,
                                            base::Optional<UniquePid> upid,
                                            base::Optional<UniqueTid> utid) {
  auto it = descriptor_tracks_.find(uuid);
  if (it != descriptor_tracks_.end()) {
    // Update existing track for |uuid|.
    TrackId track_id = it->second;
    if (name != kNullStringId) {
      context_->storage->mutable_track_table()->mutable_name()->Set(track_id,
                                                                    name);
    }

#if PERFETTO_DLOG_IS_ON()
    if (upid) {
      // Verify that upid didn't change.
      auto process_track_row =
          context_->storage->process_track_table().id().IndexOf(
              SqlValue::Long(track_id));
      if (!process_track_row) {
        PERFETTO_DLOG("Can't update non-scoped track with uuid %" PRIu64
                      " to a scoped track.",
                      uuid);
      } else {
        auto old_upid =
            context_->storage->process_track_table().upid()[*process_track_row];
        if (old_upid != upid) {
          PERFETTO_DLOG("Ignoring upid change for track with uuid %" PRIu64
                        " from %" PRIu32 " to %" PRIu32 ".",
                        uuid, old_upid, *upid);
        }
      }
    }

    if (utid) {
      // Verify that utid didn't change.
      auto thread_track_row =
          context_->storage->thread_track_table().id().IndexOf(
              SqlValue::Long(track_id));
      if (!thread_track_row) {
        PERFETTO_DLOG("Can't update non-thread track with uuid %" PRIu64
                      " to a thread track.",
                      uuid);
      } else {
        auto old_utid =
            context_->storage->thread_track_table().utid()[*thread_track_row];
        if (old_utid != utid) {
          PERFETTO_DLOG("Ignoring utid change for track with uuid %" PRIu64
                        " from %" PRIu32 " to %" PRIu32 ".",
                        uuid, old_utid, *utid);
        }
      }
    }
#endif  // PERFETTO_DLOG_IS_ON()

    return track_id;
  }

  TrackId track_id;

  if (utid) {
    // Update existing track for the thread if we have previously created one
    // in GetOrCreateDescriptorTrackForThread().
    auto utid_it = descriptor_tracks_by_utid_.find(*utid);
    if (utid_it != descriptor_tracks_by_utid_.end()) {
      TrackId candidate_track_id = utid_it->second;
      // Only update this track if it hasn't been associated with a different
      // UUID already.
      auto descriptor_it = std::find_if(
          descriptor_tracks_.begin(), descriptor_tracks_.end(),
          [candidate_track_id](const std::pair<uint64_t, TrackId>& entry) {
            return entry.second == candidate_track_id;
          });
      if (descriptor_it == descriptor_tracks_.end()) {
        descriptor_tracks_[uuid] = candidate_track_id;

        RowId row_id =
            TraceStorage::CreateRowId(TableId::kTrack, candidate_track_id);
        context_->args_tracker->AddArg(
            row_id, source_id_key_, source_id_key_,
            Variadic::Integer(static_cast<int64_t>(uuid)));

        return candidate_track_id;
      }
    }

    // New thread track.
    tables::ThreadTrackTable::Row row(name);
    row.utid = *utid;
    track_id = context_->storage->mutable_thread_track_table()->Insert(row);
    if (descriptor_tracks_by_utid_.find(*utid) ==
        descriptor_tracks_by_utid_.end()) {
      descriptor_tracks_by_utid_[*utid] = track_id;
    }
  } else if (upid) {
    // New process-scoped async track.
    tables::ProcessTrackTable::Row track(name);
    track.upid = *upid;
    track_id = context_->storage->mutable_process_track_table()->Insert(track);
  } else {
    // New global async track.
    tables::TrackTable::Row track(name);
    track_id = context_->storage->mutable_track_table()->Insert(track);
  }

  descriptor_tracks_[uuid] = track_id;

  RowId row_id = TraceStorage::CreateRowId(TableId::kTrack, track_id);
  context_->args_tracker->AddArg(row_id, source_key_, source_key_,
                                 Variadic::String(descriptor_source_));
  context_->args_tracker->AddArg(row_id, source_id_key_, source_id_key_,
                                 Variadic::Integer(static_cast<int64_t>(uuid)));

  return track_id;
}

base::Optional<TrackId> TrackTracker::GetDescriptorTrack(uint64_t uuid) const {
  auto it = descriptor_tracks_.find(uuid);
  if (it == descriptor_tracks_.end())
    return base::nullopt;
  return it->second;
}

TrackId TrackTracker::GetOrCreateDescriptorTrackForThread(UniqueTid utid) {
  auto it = descriptor_tracks_by_utid_.find(utid);
  if (it != descriptor_tracks_by_utid_.end()) {
    return it->second;
  }
  // TODO(eseckler): How should this track receive its name?
  tables::ThreadTrackTable::Row row(/*name=*/kNullStringId);
  row.utid = utid;
  TrackId track_id =
      context_->storage->mutable_thread_track_table()->Insert(row);
  descriptor_tracks_by_utid_[utid] = track_id;

  RowId row_id = TraceStorage::CreateRowId(TableId::kTrack, track_id);
  context_->args_tracker->AddArg(row_id, source_key_, source_key_,
                                 Variadic::String(descriptor_source_));
  return track_id;
}

TrackId TrackTracker::GetOrCreateDefaultDescriptorTrack() {
  base::Optional<TrackId> opt_track_id =
      GetDescriptorTrack(kDefaultDescriptorTrackUuid);
  if (opt_track_id)
    return *opt_track_id;

  return UpdateDescriptorTrack(kDefaultDescriptorTrackUuid,
                               default_descriptor_track_name_);
}

}  // namespace trace_processor
}  // namespace perfetto
