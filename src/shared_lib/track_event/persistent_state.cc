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
#include "perfetto/public/abi/track_event_hl_abi.h"
#include "perfetto/tracing/internal/track_event_internal.h"
#include "src/shared_lib/track_event/category_impl.h"
#include "src/shared_lib/track_event/hl.h"

struct PerfettoTePersistentStateTrackImpl {
  struct State {
    PerfettoTeTimestamp start;
    // Serialized perfetto.protos.TrackEvent fields (value and args).
    std::vector<uint8_t> fields;
  };

  PerfettoTeCategoryImpl* cat = nullptr;
  // Copy of the track descriptor: `track.descriptor` points to it.
  std::vector<uint8_t> descriptor;
  PerfettoTeRegisteredTrackImpl track{};
  // Owned. Only accessed by whoever took it out of here (with an exchange),
  // never in place.
  std::atomic<State*> state{nullptr};
};

namespace perfetto::shlib {
namespace {

using Track = PerfettoTePersistentStateTrackImpl;
using State = Track::State;

// All the tracks.
struct Registry {
  static Registry& Get() {
    static Registry* instance = new Registry();
    return *instance;
  }
  std::mutex mu;
  std::vector<Track*> tracks PERFETTO_GUARDED_BY(mu);
};

// Emits the state `s` of the track `t` on the data source instances in
// `instance_mask`.
void EmitState(const Track& t, const State& s, uint32_t instance_mask) {
  PerfettoTeHlExtraRegisteredTrack track{
      {PERFETTO_TE_HL_EXTRA_TYPE_REGISTERED_TRACK}, &t.track};
  PerfettoTeHlExtraTimestamp ts{{PERFETTO_TE_HL_EXTRA_TYPE_TIMESTAMP}, s.start};
  PerfettoTeHlExtra* extras[] = {&track.header, &ts.header, nullptr};
  TeHlEmit(t.cat, PERFETTO_TE_TYPE_STATE, /*name=*/nullptr, extras,
           instance_mask, {s.fields.data(), s.fields.size()});
}

// Returns true if extras of type `type` are part of the state.
bool IsPersistedExtra(uint32_t type) {
  switch (type) {
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL:
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_UINT64:
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64:
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE:
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING:
    case PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_POINTER:
    case PERFETTO_TE_HL_EXTRA_TYPE_PROTO_FIELDS:
      return true;
  }
  return false;
}

}  // namespace

void EmitPersistentStates(uint32_t instance_index) {
  const uint32_t bit = 1u << instance_index;
  Registry& registry = Registry::Get();
  std::lock_guard<std::mutex> lock(registry.mu);
  for (Track* t : registry.tracks) {
    if (!(t->cat->instances.load(std::memory_order_relaxed) & bit)) {
      continue;
    }
    // Take the state out while emitting it. A concurrent Set() will see no
    // previous state.
    std::unique_ptr<State> s(t->state.exchange(nullptr));
    if (!s) {
      continue;
    }
    EmitState(*t, *s, bit);
    State* expected = nullptr;
    if (t->state.compare_exchange_strong(expected, s.get())) {
      s.release();
      continue;
    }
    // A Set() replaced the state in the meantime, without emitting it as the
    // previous state: do it on its behalf (it was already emitted on `bit`).
    EmitState(*t, *s, ~bit);
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
  {
    auto& registry = perfetto::shlib::Registry::Get();
    std::lock_guard<std::mutex> lock(registry.mu);
    registry.tracks.erase(
        std::remove(registry.tracks.begin(), registry.tracks.end(), t),
        registry.tracks.end());
  }
  delete t->state.load();
  delete t;
}

void PerfettoTePersistentStateTrackImplSet(
    struct PerfettoTePersistentStateTrackImpl* t,
    const char* value,
    struct PerfettoTeHlExtra* const* extra_data) {
  using perfetto::shlib::EmitState;
  using perfetto::shlib::State;
  PERFETTO_DCHECK(value);
  PerfettoTeHlExtraTimestamp ts{{PERFETTO_TE_HL_EXTRA_TYPE_TIMESTAMP}, {}};
  bool has_ts = false;
  perfetto::base::SmallVector<PerfettoTeHlExtra*, 8> persisted;
  perfetto::base::SmallVector<PerfettoTeHlExtra*, 8> all;
  for (auto* const* it = extra_data; *it != nullptr; it++) {
    if ((*it)->type == PERFETTO_TE_HL_EXTRA_TYPE_TIMESTAMP) {
      ts = *reinterpret_cast<const PerfettoTeHlExtraTimestamp*>(*it);
      has_ts = true;
    } else if (perfetto::shlib::IsPersistedExtra((*it)->type)) {
      persisted.emplace_back(*it);
    }
    all.emplace_back(*it);
  }
  persisted.emplace_back(nullptr);
  if (!has_ts) {
    perfetto::TraceTimestamp now =
        perfetto::internal::TrackEventInternal::GetTraceTime();
    ts.timestamp = {now.clock_id, now.value};
  }

  std::unique_ptr<State> prev(t->state.exchange(new State{
      ts.timestamp,
      perfetto::shlib::SerializeTeHlEventFields(value, persisted.data())}));
  // The previous state ends now: emit it (with the timestamp it started at)
  // before the new one.
  if (prev) {
    EmitState(*t, *prev, ~0u);
  }

  // Emit the new state from the original extras (it's not safe to access the
  // new state after publishing it).
  PerfettoTeHlExtraRegisteredTrack track{
      {PERFETTO_TE_HL_EXTRA_TYPE_REGISTERED_TRACK}, &t->track};
  all.emplace_back(&track.header);
  all.emplace_back(&ts.header);
  all.emplace_back(nullptr);
  perfetto::shlib::TeHlEmit(t->cat, PERFETTO_TE_TYPE_STATE, value, all.data(),
                            ~0u, /*raw_fields=*/{});
}
