/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/shared_lib/track_event/persistent_state.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/thread_annotations.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/public/abi/track_event_abi.h"
#include "perfetto/tracing/internal/track_event_internal.h"
#include "src/shared_lib/track_event/category_impl.h"
#include "src/shared_lib/track_event/hl.h"

struct PerfettoTePersistentStateTrackImpl {
  // The state of the track, valid only in the category epoch it was set in.
  struct Snapshot {
    uint32_t epoch;
    PerfettoTeTimestamp start;
    // Serialized perfetto.protos.TrackEvent fields (name and extra args).
    std::vector<uint8_t> fields;
  };

  PerfettoTeCategoryImpl* cat = nullptr;
  std::vector<uint8_t> descriptor;
  PerfettoTeRegisteredTrackImpl track{};
  // Owned. A snapshot can only be accessed (or freed) by whoever took it out of
  // here (with an exchange or a compare_exchange), never in place.
  std::atomic<Snapshot*> snapshot{nullptr};
  // Bitmask of the data source instances where the state changed since their
  // last flush.
  std::atomic<uint8_t> dirty{0};
};

namespace perfetto::shlib {
namespace {

using Track = PerfettoTePersistentStateTrackImpl;
using Snapshot = Track::Snapshot;

struct Registry {
  static Registry& Get() {
    static Registry* instance = new Registry();
    return *instance;
  }
  std::mutex mu;
  std::vector<Track*> tracks PERFETTO_GUARDED_BY(mu);
};

// Returns true if `s` belongs to the current epoch of `t->cat` and the
// category is still enabled.
bool IsCurrent(const Track& t, const Snapshot& s) {
  return s.epoch == t.cat->epoch.load(std::memory_order_seq_cst) &&
         t.cat->instances.load(std::memory_order_seq_cst) != 0;
}

void EmitSnapshot(const Track& t,
                  const Snapshot& s,
                  uint32_t instance_mask,
                  bool flush = false) {
  PerfettoTeHlExtraRegisteredTrack track{
      {PERFETTO_TE_HL_EXTRA_TYPE_REGISTERED_TRACK}, &t.track};
  PerfettoTeHlExtraTimestamp ts{{PERFETTO_TE_HL_EXTRA_TYPE_TIMESTAMP}, s.start};
  TeHlExtraRawFields fields{
      {kTeHlExtraTypeRawFields}, s.fields.data(), s.fields.size()};
  PerfettoTeHlExtra flush_extra{PERFETTO_TE_HL_EXTRA_TYPE_FLUSH};
  PerfettoTeHlExtra* extras[] = {&track.header, &ts.header, &fields.header,
                                 flush ? &flush_extra : nullptr, nullptr};
  TeHlEmit(t.cat, PERFETTO_TE_TYPE_STATE, /*name=*/nullptr, extras,
           instance_mask);
}

// Called after a snapshot has been installed concurrently with the category
// being disabled (and maybe re-enabled): frees the snapshot of `t` unless it
// belongs to the current epoch.
void ReclaimIfStale(Track* t) {
  std::unique_ptr<Snapshot> s(
      t->snapshot.exchange(nullptr, std::memory_order_seq_cst));
  if (s && IsCurrent(*t, *s)) {
    Snapshot* expected = nullptr;
    if (t->snapshot.compare_exchange_strong(expected, s.get(),
                                            std::memory_order_seq_cst)) {
      s.release();
    }
  }
}

}  // namespace

void PersistentStateTrackUpdate(
    struct PerfettoTeCategoryImpl* cat,
    const struct PerfettoTeHlExtraPersistentStateTrack& extra,
    const char* name,
    struct PerfettoTeHlExtra* const* extra_data) {
  Track* t = extra.track;
  PERFETTO_DCHECK(t->cat == cat);
  // seq_cst pairs with DisableInstance() and PersistentStateTracksOnStop().
  if (!cat->instances.load(std::memory_order_seq_cst)) {
    return;
  }
  const uint32_t epoch = cat->epoch.load(std::memory_order_seq_cst);

  TraceTimestamp now = internal::TrackEventInternal::GetTraceTime();
  PerfettoTeTimestamp start{now.clock_id, now.value};
  bool flush = false;
  base::SmallVector<PerfettoTeHlExtra*, 8> field_extras;
  for (auto* const* it = extra_data; *it != nullptr; it++) {
    switch ((*it)->type) {
      case PERFETTO_TE_HL_EXTRA_TYPE_TIMESTAMP:
        start =
            reinterpret_cast<const PerfettoTeHlExtraTimestamp*>(*it)->timestamp;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_UINT64:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING:
      case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_POINTER:
      case PERFETTO_TE_HL_EXTRA_TYPE_PROTO_FIELDS:
        field_extras.emplace_back(*it);
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_FLUSH:
        flush = true;
        break;
      case PERFETTO_TE_HL_EXTRA_TYPE_PERSISTENT_STATE_TRACK:
      case PERFETTO_TE_HL_EXTRA_TYPE_NO_INTERN:
        break;
      default:
        // Other tracks, flows, correlation ids, ... are not supported: they
        // would be duplicated every time the state is re-emitted.
        PERFETTO_DFATAL("Unsupported extra type %u on persistent state track",
                        (*it)->type);
        break;
    }
  }
  field_extras.emplace_back(nullptr);

  auto* s = new Snapshot{epoch, start,
                         SerializeTeHlEventFields(name, field_extras.data())};
  Snapshot* old = nullptr;
  if (extra.is_init) {
    if (!t->snapshot.compare_exchange_strong(old, s,
                                             std::memory_order_seq_cst)) {
      delete s;  // The track already has a state.
      return;
    }
  } else {
    old = t->snapshot.exchange(s, std::memory_order_seq_cst);
  }
  std::unique_ptr<Snapshot> old_owner(old);
  t->dirty.store(0xff, std::memory_order_seq_cst);

  if (cat->epoch.load(std::memory_order_seq_cst) != epoch ||
      !cat->instances.load(std::memory_order_seq_cst)) {
    // The last data source instance with `cat` enabled has stopped in the
    // meantime: `s` might have been installed after PersistentStateTracksOnStop
    // and must not leak (or leak into the next tracing session).
    ReclaimIfStale(t);
    return;
  }
  if (old && old->epoch == epoch) {
    // The previous state ends now: emit it on all the active instances.
    EmitSnapshot(*t, *old, ~0u, flush);
  }
}

void PersistentStateTracksFlush(uint32_t instance_index, bool all) {
  const auto bit = static_cast<uint8_t>(1u << instance_index);
  Registry& registry = Registry::Get();
  std::lock_guard<std::mutex> lock(registry.mu);
  for (Track* t : registry.tracks) {
    if (!(t->cat->instances.load(std::memory_order_relaxed) & bit)) {
      continue;
    }
    bool was_dirty = t->dirty.fetch_and(static_cast<uint8_t>(~bit),
                                        std::memory_order_seq_cst) &
                     bit;
    if (!all && !was_dirty) {
      continue;
    }
    std::unique_ptr<Snapshot> s(
        t->snapshot.exchange(nullptr, std::memory_order_seq_cst));
    if (!s || !IsCurrent(*t, *s)) {
      continue;
    }
    EmitSnapshot(*t, *s, bit);
    Snapshot* expected = nullptr;
    if (t->snapshot.compare_exchange_strong(expected, s.get(),
                                            std::memory_order_seq_cst)) {
      s.release();
      continue;
    }
    // The state was replaced while we were holding it, so its writer didn't
    // emit it as the previous state: do it on its behalf.
    EmitSnapshot(*t, *s, static_cast<uint8_t>(~bit));
  }
}

void PersistentStateTracksOnStop() {
  Registry& registry = Registry::Get();
  std::lock_guard<std::mutex> lock(registry.mu);
  for (Track* t : registry.tracks) {
    if (t->cat->instances.load(std::memory_order_seq_cst) == 0) {
      delete t->snapshot.exchange(nullptr, std::memory_order_seq_cst);
      t->dirty.store(0, std::memory_order_relaxed);
    }
  }
}

}  // namespace perfetto::shlib

struct PerfettoTePersistentStateTrackImpl*
PerfettoTePersistentStateTrackImplCreate(
    struct PerfettoTeCategoryImpl* cat,
    const struct PerfettoTeRegisteredTrackImpl* track) {
  // Dynamic categories are not supported.
  PERFETTO_DCHECK(cat && cat != perfetto_te_any_categories);
  auto* t = new PerfettoTePersistentStateTrackImpl;
  t->cat = cat;
  const auto* desc = static_cast<const uint8_t*>(track->descriptor);
  t->descriptor.assign(desc, desc + track->descriptor_size);
  t->track = {t->descriptor.data(), t->descriptor.size(), track->uuid};

  auto& registry = perfetto::shlib::Registry::Get();
  std::lock_guard<std::mutex> lock(registry.mu);
  registry.tracks.push_back(t);
  return t;
}

void PerfettoTePersistentStateTrackImplDestroy(
    struct PerfettoTePersistentStateTrackImpl* t) {
  auto& registry = perfetto::shlib::Registry::Get();
  {
    std::lock_guard<std::mutex> lock(registry.mu);
    registry.tracks.erase(
        std::remove(registry.tracks.begin(), registry.tracks.end(), t),
        registry.tracks.end());
  }
  delete t->snapshot.exchange(nullptr);
  delete t;
}
