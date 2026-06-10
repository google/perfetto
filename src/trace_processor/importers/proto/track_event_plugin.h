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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PLUGIN_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PLUGIN_H_

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/proto_decoder.h"

namespace perfetto::trace_processor {

// Parses an extension field nested inside a TrackEvent. Registered against one
// or more TrackEvent field ids; ParseField runs for each matching field.
class TrackEventPlugin {
 public:
  virtual ~TrackEventPlugin();
  virtual void ParseField(uint32_t field_id,
                          protozero::ConstBytes data,
                          int64_t ts) = 0;
};

// Field-id-keyed dispatch to TrackEventPlugins. A no-op until something
// registers, so an empty registry costs one check per event.
class TrackEventPluginRegistry {
 public:
  // Registers `plugin` to handle each id in `field_ids`. Takes ownership.
  void Register(std::unique_ptr<TrackEventPlugin> plugin,
                std::initializer_list<uint32_t> field_ids) {
    TrackEventPlugin* p = plugin.get();
    plugins_.push_back(std::move(plugin));
    for (uint32_t id : field_ids)
      handlers_.Insert(id, p);
  }

  void ParseFields(protozero::ConstBytes event_bytes, int64_t ts) const {
    if (handlers_.size() == 0)
      return;
    protozero::ProtoDecoder decoder(event_bytes);
    for (auto f = decoder.ReadField(); f.valid(); f = decoder.ReadField()) {
      if (auto* h = handlers_.Find(f.id()))
        (*h)->ParseField(f.id(), f.as_bytes(), ts);
    }
  }

 private:
  base::FlatHashMap<uint32_t, TrackEventPlugin*> handlers_;
  std::vector<std::unique_ptr<TrackEventPlugin>> plugins_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_PLUGIN_H_
